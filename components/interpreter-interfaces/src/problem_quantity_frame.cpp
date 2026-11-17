#include "problem_quantity_frame.hpp"
#include "problem_semantic.hpp"

#include <json-c/json.h>

#include <map>
#include <memory>
#include <stdexcept>

namespace axiom {
namespace {
using J = json_object*;
using Json = std::unique_ptr<json_object, decltype(&json_object_put)>;
void require(bool condition, const char* message) {
    if (!condition) throw std::invalid_argument(message);
}
J text(const std::string& value) { return json_object_new_string_len(value.data(),value.size()); }
J array(std::initializer_list<J> values) {
    auto result = json_object_new_array();
    for (const auto value : values) json_object_array_add(result,value);
    return result;
}
std::string dump(J value) { return json_object_to_json_string_ext(value,JSON_C_TO_STRING_PLAIN); }

class Compiler {
    const ProblemSourceCatalog& catalog;
    J declarations, bindings;
    std::map<std::string,std::string> entities, properties, scopes, quantities;

    J list(const char* key) { return json_object_object_get(declarations,key); }
    std::string alias(std::map<std::string,std::string>& table, const std::string& key,
                      const char* prefix, bool& inserted) {
        const auto [it,fresh] = table.emplace(key,std::string(prefix)+std::to_string(table.size()));
        inserted = fresh;
        return it->second;
    }
    std::string span_key(const QuantityFrameSpan& span) const {
        const auto resolved = resolve(span);
        return std::to_string(resolved.view)+":"+std::to_string(resolved.offset)+":"
            +std::to_string(resolved.length)+":"+std::to_string(resolved.locator);
    }
    EvidenceSpan resolve(const QuantityFrameSpan& span) const {
        return catalog.resolve(catalog.snapshot(),span.first,
            span.last ? std::optional<std::string_view>{*span.last} : std::nullopt);
    }
    std::string label(const QuantityFrameSpan& span) const {
        const auto resolved = resolve(span);
        return catalog.context().views[resolved.view].text.substr(resolved.offset,resolved.length);
    }
    J selector(const QuantityFrameSpan& span) const {
        (void)resolve(span);
        return span.last ? array({text(span.first),text(*span.last)}) : text(span.first);
    }
    J supports(const QuantityFrameIdentity& identity) const {
        require(!identity.supports.empty() && identity.supports.size() <= 16,
            "Quantity frame requires bounded explicit supports");
        Json result(json_object_new_array(),json_object_put);
        for (const auto& span : identity.supports) json_object_array_add(result.get(),selector(span));
        return result.release();
    }
    std::string mention(const QuantityFrameSpan& span, bool entity) {
        auto& table = entity ? entities : properties;
        bool fresh = false;
        const auto name = alias(table,span_key(span),entity ? "e" : "p",fresh);
        if (fresh) json_object_array_add(list(entity ? "entities" : "properties"),
            array({text(name),text(label(span)),array({selector(span)})}));
        return name;
    }
    std::string scope(const QuantityFrameScope& value) {
        if (value.kind == QuantityFrameScopeKind::Unresolved)
            throw ProblemDecline(ProblemDeclineKind::Clarify,"Unresolved quantity frame scope");
        require(value.kind == QuantityFrameScopeKind::Global
            || value.kind == QuantityFrameScopeKind::Qualified,"Invalid quantity frame scope");
        const bool global = value.kind == QuantityFrameScopeKind::Global;
        require(global ? value.qualifiers.empty() : !value.qualifiers.empty(),
            "Scope kind contradicts quantity frame qualifiers");
        require(value.qualifiers.size() <= 16,"Too many quantity frame qualifiers");
        // Canonicalize order, never labels or meaning. Each qualifier retains
        // both occurrence identities and its cited key/value bytes.
        std::map<std::string,const QuantityFrameQualifier*> sorted;
        for (const auto& qualifier : value.qualifiers)
            require(sorted.emplace(span_key(qualifier.key),&qualifier).second,
                "Duplicate quantity frame qualifier key");
        Json qualifiers(json_object_new_array(),json_object_put);
        std::string key = global ? "global" : "qualified";
        for (const auto& [identity,qualifier] : sorted) {
            key += "|"+identity+"="+span_key(qualifier->value);
            json_object_array_add(qualifiers.get(),array({text(label(qualifier->key)),
                text(label(qualifier->value)),array({selector(qualifier->key),selector(qualifier->value)})}));
        }
        bool fresh = false;
        const auto name = alias(scopes,key,"s",fresh);
        if (fresh) json_object_array_add(list("scopes"),
            array({text(name),json_object_new_boolean(true),json_object_get(qualifiers.get())}));
        return name;
    }
public:
    Compiler(const ProblemSourceCatalog& source, J decl, J bound)
        : catalog(source), declarations(decl), bindings(bound) {}

    std::string quantity(const QuantityFrameIdentity& value) {
        (void)catalog.unit(catalog.snapshot(),value.unit);
        Json evidence(supports(value),json_object_put);
        const auto entity = mention(value.subject,true), property = mention(value.property,false);
        const auto scoped = scope(value.scope);
        bool fresh = false;
        const auto name = alias(quantities,entity+"|"+property+"|"+scoped,"q",fresh);
        if (fresh) json_object_array_add(list("quantities"),array({text(name),text(entity),
            text(property),text(scoped),text(value.unit),json_object_get(evidence.get())}));
        return name;
    }
    void observation(const QuantityFrameObservation& value, std::size_t index) {
        const auto name = quantity(value.quantity), number = "n"+std::to_string(index);
        const char* precision = nullptr;
        switch (value.precision) {
        case QuantityPrecision::Exact: precision = "exact"; break;
        case QuantityPrecision::Approximate: precision = "approximate"; break;
        case QuantityPrecision::Bound: precision = "bound"; break;
        case QuantityPrecision::Unspecified: precision = "unspecified"; break;
        default: throw std::invalid_argument("Invalid quantity frame precision");
        }
        Json numeral(selector(value.numeral),json_object_put);
        Json unit(value.unit_span ? selector(*value.unit_span) : nullptr,json_object_put);
        Json qualifier(value.precision_span ? selector(*value.precision_span) : nullptr,json_object_put);
        json_object_array_add(list("numbers"),array({text(number),json_object_get(numeral.get()),
            json_object_get(unit.get()),text(value.quantity.unit),json_object_new_string(precision),
            json_object_get(qualifier.get())}));
        json_object_array_add(bindings,array({text(name),text(number),supports(value.quantity)}));
    }
    J goal(const QuantityFrameIdentity& value) {
        const auto name = quantity(value);
        return array({text("q"),text(name),text(value.unit),supports(value)});
    }
};
}

CompiledProblemTransaction compile_problem_quantity_frame(
    const ProblemQuantityFrame& frame, const ProblemSourceCatalog& catalog) {
    require(frame.snapshot == catalog.snapshot(),"Stale quantity frame snapshot");
    require(frame.observations.size() <= 64,"Too many quantity frame observations");
    Json root(json_object_new_object(),json_object_put);
    auto declarations = json_object_new_object(), assertions = json_object_new_object();
    json_object_object_add(root.get(),"snapshot",text(frame.snapshot));
    json_object_object_add(root.get(),"declarations",declarations);
    json_object_object_add(root.get(),"assertions",assertions);
    for (const auto key : {"entities","scopes","properties","quantities","numbers","predicates"})
        json_object_object_add(declarations,key,json_object_new_array());
    for (const auto key : {"bindings","nodes","equations","rules"})
        json_object_object_add(assertions,key,json_object_new_array());
    Compiler compiler(catalog,declarations,json_object_object_get(assertions,"bindings"));
    for (std::size_t i = 0; i < frame.observations.size(); ++i)
        compiler.observation(frame.observations[i],i);
    json_object_object_add(assertions,"goal",compiler.goal(frame.target));
    auto result = compile_problem_transaction(dump(root.get()),catalog);
    // Preserve the lowered choices without confusing this arm with a model's
    // authored transaction. The request digest remains the native transaction's.
    Json record(json_tokener_parse(result.interpretation_record.c_str()),json_object_put);
    json_object_object_add(record.get(),"adapter",text("source-quantity-frame-v1"));
    result.interpretation_record = dump(record.get());
    return result;
}

} // namespace axiom
