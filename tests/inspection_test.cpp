#include "inspection.hpp"
#include "virtual_drive.hpp"

#include <json-c/json.h>
#include <iostream>
#include <source_location>
#include <unistd.h>

namespace {
using namespace axiom::knowledge;
using Json = std::unique_ptr<json_object, decltype(&json_object_put)>;
std::size_t checks = 0;
void check(bool condition, const std::source_location where = std::source_location::current()) {
    ++checks;
    if (!condition) throw std::runtime_error("Inspection assertion at " + std::to_string(where.line()));
}
template<class F> void rejects(ErrorCode code, F&& operation) {
    ++checks;
    try { operation(); }
    catch (const Error& error) { if (error.code == code) return; throw; }
    throw std::runtime_error("Expected inspection rejection");
}
struct Directory {
    std::filesystem::path path;
    Directory() {
        std::string pattern = "/private/tmp/inspection-test-XXXXXX";
        const auto made = mkdtemp(pattern.data());
        if (!made) throw std::runtime_error("Cannot create inspection fixture");
        path = made;
    }
    ~Directory() { std::filesystem::remove_all(path); }
};
Json parse(std::string_view text) {
    Json root(json_tokener_parse(std::string(text).c_str()), json_object_put);
    check(root != nullptr);
    return root;
}
json_object* get(json_object* object, const char* field) {
    return json_object_object_get(object, field);
}
std::string string(json_object* object, const char* field) {
    auto* value = get(object, field);
    check(value && json_object_get_type(value) == json_type_string);
    return json_object_get_string(value);
}
json_object* node(json_object* output, const ObjectId& id) {
    const auto nodes = get(output, "nodes");
    for (std::size_t i = 0; i < json_object_array_length(nodes); ++i) {
        auto* record = json_object_array_get_idx(nodes, i);
        if (string(record, "id") == hex(id)) return record;
    }
    throw std::runtime_error("Missing inspection node");
}
Object value(ObjectKind kind, std::string label) {
    Object object;
    object.kind = kind;
    object.label = std::move(label);
    object.format = "synthetic.inspect.v1";
    object.payload = {0, 255, 1};
    return object;
}
}

int main() {
    try {
        Directory directory;
        Journal journal(directory.path / "journal.db");
        const auto a = journal.put(value(ObjectKind::Claim, "A"));
        const auto b = journal.put(value(ObjectKind::Claim, "B"));
        const auto rule = journal.put(value(ObjectKind::Rule, "R"));
        const auto assumption = journal.put(value(ObjectKind::Assumption, "Condition"));
        auto source = value(ObjectKind::Evidence, "Exact archived DSL");
        const std::string raw_dsl = "CLAIM source assertion\n";
        source.payload = Bytes(raw_dsl.begin(), raw_dsl.end());
        source.sources.push_back({"synthetic-zim", "1", "A/article", "verbatim",
            digest(source.payload), 2, 12});
        source.sources.push_back({"synthetic-large-offset", "1", "unavailable", "fixture",
            digest(Bytes{4}), 9007199254740993ULL, 1});
        const auto evidence = journal.put(source);
        const auto conclusion = journal.put(value(ObjectKind::Fact, "Conclusion"));
        auto derivation = value(ObjectKind::Derivation, "Joint derivation");
        derivation.derivation = Derivation{conclusion, rule, {b, a, b}, {assumption}, {evidence}};
        const auto proof = journal.put(derivation);
        auto alternative = value(ObjectKind::Derivation, "Alternative derivation");
        alternative.derivation = Derivation{conclusion, rule, {a}, {}, {evidence}};
        alternative.links.push_back({Relationship::AlternativeTo, proof});
        const auto alternate = journal.put(alternative);
        auto contradiction = value(ObjectKind::Contradiction, "Disagreement");
        contradiction.links = {{Relationship::Contradicts, conclusion}, {Relationship::Requires, a}};
        const auto conflict = journal.put(contradiction);
        Assessment assessment;
        assessment.state = KnowledgeState::Disputed;
        assessment.logical = LogicalValidity::Valid;
        assessment.evidence = EvidenceAssessment::Conflicting;
        assessment.checker = "synthetic-observed-checker";
        assessment.reason = "Synthetic independent logical and evidence metadata";
        assessment.checked_dependencies = {assumption, evidence};
        journal.record_assessment(conclusion, assessment);
        JournalArchive local(journal);
        UnifiedKnowledge knowledge({&local});
        const auto original_count = journal.objects({}, 64).objects.size();
        const auto exported = parse(inspect_knowledge(knowledge, conclusion));
        check(json_object_get_boolean(get(exported.get(), "observational_only")));
        check(!json_object_get_boolean(get(exported.get(), "published")));
        check(!json_object_get_boolean(get(exported.get(), "exploration_limited")));
        check(!json_object_get_boolean(get(exported.get(), "has_unavailable")));
        check(json_object_array_length(get(exported.get(), "nodes")) == original_count);
        auto* shown = node(exported.get(), proof);
        check(string(shown, "kind") == "DERIVATION");
        auto* joint = get(shown, "derivation");
        auto* premises = get(joint, "premises");
        check(json_object_array_length(premises) == 3);
        check(std::string(json_object_get_string(json_object_array_get_idx(premises, 0))) == hex(b));
        check(std::string(json_object_get_string(json_object_array_get_idx(premises, 1))) == hex(a));
        check(std::string(json_object_get_string(json_object_array_get_idx(premises, 2))) == hex(b));
        check(string(node(exported.get(), alternate), "kind") == "DERIVATION");
        check(string(node(exported.get(), conflict), "kind") == "CONTRADICTION");
        const auto observed = get(node(exported.get(), conclusion), "assessment");
        check(string(observed, "logical") == "VALID");
        check(string(observed, "evidence") == "CONFLICTING");
        check(string(observed, "state") == "DISPUTED");
        check(string(node(exported.get(), a), "payload_hex") == "00ff01");
        const auto provenance = json_object_array_get_idx(get(node(exported.get(), evidence), "sources"), 0);
        check(string(provenance, "entry") == "A/article");
        check(json_object_get_uint64(get(provenance, "offset")) == 2);
        check(json_object_get_uint64(get(provenance, "length")) == 12);
        check(string(json_object_array_get_idx(get(node(exported.get(), evidence), "sources"), 1), "offset")
            == "9007199254740993");
        check(journal.objects({}, 64).objects.size() == original_count);
        check(journal.assessment_history(conclusion).records.size() == 1);
        check(!journal.assessment(proof));
        KnowledgeQuery incoming;
        incoming.kind = QueryKind::Related;
        incoming.target = conclusion;
        incoming.relationship = Relationship::Produces;
        const auto first_page = parse(inspect_index(knowledge, incoming, {}, 1));
        check(!json_object_get_boolean(get(first_page.get(), "complete")));
        check(json_object_array_length(get(first_page.get(), "records")) == 1);
        const auto cursor = parse_id(string(first_page.get(), "next"));
        const auto second_page = parse(inspect_index(knowledge, incoming, cursor, 1));
        check(json_object_get_boolean(get(second_page.get(), "complete")));
        check(json_object_array_length(get(second_page.get(), "records")) == 1);
        check(string(json_object_array_get_idx(get(first_page.get(), "records"), 0), "id")
            != string(json_object_array_get_idx(get(second_page.get(), "records"), 0), "id"));
        KnowledgeQuery search;
        search.kind = QueryKind::Text;
        search.value = "Joint";
        const auto matches = parse(inspect_index(knowledge, search));
        check(json_object_array_length(get(matches.get(), "records")) == 1);
        check(string(json_object_array_get_idx(get(matches.get(), "records"), 0), "id") == hex(proof));
        rejects(ErrorCode::Budget, [&] { inspect_index(knowledge, search, {}, 65); });
        KnowledgeQuery by_kind;
        by_kind.kind = QueryKind::Kind;
        by_kind.object_kind = ObjectKind::Claim;
        const auto claims = parse(inspect_index(knowledge, by_kind));
        check(json_object_array_length(get(claims.get(), "records")) == 2);
        const auto all = parse(inspect_index(knowledge, KnowledgeQuery{}));
        check(json_object_array_length(get(all.get(), "records")) == original_count);
        InspectionLimits small;
        small.objects = 1;
        const auto limited = parse(inspect_knowledge(knowledge, conclusion, small));
        check(json_object_array_length(get(limited.get(), "nodes")) == 1);
        check(json_object_get_boolean(get(limited.get(), "exploration_limited")));
        small = {};
        small.queries = 1;
        const auto query_limited = parse(inspect_knowledge(knowledge, conclusion, small));
        check(json_object_get_boolean(get(query_limited.get(), "exploration_limited")));
        small = {};
        small.bytes = 1;
        rejects(ErrorCode::Budget, [&] { inspect_knowledge(knowledge, conclusion, small); });
        small = {};
        small.frontier = 0;
        rejects(ErrorCode::Budget, [&] { inspect_knowledge(knowledge, conclusion, small); });
        const auto missing = ObjectId{digest(Bytes{42})};
        const auto absent = parse(inspect_knowledge(knowledge, missing));
        check(json_object_array_length(get(absent.get(), "nodes")) == 0);
        check(json_object_get_boolean(get(absent.get(), "has_unavailable")));
        check(std::string(json_object_get_string(json_object_array_get_idx(get(absent.get(), "unavailable"), 0))) == hex(missing));
        const auto snapshot = compile_snapshot(journal, directory.path / "snapshot", "inspection", "1");
        GksArchive archive(directory.path / "snapshot", snapshot.root);
        UnifiedKnowledge immutable({&archive});
        const auto archived = parse(inspect_knowledge(immutable, conclusion));
        check(string(node(archived.get(), proof), "canonical_hex") == string(shown, "canonical_hex"));
        check(string(node(archived.get(), evidence), "payload_hex") == string(node(exported.get(), evidence), "payload_hex"));
        check(string(get(node(archived.get(), conclusion), "assessment"), "evidence") == "CONFLICTING");
        std::cout << "Inspection checks passed: " << checks << '\n';
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
