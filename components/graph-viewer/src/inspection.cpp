#include "inspection.hpp"

#include <json-c/json.h>
#include <deque>
#include <set>

namespace axiom::knowledge {
namespace {
using Json = std::unique_ptr<json_object, decltype(&json_object_put)>;
void text(json_object* object, const char* key, std::string_view value) {
    json_object_object_add(object, key, json_object_new_string_len(value.data(), static_cast<int>(value.size())));
}
void number(json_object* object, const char* key, std::uint64_t value) {
    json_object_object_add(object, key, json_object_new_uint64(value));
}
void flag(json_object* object, const char* key, bool value) {
    json_object_object_add(object, key, json_object_new_boolean(value));
}
std::string hex_bytes(std::span<const std::uint8_t> bytes) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(bytes.size() * 2);
    for (const auto value : bytes) {
        result.push_back(digits[value >> 4]);
        result.push_back(digits[value & 15]);
    }
    return result;
}
void identities(json_object* parent, const char* key, std::span<const ObjectId> ids) {
    auto* array = json_object_new_array();
    json_object_object_add(parent, key, array);
    for (const auto& id : ids) json_object_array_add(array, json_object_new_string(hex(id).c_str()));
}
Json describe(const RetrievedObject& found, const Bytes& encoded) {
    Json root(json_object_new_object(), json_object_put);
    auto* value = root.get();
    const auto& object = found.record.object;
    text(value, "id", hex(found.id));
    number(value, "archive", found.archive);
    text(value, "canonical_hex", hex_bytes(encoded));
    text(value, "kind", name(object.kind));
    text(value, "visibility", name(object.visibility));
    text(value, "label", object.label);
    text(value, "format", object.format);
    text(value, "payload_hex", hex_bytes(object.payload));
    auto* concepts = json_object_new_array();
    json_object_object_add(value, "concepts", concepts);
    for (const auto& concept_name : object.concepts)
        json_object_array_add(concepts, json_object_new_string(concept_name.c_str()));
    auto* sources = json_object_new_array();
    json_object_object_add(value, "sources", sources);
    for (const auto& source : object.sources) {
        auto* item = json_object_new_object();
        json_object_array_add(sources, item);
        text(item, "archive", source.archive_id);
        text(item, "version", source.archive_version);
        text(item, "entry", source.entry_path);
        text(item, "extractor", source.extractor_version);
        text(item, "digest", hex(ObjectId{source.source_digest}));
        text(item, "offset", std::to_string(source.offset));
        text(item, "length", std::to_string(source.length));
    }
    auto* links = json_object_new_array();
    json_object_object_add(value, "links", links);
    for (const auto& link : object.links) {
        auto* item = json_object_new_object();
        json_object_array_add(links, item);
        text(item, "relationship", name(link.relationship));
        text(item, "target", hex(link.target));
    }
    json_object_object_add(value, "derivation", nullptr);
    if (object.derivation) {
        const auto& derivation = *object.derivation;
        auto* item = json_object_new_object();
        json_object_object_add(value, "derivation", item);
        text(item, "conclusion", hex(derivation.conclusion));
        text(item, "rule", hex(derivation.rule));
        identities(item, "premises", derivation.premises);
        identities(item, "assumptions", derivation.assumptions);
        identities(item, "evidence", derivation.evidence);
    }
    json_object_object_add(value, "assessment", nullptr);
    if (found.record.assessment) {
        const auto& assessment = *found.record.assessment;
        auto* item = json_object_new_object();
        json_object_object_add(value, "assessment", item);
        text(item, "version", std::to_string(assessment.version));
        text(item, "state", name(assessment.state));
        text(item, "logical", name(assessment.logical));
        text(item, "evidence", name(assessment.evidence));
        text(item, "checker", assessment.checker);
        text(item, "reason", assessment.reason);
        if (assessment.certificate) text(item, "certificate", hex(*assessment.certificate));
        else json_object_object_add(item, "certificate", nullptr);
        identities(item, "checked_dependencies", assessment.checked_dependencies);
    }
    return root;
}
}

std::string inspect_knowledge(UnifiedKnowledge& knowledge, const ObjectId& root, InspectionLimits limits) {
    if (!limits.objects || limits.objects > 256 || !limits.bytes || limits.bytes > 16 * 1024 * 1024
        || !limits.queries || limits.queries > 1024 || !limits.frontier || limits.frontier > 4096
        || limits.depth > 32 || !limits.incoming_page || limits.incoming_page > 64)
        throw Error(ErrorCode::Budget, "Invalid inspection budget");
    Json output(json_object_new_object(), json_object_put);
    text(output.get(), "format", "gammathink.inspection.v1");
    text(output.get(), "root", hex(root));
    flag(output.get(), "observational_only", true);
    flag(output.get(), "published", false);
    auto* nodes = json_object_new_array();
    auto* missing = json_object_new_array();
    json_object_object_add(output.get(), "nodes", nodes);
    json_object_object_add(output.get(), "unavailable", missing);
    std::deque<std::pair<ObjectId, std::size_t>> pending{{root, 0}};
    std::set<ObjectId> queued{root};
    std::size_t bytes = 0, queries = 0, visited = 0;
    bool boundary = false;
    const auto enqueue = [&](const ObjectId& id, std::size_t depth) {
        if (queued.contains(id)) return;
        if (depth > limits.depth || queued.size() >= limits.frontier) { boundary = true; return; }
        queued.insert(id);
        pending.emplace_back(id, depth);
    };
    while (!pending.empty() && visited < limits.objects && queries < limits.queries) {
        const auto [id, depth] = pending.front();
        pending.pop_front();
        ++visited;
        ++queries;
        auto found = knowledge.get(id);
        if (!found) {
            json_object_array_add(missing, json_object_new_string(hex(id).c_str()));
            continue;
        }
        const auto encoded = encode_object(found->record.object);
        auto record = describe(*found, encoded);
        const auto size = std::string_view(json_object_to_json_string_ext(record.get(), JSON_C_TO_STRING_PLAIN)).size();
        if (size > limits.bytes - bytes) {
            boundary = true;
            // The focus must not silently disappear behind a budget marker.
            if (id == root) throw Error(ErrorCode::Budget, "Inspection focus exceeds output budget");
            continue;
        }
        bytes += size;
        json_object_array_add(nodes, record.release());
        for (const auto& target : references(found->record.object)) enqueue(target, depth + 1);
        if (const auto& assessment = found->record.assessment) {
            for (const auto& target : assessment->checked_dependencies) enqueue(target, depth + 1);
            if (assessment->certificate) enqueue(*assessment->certificate, depth + 1);
        }
        // Incoming derivations, alternatives and contradictions are indexed
        // observations, not new implications invented by the viewer.
        for (const auto relationship : {Relationship::Produces, Relationship::Contradicts,
                Relationship::AlternativeTo, Relationship::Tests, Relationship::Implies,
                Relationship::Requires, Relationship::DependsOn, Relationship::Supports}) {
            if (queries >= limits.queries) { boundary = true; break; }
            ++queries;
            KnowledgeQuery query;
            query.kind = QueryKind::Related;
            query.target = id;
            query.relationship = relationship;
            const auto page = knowledge.query(query, {}, limits.incoming_page);
            if (!page.complete) boundary = true;
            for (const auto& target : page.objects) enqueue(target, depth + 1);
        }
    }
    boundary = boundary || !pending.empty();
    flag(output.get(), "exploration_limited", boundary);
    flag(output.get(), "has_unavailable", json_object_array_length(missing) != 0);
    number(output.get(), "queries", queries);
    number(output.get(), "record_bytes", bytes);
    return json_object_to_json_string_ext(output.get(), JSON_C_TO_STRING_PLAIN);
}

std::string inspect_index(UnifiedKnowledge& knowledge, const KnowledgeQuery& query,
                          std::optional<ObjectId> after, std::size_t limit) {
    if (!limit || limit > 64) throw Error(ErrorCode::Budget, "Invalid inspection index page");
    const auto page = knowledge.query(query, after, limit);
    Json output(json_object_new_object(), json_object_put);
    text(output.get(), "format", "gammathink.inspection-index.v1");
    flag(output.get(), "observational_only", true);
    flag(output.get(), "complete", page.complete);
    if (page.next) text(output.get(), "next", hex(*page.next));
    else json_object_object_add(output.get(), "next", nullptr);
    auto* records = json_object_new_array();
    json_object_object_add(output.get(), "records", records);
    std::size_t bytes = 0;
    for (const auto& id : page.objects) {
        Json item(json_object_new_object(), json_object_put);
        text(item.get(), "id", hex(id));
        const auto found = knowledge.get(id);
        flag(item.get(), "available", found.has_value());
        if (found) {
            text(item.get(), "label", found->record.object.label);
            text(item.get(), "kind", name(found->record.object.kind));
            if (found->record.assessment) {
                const auto& assessment = *found->record.assessment;
                text(item.get(), "state", name(assessment.state));
                text(item.get(), "logical", name(assessment.logical));
                text(item.get(), "evidence", name(assessment.evidence));
            }
        }
        const auto size = std::string_view(json_object_to_json_string_ext(item.get(), JSON_C_TO_STRING_PLAIN)).size();
        if (size > 1024 * 1024 - bytes) throw Error(ErrorCode::Budget, "Inspection index exceeds output budget");
        bytes += size;
        json_object_array_add(records, item.release());
    }
    return json_object_to_json_string_ext(output.get(), JSON_C_TO_STRING_PLAIN);
}

} // namespace axiom::knowledge
