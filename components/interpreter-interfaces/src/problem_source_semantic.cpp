#include "problem_source_semantic.hpp"
#include "problem_semantic.hpp"

#include <json-c/json.h>

#include <memory>
#include <stdexcept>

namespace axiom {
namespace {
using J = json_object*;
using Json = std::unique_ptr<json_object, decltype(&json_object_put)>;
void require(bool condition, const char* message) {
    if (!condition) throw std::invalid_argument(message);
}
Json parse(const std::string& json) {
    require(!json.empty() && json.size() <= 262144 && json.find('\0') == std::string::npos,
        "Invalid source proposal size");
    const auto tokener = std::unique_ptr<json_tokener, decltype(&json_tokener_free)>(
        json_tokener_new_ex(48), json_tokener_free);
    json_tokener_set_flags(tokener.get(), JSON_TOKENER_STRICT);
    Json result(json_tokener_parse_ex(tokener.get(), json.data(), static_cast<int>(json.size())), json_object_put);
    require(json_tokener_get_error(tokener.get()) == json_tokener_success
        && result && json_object_get_type(result.get()) == json_type_object, "Invalid source proposal JSON");
    for (auto i = json_tokener_get_parse_end(tokener.get()); i < json.size(); ++i)
        require(json[i] == ' ' || json[i] == '\r' || json[i] == '\n' || json[i] == '\t',
            "Trailing source proposal JSON");
    return result;
}
J field(J object, const char* key) {
    J value = nullptr;
    require(json_object_get_type(object) == json_type_object
        && json_object_object_get_ex(object, key, &value), "Missing source proposal field");
    return value;
}
std::string string(J value) {
    require(json_object_get_type(value) == json_type_string, "Invalid source handle type");
    std::string result(json_object_get_string(value), json_object_get_string_len(value));
    require(!result.empty() && result.find('\0') == std::string::npos, "Invalid source handle text");
    return result;
}
std::size_t count(J value, std::size_t maximum) {
    require(json_object_get_type(value) == json_type_array, "Invalid source proposal array");
    const auto size = json_object_array_length(value);
    require(size <= maximum, "Source proposal array limit exceeded");
    return size;
}
J at(J value, std::size_t index) {
    require(index < count(value, 256), "Incomplete source proposal tuple");
    return json_object_array_get_idx(value, index);
}
J encoded(const EvidenceSpan& span) {
    auto result = json_object_new_array();
    for (const auto number : {span.view, span.offset, span.length})
        json_object_array_add(result, json_object_new_int64(number));
    json_object_array_add(result, span.locator == no_step ? nullptr : json_object_new_int64(span.locator));
    return result;
}
}

ProblemRequest parse_problem_source_proposal(const std::string& json,
                                            const ProblemSourceCatalog& catalog) {
    const auto root = parse(json);
    require(json_object_object_length(root.get()) == 2, "Unexpected source proposal fields");
    const auto snapshot = string(field(root.get(), "snapshot"));
    require(snapshot == catalog.snapshot(), "Stale catalog snapshot");
    const auto proposal = field(root.get(), "proposal");
    require(json_object_get_type(proposal) == json_type_object, "Invalid source proposal object");
    J status = nullptr;
    if (!json_object_object_get_ex(proposal, "status", &status)) {
        const auto span = [&](J selector) {
            if (json_object_get_type(selector) == json_type_string)
                return encoded(catalog.resolve(snapshot, string(selector)));
            require(count(selector, 2) == 2, "Invalid source range");
            return encoded(catalog.resolve(snapshot, string(at(selector, 0)), string(at(selector, 1))));
        };
        const auto replace_span = [&](J tuple, std::size_t index, bool nullable = false) {
            const auto selector = at(tuple, index);
            if (nullable && json_object_get_type(selector) == json_type_null) return;
            json_object_array_put_idx(tuple, index, span(selector));
        };
        const auto origin = [&](J supports) {
            const auto size = count(supports, 16);
            for (std::size_t i = 0; i < size; ++i) replace_span(supports, i);
        };
        const auto unit = [&](J tuple, std::size_t index) {
            const auto resolved = catalog.unit(snapshot, string(at(tuple, index)));
            json_object_array_put_idx(tuple, index, json_object_new_int64(resolved));
        };
        const auto each = [&](const char* key, std::size_t limit, auto action) {
            const auto values = field(proposal, key);
            const auto size = count(values, limit);
            for (std::size_t i = 0; i < size; ++i) action(at(values, i));
        };
        each("quantities", 64, [&](J value) { unit(value, 3); });
        each("numbers", 128, [&](J value) {
            replace_span(value, 0);
            replace_span(value, 1, true);
            unit(value, 2);
            replace_span(value, 4, true);
        });
        each("bindings", 128, [&](J value) { origin(at(value, 2)); });
        each("equations", 64, [&](J value) { origin(at(value, 4)); });
        each("rules", 128, [&](J value) { origin(at(value, 3)); });
        const auto goal = field(proposal, "goal");
        if (string(at(goal, 0)) == "q") unit(goal, 2);
    }
    return parse_problem_proposal(json_object_to_json_string_ext(proposal, JSON_C_TO_STRING_PLAIN),
        catalog.context(), catalog.limits());
}

std::string problem_source_proposal_schema(const ProblemSourceCatalog& catalog) {
    const auto schema = parse(problem_proposal_schema());
    const auto definitions = field(schema.get(), "$defs");
    auto handles = json_object_new_object(), sources = json_object_new_array();
    json_object_object_add(handles, "enum", sources);
    for (std::size_t i = 0; i < catalog.entries().size(); ++i)
        json_object_array_add(sources, json_object_new_string(ProblemSourceCatalog::source_handle(i).c_str()));
    json_object_object_add(definitions, "source_handle", handles);
    auto pieces = json_object_new_object(), piece_names = json_object_new_array();
    json_object_object_add(pieces, "enum", piece_names);
    for (std::size_t i = 0; i < catalog.entries().size(); ++i) {
        if (!catalog.entries()[i].whole_region)
            json_object_array_add(piece_names, json_object_new_string(ProblemSourceCatalog::source_handle(i).c_str()));
    }
    json_object_object_add(definitions, "piece_handle", pieces);
    const auto source_span = parse(R"({"anyOf":[{"$ref":"#/$defs/source_handle"},{"type":"array","minItems":2,"maxItems":2,"prefixItems":[{"$ref":"#/$defs/piece_handle"},{"$ref":"#/$defs/piece_handle"}]}]})");
    json_object_object_add(definitions, "span", json_object_get(source_span.get()));
    auto units = json_object_new_object(), unit_names = json_object_new_array();
    json_object_object_add(units, "enum", unit_names);
    for (std::size_t i = 0; i < catalog.context().units.size(); ++i)
        json_object_array_add(unit_names, json_object_new_string(("u" + std::to_string(i)).c_str()));
    json_object_object_add(definitions, "unit_handle", units);
    const auto unit_schema = parse(R"({"$ref":"#/$defs/unit_handle"})");
    json_object_array_put_idx(field(field(definitions, "quantity"), "prefixItems"), 3,
        json_object_get(unit_schema.get()));
    json_object_array_put_idx(field(field(definitions, "number"), "prefixItems"), 2,
        json_object_get(unit_schema.get()));
    const auto quantity_goal = at(field(field(definitions, "goal"), "anyOf"), 0);
    json_object_array_put_idx(field(quantity_goal, "prefixItems"), 2, json_object_get(unit_schema.get()));
    auto proposal = json_object_new_object();
    json_object_object_add(proposal, "oneOf", json_object_get(field(schema.get(), "oneOf")));
    json_object_object_del(schema.get(), "oneOf");
    json_object_object_add(definitions, "proposal", proposal);
    const auto envelope = parse(R"({"type":"object","additionalProperties":false,"required":["snapshot","proposal"],"properties":{"snapshot":{},"proposal":{"$ref":"#/$defs/proposal"}}})");
    auto snapshot = field(field(envelope.get(), "properties"), "snapshot");
    json_object_object_add(snapshot, "const", json_object_new_string(catalog.snapshot().c_str()));
    for (const auto name : {"type", "additionalProperties", "required", "properties"})
        json_object_object_add(schema.get(), name, json_object_get(field(envelope.get(), name)));
    return json_object_to_json_string_ext(schema.get(), JSON_C_TO_STRING_PLAIN);
}

std::string problem_source_semantic_prompt() {
    auto result = problem_semantic_prompt(ProblemSelectorMode::OffsetV1);
    const std::string old = "All offsets and lengths are UTF-8 bytes in the host evidence views. Units are host indices; never redefine them.";
    result.replace(result.find(old), old.size(),
        "Select source handles from the host catalog. Units are host UnitRefs such as u0; never redefine them.");
    const std::string old_span = "span=[view,offset,length,locator-or-null]";
    result.replace(result.find(old_span), old_span.size(),
        "span=SourceRef such as \"s7\", or [first-piece-SourceRef,last-piece-SourceRef]");
    result += "\nReturn {\"snapshot\":the supplied snapshot,\"proposal\":the proposal or decline object}. "
        "Catalog sources are [SourceRef,view,locator-or-null,region-or-piece,text]. "
        "A range includes both endpoint pieces and all intervening bytes in one locator. "
        "A region handle selects the whole region and cannot be a range endpoint. "
        "All entity, quantity, node, predicate and variable indices remain zero-based array indices. "
        "Quantity units, number units and quantity-goal units must instead use host UnitRefs. "
        "The context contains the full evidence and registry; unit_refs maps UnitRefs to that registry.";
    return result;
}

} // namespace axiom
