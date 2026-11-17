#include "retrieval.hpp"

#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace {
using namespace axiom::knowledge;
std::size_t assertions = 0;
void check(bool condition) {
    ++assertions;
    if (!condition) throw std::runtime_error("Retrieval assertion " + std::to_string(assertions));
}
template<class Function> void rejects(ErrorCode expected, Function function) {
    try { function(); }
    catch (const Error& error) { check(error.code == expected); return; }
    throw std::runtime_error("Expected retrieval rejection");
}
Object text(std::string value) {
    Object result;
    result.label = value;
    result.format = "text/plain; charset=utf-8";
    result.payload.assign(value.begin(), value.end());
    result.concepts = {"shared"};
    return result;
}

// Deliberately corrupt backend boundary, not a mock of real journal behavior.
class FaultArchive final : public KnowledgeArchive {
public:
    std::optional<KnowledgeRecord> record;
    ObjectPage page;
    bool fail = false;
    std::optional<KnowledgeRecord> get(const ObjectId&) override {
        if (fail) throw Error(ErrorCode::Unavailable, "Provider unavailable");
        return record;
    }
    ObjectPage query(const KnowledgeQuery&, std::optional<ObjectId>, std::size_t) override {
        if (fail) throw Error(ErrorCode::Unavailable, "Provider unavailable");
        return page;
    }
};

class NarrowArchive final : public KnowledgeArchive {
public:
    explicit NarrowArchive(KnowledgeArchive& archive) : archive_(archive) {}
    std::optional<KnowledgeRecord> get(const ObjectId& id) override { return archive_.get(id); }
    ObjectPage query(const KnowledgeQuery& query, std::optional<ObjectId> after,
                     std::size_t) override { return archive_.query(query, after, 1); }
private:
    KnowledgeArchive& archive_;
};
}

int main() {
    try {
        Journal local(":memory:"), archive(":memory:");
        JournalArchive local_reader(local), archive_reader(archive);
        UnifiedKnowledge knowledge({&local_reader, &archive_reader});
        auto leaf = text("leaf");
        leaf.sources.push_back({"source", "v1", "article", "raw-v1", digest(leaf.payload), 0, leaf.payload.size()});
        const auto leaf_id = archive.put(leaf);
        local.put(leaf); // Same identity in both providers must not duplicate.
        auto branch = text("branch");
        branch.links = {{Relationship::DependsOn, leaf_id}};
        const auto branch_id = archive.put(branch);
        auto root = text("root");
        root.links = {{Relationship::Requires, branch_id}, {Relationship::Requires, leaf_id}};
        const auto root_id = local.put(root);
        check(knowledge.get(root_id)->archive == 0);
        check(knowledge.get(branch_id)->archive == 1);
        check(knowledge.get(leaf_id)->record.object == leaf);
        check(!knowledge.get(identify(text("missing"))));
        auto resolution = knowledge.resolve(root_id);
        check(resolution.complete() && resolution.objects.size() == 3);
        check(resolution.objects.front().record.object == root);
        check(knowledge.resolve(root_id, {3, 16 * 1024 * 1024, 3, 1}).complete());
        rejects(ErrorCode::Budget, [&] { (void)knowledge.resolve(root_id, {2}); });
        rejects(ErrorCode::Budget, [&] { (void)knowledge.resolve(root_id, {256, 1}); });
        rejects(ErrorCode::Budget, [&] { (void)knowledge.resolve(root_id, {256, 16 * 1024 * 1024, 2}); });
        rejects(ErrorCode::Budget, [&] { (void)knowledge.resolve(root_id, {256, 16 * 1024 * 1024, 4096, 0}); });
        auto incomplete = text("needs absent source");
        const auto missing_id = identify(text("absent source"));
        incomplete.links = {{Relationship::DependsOn, missing_id}};
        const auto incomplete_id = local.put(incomplete);
        resolution = knowledge.resolve(incomplete_id);
        check(!resolution.complete() && resolution.unavailable == std::vector<ObjectId>{missing_id});
        check(resolution.objects.front().record.object == incomplete);
        const auto unknown = knowledge.resolve(missing_id);
        check(!unknown.complete() && unknown.objects.empty());
        // Real versioned metadata can cycle without forging content hashes.
        Assessment a;
        a.checker = "retrieval-fixture-v1";
        a.checked_dependencies = {branch_id};
        local.record_assessment(root_id, a);
        a.checked_dependencies = {root_id};
        a.certificate = leaf_id;
        archive.record_assessment(branch_id, a);
        resolution = knowledge.resolve(root_id);
        check(resolution.complete() && resolution.objects.size() == 3);
        check(resolution.objects.front().record.assessment->version == 1);
        check(resolution.objects.front().record.assessment->logical == LogicalValidity::Unchecked);
        // Current metadata is fetched again, not frozen in a stale object cache.
        a.checked_dependencies = {missing_id};
        a.certificate = root_id; // Self-certificate adds no recursive work.
        local.record_assessment(root_id, a);
        resolution = knowledge.resolve(root_id);
        check(!resolution.complete() && resolution.unavailable == std::vector<ObjectId>{missing_id});
        check(resolution.objects.front().record.assessment->version == 2);
        const auto certificate_only = local.put(text("certificate dependency only"));
        Assessment certificate;
        certificate.checker = "retrieval-fixture-v1";
        certificate.certificate = missing_id;
        local.record_assessment(certificate_only, certificate);
        check(knowledge.resolve(certificate_only).unavailable == std::vector<ObjectId>{missing_id});

        KnowledgeQuery query;
        query.kind = QueryKind::Concept;
        query.value = "shared";
        std::vector<ObjectId> all;
        std::optional<ObjectId> cursor;
        do {
            const auto page = knowledge.query(query, cursor, 1);
            check(page.objects.size() == 1);
            all.insert(all.end(), page.objects.begin(), page.objects.end());
            cursor = page.next;
            check(page.complete == !cursor.has_value());
        } while (cursor);
        auto expected = std::vector<ObjectId>{root_id, branch_id, leaf_id, incomplete_id, certificate_only};
        std::sort(expected.begin(), expected.end());
        check(all == expected);
        // Force an unseen thin-provider identity strictly before the wide
        // provider's tail; merging all returned rows would skip it forever.
        Journal thin_store(":memory:"), wide_store(":memory:");
        auto ordered = std::vector<Object>{text("ordered-0"), text("ordered-1"),
                                           text("ordered-2"), text("ordered-3")};
        std::sort(ordered.begin(), ordered.end(),
                  [](const auto& left, const auto& right) { return identify(left) < identify(right); });
        thin_store.put(ordered[0]); thin_store.put(ordered[1]);
        wide_store.put(ordered[2]); wide_store.put(ordered[3]);
        JournalArchive thin_reader(thin_store), wide_reader(wide_store);
        NarrowArchive thin(thin_reader);
        UnifiedKnowledge prefix({&wide_reader, &thin});
        const auto first = prefix.query(query);
        check(first.objects == std::vector<ObjectId>{identify(ordered[0])});
        check(!first.complete && first.next == identify(ordered[0]));
        const auto second = prefix.query(query, first.next);
        check(second.complete && second.objects == std::vector<ObjectId>({
            identify(ordered[1]), identify(ordered[2]), identify(ordered[3])}));
        NarrowArchive narrow(archive_reader);
        UnifiedKnowledge short_pages({&local_reader, &narrow});
        all.clear();
        cursor.reset();
        do {
            const auto page = short_pages.query(query, cursor, 64);
            check(!page.objects.empty());
            all.insert(all.end(), page.objects.begin(), page.objects.end());
            cursor = page.next;
        } while (cursor);
        check(all == expected);
        query.kind = QueryKind::Source;
        query.value = "source";
        query.source_version = "v1";
        check(knowledge.query(query).objects == std::vector<ObjectId>{leaf_id});
        query.source_version = "v2";
        check(knowledge.query(query).objects.empty());
        query.kind = QueryKind::Kind;
        query.object_kind = ObjectKind::Claim;
        check(knowledge.query(query).objects == expected);
        query.kind = QueryKind::Text;
        query.value = "needs absent";
        check(knowledge.query(query).objects == std::vector<ObjectId>{incomplete_id});
        query.kind = QueryKind::Related;
        query.target = branch_id;
        query.relationship = Relationship::Requires;
        check(knowledge.query(query).objects == std::vector<ObjectId>{root_id});
        query.kind = QueryKind::RequiredBy;
        query.target = leaf_id;
        auto required = std::vector<ObjectId>{branch_id, root_id};
        std::sort(required.begin(), required.end());
        check(knowledge.query(query).objects == required);
        rejects(ErrorCode::Budget, [&] { (void)knowledge.query(query, {}, 0); });
        rejects(ErrorCode::Budget, [&] { (void)knowledge.query(query, {}, 257); });
        query.kind = QueryKind(999);
        rejects(ErrorCode::Schema, [&] { (void)knowledge.query(query); });

        FaultArchive fault;
        UnifiedKnowledge bad({&fault});
        fault.record = KnowledgeRecord{text("forged"), {}};
        rejects(ErrorCode::Integrity, [&] { (void)bad.get(root_id); });
        fault.record = KnowledgeRecord{root, Assessment{}};
        rejects(ErrorCode::Schema, [&] { (void)bad.get(root_id); });
        Assessment wide;
        wide.version = 1; wide.checker = "synthetic-checker";
        wide.checked_dependencies.assign(maximum_assessment_dependencies, leaf_id);
        fault.record = KnowledgeRecord{root, wide};
        check(bad.get(root_id)->record.assessment->checked_dependencies.size() == maximum_assessment_dependencies);
        wide.checked_dependencies.push_back(leaf_id);
        fault.record = KnowledgeRecord{root, wide};
        rejects(ErrorCode::Budget, [&] { (void)bad.get(root_id); });
        fault.page.complete = false;
        query = {};
        rejects(ErrorCode::Integrity, [&] { (void)bad.query(query); });
        fault.page = {{root_id, root_id}, true, {}};
        rejects(ErrorCode::Integrity, [&] { (void)bad.query(query); });
        fault.page = {{root_id}, false, leaf_id};
        rejects(ErrorCode::Integrity, [&] { (void)bad.query(query); });
        fault.page = {{root_id}, true, root_id};
        rejects(ErrorCode::Integrity, [&] { (void)bad.query(query); });
        fault.page = {{root_id}, true, {}};
        rejects(ErrorCode::Integrity, [&] { (void)bad.query(query, root_id); });
        fault.fail = true;
        rejects(ErrorCode::Unavailable, [&] { (void)bad.resolve(root_id); });
        rejects(ErrorCode::Unavailable, [&] { (void)bad.query(query); });
        rejects(ErrorCode::Schema, [&] { UnifiedKnowledge invalid({nullptr}); });
        rejects(ErrorCode::Schema, [&] { UnifiedKnowledge invalid({&fault, &fault}); });
        UnifiedKnowledge empty({});
        check(empty.query(query).complete && empty.query(query).objects.empty());
        check(!empty.resolve(root_id).complete());
        std::cout << "retrieval tests passed: " << assertions << " deterministic assertions\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
