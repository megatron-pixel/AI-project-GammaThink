#include "source_bindings.hpp"
#include "virtual_drive.hpp"

#include <iostream>
#include <source_location>
#include <unistd.h>

namespace {
using namespace axiom;
using namespace axiom::knowledge;
std::size_t checks = 0;
void check(bool value, const std::source_location where = std::source_location::current()) {
    ++checks;
    if (!value) throw std::runtime_error("Source binding assertion at " + std::to_string(where.line()));
}
template<class F> void rejects(ErrorCode code, F&& operation) {
    ++checks;
    try { operation(); }
    catch (const Error& error) { if (error.code == code) return; throw; }
    throw std::runtime_error("Expected source binding rejection");
}
struct Directory {
    std::filesystem::path path;
    Directory() {
        auto pattern = (std::filesystem::temp_directory_path() / "source-bindings-XXXXXX").string();
        const auto* created = mkdtemp(pattern.data());
        if (!created) throw std::runtime_error("Cannot create source binding fixture");
        path = created;
    }
    ~Directory() { std::filesystem::remove_all(path); }
};
SourceBindingScope scope(std::string archive, std::string entity = "cart", std::uint32_t arity = 1,
    ProblemDimension dimension = {0,1,-1,0,0,0,0}, std::string run = "one") {
    const std::string text = entity + " has exactly 7 u in run " + run + ".";
    Object source;
    source.kind = ObjectKind::Evidence; source.visibility = Visibility::Public;
    source.label = "Synthetic binding source"; source.format = "source.zim.raw.v1:text/plain";
    source.payload.assign(text.begin(), text.end());
    source.sources = {{archive, "1", "A/value", "libzim-raw-entry-v1", digest(source.payload), 0, text.size()}};
    ProblemContext host;
    host.unit_registry_id = archive; host.unit_registry_version = "1";
    host.units = {{"unit", "u", dimension, Rational(1), {"u"}}};
    host.views = {make_evidence_view("source", PremiseBasis::Source, text,
        archive, "A/value", "libzim-raw-entry-v1")};
    const auto support = "[[0,0," + std::to_string(text.size()) + ",null]]";
    const auto dsl = "GAMMATHINK_DSL 1\nENTITY \"" + entity + "\"\nPREDICATE [\"available\","
        + std::to_string(arity) + "]\nQUANTITY [\"speed\",0,[true,[[\"run\",\"" + run + "\"]]],0]\n"
        "NUMBER [[0," + std::to_string(text.find('7')) + ",1,null],[0,"
        + std::to_string(text.find(" u") + 1) + ",1,null],0,\"exact\",[0,"
        + std::to_string(text.find("exactly")) + ",7,null]]\nBINDING [0,0," + support + "]\nEND\n";
    const auto candidate = *compile_source(source, identify(source), dsl).candidate;
    const auto compiled = materialize_source(source, candidate, host);
    return {source, compiled.module, compiled.context, host};
}
SourceIdentityBinding binding(const SourceBindingScope& left, const SourceBindingScope& right, SourceSymbolKind kind) {
    return {{identify(left.module), kind, 0}, {identify(right.module), kind, 0}};
}
}

int main() {
    try {
        Directory directory;
        const auto left = scope("archive:left");
        const auto right = scope("archive:right", "vehicle");
        const std::vector<SourceBindingScope> scopes{left, right};
        const std::vector<SourceIdentityBinding> rows{
            binding(left, right, SourceSymbolKind::Entity),
            binding(left, right, SourceSymbolKind::Predicate),
            binding(left, right, SourceSymbolKind::Quantity)};
        const auto object = propose_source_bindings(scopes, rows, "Explicit test assumption, not empirical identity");
        const auto id = identify(object);
        check(object.kind == ObjectKind::Assumption && object.visibility == Visibility::Private && !object.derivation);
        check(object.sources.size() == 2 && object.links.size() == 2);
        check(decode_object(id, encode_object(object)) == object);
        check(propose_source_bindings(std::vector<SourceBindingScope>{right, left}, rows, object.label) == object);
        rejects(ErrorCode::Schema, [&] {
            (void)propose_source_bindings(scopes, std::vector<SourceIdentityBinding>{rows[2]}, object.label);
        });
        auto mismatched = rows;
        mismatched[0].right.kind = SourceSymbolKind::Predicate;
        rejects(ErrorCode::Schema, [&] { (void)propose_source_bindings(scopes, mismatched, object.label); });
        mismatched = rows; mismatched[0].left.index = 999;
        rejects(ErrorCode::Schema, [&] { (void)propose_source_bindings(scopes, mismatched, object.label); });
        rejects(ErrorCode::Schema, [&] { (void)propose_source_bindings(scopes, rows, ""); });
        rejects(ErrorCode::Budget, [&] { (void)propose_source_bindings({}, rows, object.label); });
        auto duplicate_scopes = scopes; duplicate_scopes.push_back(left);
        rejects(ErrorCode::Schema, [&] { (void)propose_source_bindings(duplicate_scopes, rows, object.label); });
        const auto extra = scope("archive:unused");
        auto unused_scopes = scopes; unused_scopes.push_back(extra);
        rejects(ErrorCode::Schema, [&] { (void)propose_source_bindings(unused_scopes, rows, object.label); });
        for (const auto& other : {
            scope("archive:arity", "vehicle", 2),
            scope("archive:dimension", "vehicle", 1, {1,0,0,0,0,0,0}),
            scope("archive:scope", "vehicle", 1, {0,1,-1,0,0,0,0}, "two")}) {
            const std::vector<SourceBindingScope> pair{left, other};
            const std::vector<SourceIdentityBinding> requested{
                binding(left, other, SourceSymbolKind::Entity),
                binding(left, other, SourceSymbolKind::Predicate),
                binding(left, other, SourceSymbolKind::Quantity)};
            rejects(ErrorCode::Schema, [&] { (void)propose_source_bindings(pair, requested, object.label); });
        }
        auto wrong = scopes;
        wrong[0].host_context.unit_registry_version = "changed";
        rejects(ErrorCode::Integrity, [&] { (void)propose_source_bindings(wrong, rows, object.label); });
        {
            Journal library(directory.path / "library.db");
            for (const auto& item : scopes) {
                library.put(item.source); library.put(item.context); library.put(item.module);
            }
            library.put(object);
            Assessment untrusted;
            untrusted.state = KnowledgeState::Verified; untrusted.logical = LogicalValidity::Valid;
            untrusted.checker = "synthetic-publisher";
            library.record_assessment(id, untrusted);
            const auto root = compile_snapshot(library, directory.path / "snapshot", "bindings", "1").root;
            Journal overlay(directory.path / "overlay.db");
            GksArchive archive(directory.path / "snapshot", root);
            JournalArchive local(overlay);
            UnifiedKnowledge knowledge({&local, &archive});
            const std::map<ObjectId, ProblemContext> hosts{
                {identify(left.module), left.host_context}, {identify(right.module), right.host_context}};
            rejects(ErrorCode::Permission, [&] { (void)activate_source_bindings(knowledge, overlay, id, hosts, {}); });
            check(overlay.objects().objects.empty()); // Published "verified" metadata is not host acceptance.
            const auto active = activate_source_bindings(knowledge, overlay, id, hosts, {id});
            check(active.assumption() == id && active.bindings() == rows);
            check(active.observations().size() == 7);
            check(overlay.objects().objects.empty() && !overlay.assessment(id));
            rejects(ErrorCode::Missing, [&] { (void)activate_source_bindings(knowledge, overlay, id, {}, {id}); });
            ResolutionLimits tiny; tiny.objects = 2;
            rejects(ErrorCode::Budget, [&] { (void)activate_source_bindings(knowledge, overlay, id, hosts, {id}, tiny); });
            auto altered = hosts;
            altered.at(identify(left.module)).unit_registry_version = "wrong";
            rejects(ErrorCode::Integrity, [&] { (void)activate_source_bindings(knowledge, overlay, id, altered, {id}); });
            overlay.put(left.module);
            overlay.invalidate(identify(left.module), "Explicit source revision withdrawal");
            rejects(ErrorCode::Unavailable, [&] { (void)activate_source_bindings(knowledge, overlay, id, hosts, {id}); });
            check(!overlay.assessment(id));
        }
        {
            Journal incomplete(directory.path / "incomplete.db");
            incomplete.put(object);
            JournalArchive reader(incomplete);
            UnifiedKnowledge knowledge({&reader});
            rejects(ErrorCode::Unavailable, [&] { (void)activate_source_bindings(knowledge, incomplete, id, {}, {id}); });
        }
        std::cout << "Source binding checks passed: " << checks << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
