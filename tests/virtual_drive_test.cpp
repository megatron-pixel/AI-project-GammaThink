#include "virtual_drive.hpp"

#include <algorithm>
#include <iostream>
#include <unistd.h>

namespace {
using namespace axiom::knowledge;
std::size_t assertions = 0;
void check(bool condition) {
    ++assertions;
    if (!condition) throw std::runtime_error("Virtual drive assertion " + std::to_string(assertions));
}
template<class Function> void rejects(ErrorCode expected, Function function) {
    try { function(); }
    catch (const Error& error) { check(error.code == expected); return; }
    throw std::runtime_error("Expected virtual drive rejection");
}
struct Directory {
    std::filesystem::path path;
    Directory() {
        auto pattern = (std::filesystem::temp_directory_path() / "gammathink-drive-XXXXXX").string();
        std::vector<char> value(pattern.begin(), pattern.end()); value.push_back(0);
        const auto* made = mkdtemp(value.data());
        if (!made) throw std::runtime_error("Cannot create virtual drive fixture");
        path = made;
    }
    ~Directory() { std::filesystem::remove_all(path); }
};
Object document(std::size_t number) {
    Object result;
    result.label = "document " + std::to_string(number);
    result.kind = number % 5 ? ObjectKind::Claim : ObjectKind::Evidence;
    result.format = "text/plain; charset=utf-8";
    const std::array<std::string, 6> texts{"Alpha beta gamma", "alpha gap beta gamma",
        "beta alpha beta", "A \"quoted\" term alpha", "caf\xc3\xa9 CAFE delta", "!!!"};
    const auto& text = texts[number % texts.size()];
    result.payload.assign(text.begin(), text.end());
    result.concepts = {number % 2 ? "Heat" : "heat", "duplicate", "duplicate", ""};
    result.sources = {{"library", number % 3 ? "v1" : "v2", "A/Entry", "synthetic-v1", {}, 0, text.size()}};
    if (number % 4 == 0) result.sources.push_back({"library", "", "B/Entry", "synthetic-v1", {}, 0, 0});
    return result;
}
void compare(KnowledgeArchive& expected, KnowledgeArchive& actual, const KnowledgeQuery& query, std::size_t limit) {
    std::optional<ObjectId> cursor;
    std::size_t pages = 0;
    do {
        check(++pages < 1000);
        const auto a = expected.query(query, cursor, limit);
        const auto b = actual.query(query, cursor, limit);
        check(a.objects == b.objects && a.complete == b.complete && a.next == b.next);
        check(a.complete || !a.objects.empty());
        cursor = a.next;
    } while (cursor);
}
}
int main() {
    try {
        Directory directory;
        Journal source(":memory:"), local(directory.path / "local.sqlite");
        std::vector<ObjectId> ids;
        for (std::size_t i = 0; i < 36; ++i) {
            auto object = document(i);
            if (i) object.links.push_back({Relationship::Requires, ids.front()});
            if (i % 7 == 0 && i) object.links.push_back({Relationship::Contradicts, ids.front()});
            ids.push_back(source.put(object));
        }
        auto derivation = document(60);
        derivation.kind = ObjectKind::Derivation;
        derivation.derivation = Derivation{ids[0], ids[1], {ids[3], ids[2], ids[3]}, {ids[4]}, {ids[5]}};
        const auto derivation_id = source.put(derivation);
        Assessment large;
        large.checker = "synthetic-checker";
        large.checked_dependencies.assign(maximum_assessment_dependencies, ids.front());
        const auto large_observed = source.record_assessment(ids[1], large);
        SnapshotLimits compile_limits; compile_limits.chunk_records = 3; compile_limits.chunk_bytes = 1024;
        const auto first_path = directory.path / "v1";
        const auto first = compile_snapshot(source, first_path, "library", "r1", compile_limits);
        JournalArchive expected(source);
        GksArchive actual(first_path, first.root);
        std::vector<KnowledgeQuery> queries(1);
        KnowledgeQuery query;
        query.kind = QueryKind::Kind;
        for (const auto kind : {ObjectKind::Claim, ObjectKind::Evidence, ObjectKind::Derivation, ObjectKind::Rule}) {
            query.object_kind = kind; queries.push_back(query);
        }
        query = {}; query.kind = QueryKind::Concept;
        for (const auto* tag : {"heat", "Heat", "duplicate", "", "absent"}) {
            query.value = tag; queries.push_back(query);
        }
        query = {}; query.kind = QueryKind::Source; query.value = "library"; queries.push_back(query);
        for (const auto* revision : {"v1", "v2", "", "missing"}) {
            query.source_version = revision; queries.push_back(query);
        }
        query = {}; query.kind = QueryKind::Text;
        for (const auto* phrase : {"alpha", "alpha beta", "alpha beta gamma", "beta alpha beta",
                "alpha alpha", "beta gamma", "quoted term", "\"quoted\" term",
                "CAFE", "cafe cafe", "caf\xc3\xa9 cafe", "absent", "!!!", "OR 1=1 --"}) {
            query.value = phrase; queries.push_back(query);
        }
        query = {}; query.kind = QueryKind::Related; query.target = ids.front();
        for (const auto relationship : {Relationship::Requires, Relationship::Contradicts, Relationship::Supports}) {
            query.relationship = relationship; queries.push_back(query);
        }
        query = {}; query.kind = QueryKind::RequiredBy;
        for (const auto& target : {ids.front(), ids[1], ids[3], ids[4], derivation_id}) {
            query.target = target; queries.push_back(query);
        }
        for (const auto& request : queries) {
            compare(expected, actual, request, 2);
            compare(expected, actual, request, 64);
        }
        check(actual.get(derivation_id)->object == derivation);
        check(actual.get(ids[1])->assessment == large_observed);

        ArchiveRegistry registry(directory.path / "catalogue.sqlite");
        registry.register_snapshot(first_path, first.root);
        auto mount = registry.mount("library", "library", "r1", first_path, 0);
        const auto shadow = *source.get(ids.front());
        local.put(shadow);
        Assessment own; own.checker = "local-checker"; own.logical = LogicalValidity::Valid;
        own.state = KnowledgeState::Verified;
        const auto own_observed = local.record_assessment(ids.front(), own);
        auto goal = document(90); goal.label = "local goal"; goal.links = {{Relationship::Requires, ids.back()}};
        const auto goal_id = local.put(goal);
        const auto goal_observed = local.record_assessment(goal_id, own);
        VirtualDrive drive(local, registry);
        auto pinned = drive.open_view();
        check(pinned->mounts().size() == 1 && pinned->mounts().front() == mount);
        const auto overlay = pinned->knowledge().get(ids.front());
        check(overlay && overlay->archive == 0 && overlay->record.assessment == own_observed);
        check(pinned->knowledge().get(ids[1])->record.assessment == large_observed);
        check(pinned->knowledge().resolve(goal_id).complete());
        KnowledgeStream old_stream(pinned->knowledge(), {}, {2, 64, 4096, 16 * 1024 * 1024});
        check(old_stream.next().has_value());
        const auto detached = registry.unmount("library", mount.generation);
        auto offline = drive.open_view();
        const auto missing = offline->knowledge().resolve(goal_id);
        check(!missing.complete() && missing.unavailable == std::vector<ObjectId>{ids.back()});
        check(local.assessment(goal_id) == goal_observed && local.get(goal_id) == goal);
        check(pinned->knowledge().resolve(goal_id).complete());
        while (old_stream.next()) {}
        check(!old_stream.next());
        const auto added = source.put(document(101));
        const auto second_path = directory.path / "v2";
        const auto second = compile_snapshot(source, second_path, "library", "r2", compile_limits);
        registry.register_snapshot(second_path, second.root);
        registry.mount("library", "library", "r2", second_path, detached.generation);
        auto current = drive.open_view();
        check(current->knowledge().get(added).has_value() && !pinned->knowledge().get(added));
        check(current->knowledge().resolve(goal_id).complete());
        check(current->mounts().front().binding->archive.root == second.root);
        auto forged = current->mounts(); forged.front().binding->archive.revision = "forged";
        rejects(ErrorCode::Integrity, [&] { DriveView bad(local, forged); });
        auto excessive = current->mounts();
        while (excessive.size() < 16) excessive.push_back(excessive.front());
        rejects(ErrorCode::Budget, [&] { DriveView bad(local, excessive); });

        for (unsigned mode = 0; mode < 4; ++mode) {
            IndexQueryLimits limits;
            if (mode == 0) limits.nodes = 1;
            if (mode == 1) limits.entries = 1;
            if (mode == 2) limits.stored_bytes = 1;
            if (mode == 3) limits.work = 1;
            GksArchive limited(first_path, first.root, limits);
            rejects(ErrorCode::Budget, [&] { limited.query({}, {}, 64); });
        }
        IndexQueryLimits limits; limits.phrase_tokens = 1;
        GksArchive token_limited(first_path, first.root, limits);
        query = {}; query.kind = QueryKind::Text; query.value = "alpha beta";
        rejects(ErrorCode::Budget, [&] { token_limited.query(query, {}, 64); });
        rejects(ErrorCode::Budget, [&] { actual.query({}, {}, 0); });
        query.kind = static_cast<QueryKind>(999);
        rejects(ErrorCode::Schema, [&] { actual.query(query, {}, 64); });
        GksSnapshot stored(first_path, first.root);
        const auto concept_root = std::find_if(stored.manifest().indexes.begin(), stored.manifest().indexes.end(),
            [](const auto& value) { return value.kind == IndexKind::Concept; });
        check(concept_root != stored.manifest().indexes.end());
        std::filesystem::remove(first_path / (hex(concept_root->block) + ".gksb"));
        query = {}; query.kind = QueryKind::Concept; query.value = "Heat";
        rejects(ErrorCode::Unavailable, [&] { actual.query(query, {}, 64); });
        check(actual.get(ids.front()).has_value());
        std::cout << "virtual drive tests passed: " << assertions << " deterministic assertions\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
