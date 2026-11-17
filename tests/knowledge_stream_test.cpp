#include "retrieval.hpp"

#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace {
using namespace axiom::knowledge;
std::size_t assertions = 0;
void check(bool condition) {
    ++assertions;
    if (!condition) throw std::runtime_error("Knowledge stream assertion " + std::to_string(assertions));
}
template<class Function> void rejects(ErrorCode expected, Function function) {
    try { function(); }
    catch (const Error& error) { check(error.code == expected); return; }
    throw std::runtime_error("Expected knowledge stream rejection");
}
class ObservedArchive final : public KnowledgeArchive {
public:
    explicit ObservedArchive(KnowledgeArchive& archive) : archive_(archive) {}
    std::size_t queries = 0, gets = 0;
    bool absent = false, fail = false;
    std::optional<KnowledgeRecord> get(const ObjectId& id) override {
        ++gets;
        if (fail) throw Error(ErrorCode::Unavailable, "Provider read failed");
        return absent ? std::nullopt : archive_.get(id);
    }
    ObjectPage query(const KnowledgeQuery& query, std::optional<ObjectId> after, std::size_t limit) override {
        ++queries;
        return archive_.query(query, after, limit);
    }
private:
    KnowledgeArchive& archive_;
};
}

int main() {
    try {
        Journal first(":memory:"), second(":memory:");
        JournalArchive first_reader(first), second_reader(second);
        ObservedArchive observed_first(first_reader), observed_second(second_reader);
        UnifiedKnowledge knowledge({&observed_first, &observed_second});
        Object target;
        target.label = "target";
        target.format = "text/plain; charset=utf-8";
        const auto target_id = first.put(target);
        std::vector<ObjectId> expected, graph;
        for (std::size_t i = 0; i < 20; ++i) {
            Object object;
            object.label = "document " + std::to_string(i);
            object.concepts = {"flow"};
            object.format = "text/plain; charset=utf-8";
            const auto body = "streamneedle " + std::to_string(i);
            object.payload.assign(body.begin(), body.end());
            if (i % 3 == 0) object.links.push_back({Relationship::Supports, target_id});
            const auto id = (i % 2 ? first : second).put(object);
            if (i % 3 == 0) graph.push_back(id);
            expected.push_back(id);
        }
        std::sort(expected.begin(), expected.end());
        std::sort(graph.begin(), graph.end());
        KnowledgeQuery query;
        query.kind = QueryKind::Concept;
        query.value = "flow";
        KnowledgeStream stream(knowledge, query, {7});
        check(observed_first.queries == 0 && observed_first.gets == 0);
        check(observed_second.queries == 0 && observed_second.gets == 0);
        auto first_item = stream.next();
        check(first_item && first_item->object && first_item->id == expected.front());
        check(observed_first.queries == 1 && observed_second.queries == 1);
        check(observed_first.gets + observed_second.gets <= 2);
        std::vector<ObjectId> actual{first_item->id};
        while (const auto item = stream.next()) {
            check(item->object && item->object->id == item->id);
            actual.push_back(item->id);
        }
        check(actual == expected);
        check(stream.stats().pages == 3 && stream.stats().items == 20);
        const auto gets = observed_first.gets + observed_second.gets;
        const auto queries = observed_first.queries + observed_second.queries;
        check(!stream.next() && !stream.next());
        check(observed_first.gets + observed_second.gets == gets);
        check(observed_first.queries + observed_second.queries == queries);

        query.kind = QueryKind::Text;
        query.value = "streamneedle";
        KnowledgeStream text_stream(knowledge, query, {3});
        actual.clear();
        while (const auto item = text_stream.next()) actual.push_back(item->id);
        check(actual == expected);
        query.kind = QueryKind::Related;
        query.target = target_id;
        query.relationship = Relationship::Supports;
        KnowledgeStream graph_stream(knowledge, query, {2});
        actual.clear();
        while (const auto item = graph_stream.next()) actual.push_back(item->id);
        check(actual == graph);

        query.kind = QueryKind::Concept;
        query.value = "flow";
        KnowledgeStream item_limited(knowledge, query, {7, 64, 1});
        check(item_limited.next().has_value());
        rejects(ErrorCode::Budget, [&] { (void)item_limited.next(); });
        rejects(ErrorCode::Unavailable, [&] { (void)item_limited.next(); });
        KnowledgeStream page_limited(knowledge, query, {1, 1});
        check(page_limited.next().has_value());
        rejects(ErrorCode::Budget, [&] { (void)page_limited.next(); });
        KnowledgeStream byte_limited(knowledge, query, {7, 64, 4096, 1});
        rejects(ErrorCode::Budget, [&] { (void)byte_limited.next(); });
        rejects(ErrorCode::Unavailable, [&] { (void)byte_limited.next(); });
        KnowledgeStream exact(knowledge, query, {7, 3, 20, stream.stats().content_bytes});
        while (exact.next()) {}
        check(exact.stats().content_bytes == stream.stats().content_bytes);
        KnowledgeStream short_bytes(knowledge, query, {7, 3, 20, stream.stats().content_bytes - 1});
        rejects(ErrorCode::Budget, [&] { while (short_bytes.next()) {} });
        query.value = "not present";
        KnowledgeStream empty(knowledge, query);
        check(!empty.next() && empty.stats().items == 0 && empty.stats().pages == 1);

        UnifiedKnowledge one({&observed_first});
        query.value = "flow";
        KnowledgeStream unavailable(one, query, {1});
        observed_first.absent = true;
        const auto absent = unavailable.next();
        check(absent && !absent->object);
        check(first.contains(absent->id)); // The index hit is not silently skipped.
        check(unavailable.stats().items == 1 && unavailable.stats().content_bytes == Digest{}.size());
        observed_first.absent = false;
        KnowledgeStream failure(one, query);
        observed_first.fail = true;
        rejects(ErrorCode::Unavailable, [&] { (void)failure.next(); });
        observed_first.fail = false;
        const auto calls = observed_first.gets;
        rejects(ErrorCode::Unavailable, [&] { (void)failure.next(); });
        check(observed_first.gets == calls);
        rejects(ErrorCode::Budget, [&] { KnowledgeStream invalid(knowledge, query, {0}); });
        rejects(ErrorCode::Budget, [&] { KnowledgeStream invalid(knowledge, query, {1, 0}); });
        std::cout << "knowledge stream tests passed: " << assertions << " deterministic assertions\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
