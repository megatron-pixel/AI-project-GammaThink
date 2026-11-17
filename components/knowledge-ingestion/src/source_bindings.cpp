#include "source_bindings.hpp"
#include "archive_dsl/codec.hpp"

#include <json-c/json.h>

namespace axiom::knowledge {
namespace {
using Json = std::unique_ptr<json_object, decltype(&json_object_put)>;
void require(bool value, ErrorCode code, const char* message) {
    if (!value) throw Error(code, message);
}
constexpr std::string_view format = "gammathink.source-bindings.v1";
Json parse(std::string_view input) {
    require(input.size() <= maximum_object_bytes, ErrorCode::Budget, "Source binding wire exceeds its limit");
    std::unique_ptr<json_tokener, decltype(&json_tokener_free)> parser(json_tokener_new_ex(16), json_tokener_free);
    require(bool(parser), ErrorCode::Storage, "Cannot allocate source binding parser");
    json_tokener_set_flags(parser.get(), JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
    Json result(json_tokener_parse_ex(parser.get(), input.data(), static_cast<int>(input.size())), json_object_put);
    require(result && json_tokener_get_error(parser.get()) == json_tokener_success &&
        json_tokener_get_parse_end(parser.get()) == input.size(), ErrorCode::Schema, "Invalid source binding JSON");
    return result;
}
void tuple(json_object* value, std::size_t size) {
    require(value && json_object_is_type(value, json_type_array) && json_object_array_length(value) == size,
            ErrorCode::Schema, "Invalid source binding tuple");
}
std::string text(json_object* value) {
    require(value && json_object_is_type(value, json_type_string), ErrorCode::Schema, "Expected source binding text");
    return {json_object_get_string(value), static_cast<std::size_t>(json_object_get_string_len(value))};
}
std::uint32_t index(json_object* value) {
    require(value && json_object_is_type(value, json_type_int) && json_object_get_int64(value) >= 0 &&
        json_object_get_uint64(value) <= UINT32_MAX, ErrorCode::Schema, "Invalid source symbol index");
    return static_cast<std::uint32_t>(json_object_get_uint64(value));
}
std::string_view kind(SourceSymbolKind value) {
    switch (value) {
    case SourceSymbolKind::Entity: return "entity";
    case SourceSymbolKind::Predicate: return "predicate";
    case SourceSymbolKind::Quantity: return "quantity";
    }
    throw Error(ErrorCode::Schema, "Invalid source symbol kind");
}
json_object* symbol_json(const SourceSymbol& symbol) {
    auto* value = json_object_new_array();
    json_object_array_add(value, json_object_new_string(hex(symbol.module).c_str()));
    json_object_array_add(value, json_object_new_string(kind(symbol.kind).data()));
    json_object_array_add(value, json_object_new_uint64(symbol.index));
    return value;
}
SourceSymbol symbol(json_object* value) {
    tuple(value, 3);
    const auto name = text(json_object_array_get_idx(value, 1));
    const auto type = name == "entity" ? SourceSymbolKind::Entity :
        name == "predicate" ? SourceSymbolKind::Predicate : SourceSymbolKind::Quantity;
    require(name == kind(type), ErrorCode::Schema, "Unknown source symbol kind");
    return {parse_id(text(json_object_array_get_idx(value, 0))), type, index(json_object_array_get_idx(value, 2))};
}
std::vector<SourceIdentityBinding> decode(const Object& object) {
    require(object.kind == ObjectKind::Assumption && object.format == format && !object.derivation,
            ErrorCode::Schema, "Expected an explicit source binding assumption");
    const auto root = parse(std::string_view(reinterpret_cast<const char*>(object.payload.data()), object.payload.size()));
    tuple(root.get(), 2);
    require(text(json_object_array_get_idx(root.get(), 0)) == format, ErrorCode::Schema, "Source binding profile differs");
    auto* rows = json_object_array_get_idx(root.get(), 1);
    require(rows && json_object_is_type(rows, json_type_array) &&
        json_object_array_length(rows) > 0 && json_object_array_length(rows) <= 64,
        ErrorCode::Budget, "Source binding count exceeds its bound");
    std::vector<SourceIdentityBinding> result;
    for (std::size_t i = 0; i < json_object_array_length(rows); ++i) {
        auto* row = json_object_array_get_idx(rows, i); tuple(row, 2);
        result.push_back({symbol(json_object_array_get_idx(row, 0)), symbol(json_object_array_get_idx(row, 1))});
    }
    return result;
}
}

Object propose_source_bindings(std::span<const SourceBindingScope> scopes,
    std::span<const SourceIdentityBinding> bindings, std::string reason) {
    require(!scopes.empty() && scopes.size() <= 16 && !bindings.empty() && bindings.size() <= 64,
            ErrorCode::Budget, "Source binding scope or row bound exceeded");
    validate_text(reason);
    require(!reason.empty(), ErrorCode::Schema, "Source binding requires its explicit assumption rationale");
    std::map<ObjectId, const SourceBindingScope*> by_id;
    std::map<ObjectId, Json> theories;
    for (const auto& scope : scopes) {
        const auto id = identify(scope.module);
        require(by_id.emplace(id, &scope).second, ErrorCode::Schema, "Duplicate source binding scope");
        auto candidate = scope.module;
        require(candidate.links.size() == 2, ErrorCode::Integrity, "Source binding module lacks its context");
        candidate.links.pop_back();
        const auto mode = scope.context.format == "axiom.problem-context.offset-v1.json"
            ? ProblemSelectorMode::OffsetV1 : ProblemSelectorMode::QuoteV1;
        const auto checked = materialize_source(scope.source, candidate, scope.host_context, {}, {}, mode);
        require(checked.module == scope.module && checked.context == scope.context, ErrorCode::Integrity,
                "Source binding scope differs from host-owned native context");
        const auto parsed = gammathink::archive_dsl::decode(
            std::string(scope.module.payload.begin(), scope.module.payload.end()));
        theories.emplace(id, parse(parsed));
    }
    std::set<ObjectId> used;
    const auto item = [&](const SourceSymbol& endpoint) {
        const auto found = theories.find(endpoint.module);
        require(found != theories.end(), ErrorCode::Missing, "Source binding refers to an unprovided module");
        used.insert(endpoint.module);
        auto* theory = json_object_object_get(found->second.get(), "theory");
        auto* values = json_object_object_get(theory, endpoint.kind == SourceSymbolKind::Entity ? "entities" :
            endpoint.kind == SourceSymbolKind::Predicate ? "predicates" : "quantities");
        (void)kind(endpoint.kind);
        require(endpoint.index < json_object_array_length(values), ErrorCode::Schema, "Source binding symbol is outside its scope");
        return json_object_array_get_idx(values, endpoint.index);
    };
    for (const auto& binding : bindings) {
        require(binding.left.module != binding.right.module && binding.left.kind == binding.right.kind,
                ErrorCode::Schema, "Source binding requires matching kinds in distinct modules");
        auto* left = item(binding.left); auto* right = item(binding.right);
        if (binding.left.kind == SourceSymbolKind::Predicate) {
            require(index(json_object_array_get_idx(left, 1)) == index(json_object_array_get_idx(right, 1)),
                    ErrorCode::Schema, "Source binding predicate arities differ");
        } else if (binding.left.kind == SourceSymbolKind::Quantity) {
            const SourceSymbol left_entity{binding.left.module, SourceSymbolKind::Entity,
                index(json_object_array_get_idx(left, 1))};
            const SourceSymbol right_entity{binding.right.module, SourceSymbolKind::Entity,
                index(json_object_array_get_idx(right, 1))};
            bool entity_bound = false;
            for (const auto& row : bindings) entity_bound |=
                (row.left == left_entity && row.right == right_entity) ||
                (row.left == right_entity && row.right == left_entity);
            require(entity_bound, ErrorCode::Schema, "Quantity identity requires explicit entity identity");
            auto* left_scope = json_object_array_get_idx(left, 2);
            auto* right_scope = json_object_array_get_idx(right, 2);
            require(json_object_get_boolean(json_object_array_get_idx(left_scope, 0)) &&
                    json_object_get_boolean(json_object_array_get_idx(right_scope, 0)) &&
                    json_object_equal(left_scope, right_scope),
                    ErrorCode::Schema, "Quantity identity requires matching resolved scopes");
            const auto& left_unit = by_id.at(binding.left.module)->host_context.units.at(index(json_object_array_get_idx(left, 3)));
            const auto& right_unit = by_id.at(binding.right.module)->host_context.units.at(index(json_object_array_get_idx(right, 3)));
            require(left_unit.dimension == right_unit.dimension, ErrorCode::Schema, "Quantity identity dimensions differ");
        }
    }
    require(used.size() == scopes.size(), ErrorCode::Schema, "Source binding includes unused scopes");
    Object result;
    result.kind = ObjectKind::Assumption; result.label = std::move(reason); result.format = format;
    Json root(json_object_new_array(), json_object_put);
    json_object_array_add(root.get(), json_object_new_string(format.data()));
    auto* rows = json_object_new_array(); json_object_array_add(root.get(), rows);
    for (const auto& binding : bindings) {
        auto* row = json_object_new_array();
        json_object_array_add(row, symbol_json(binding.left)); json_object_array_add(row, symbol_json(binding.right));
        json_object_array_add(rows, row);
    }
    const std::string wire = json_object_to_json_string_ext(root.get(), JSON_C_TO_STRING_PLAIN);
    result.payload.assign(wire.begin(), wire.end());
    for (const auto& [id, scope] : by_id) {
        result.links.push_back({Relationship::DependsOn, id});
        result.sources.insert(result.sources.end(), scope->source.sources.begin(), scope->source.sources.end());
    }
    (void)encode_object(result);
    return result;
}

ActiveSourceBindings::ActiveSourceBindings(ObjectId assumption, std::vector<SourceIdentityBinding> bindings,
    std::vector<ObservedAssessment> observations)
    : assumption_(assumption), bindings_(std::move(bindings)), observations_(std::move(observations)) {}

ActiveSourceBindings activate_source_bindings(UnifiedKnowledge& knowledge, Journal& journal,
    const ObjectId& binding, const std::map<ObjectId, ProblemContext>& host_contexts,
    const std::set<ObjectId>& accepted, ResolutionLimits limits) {
    require(accepted.contains(binding), ErrorCode::Permission, "Source identity assumption has not been explicitly accepted");
    std::vector<SourceIdentityBinding> rows;
    std::vector<ObservedAssessment> observations;
    journal.read_snapshot([&] {
        const auto closure = knowledge.resolve(binding, limits);
        require(closure.complete(), ErrorCode::Unavailable, "Source binding dependencies are unavailable");
        std::map<ObjectId, const Object*> records;
        for (const auto& item : closure.objects) {
            const auto current = journal.assessment(item.id);
            const auto invalid = [](const std::optional<Assessment>& value) {
                return value && (value->state == KnowledgeState::Invalidated || value->logical == LogicalValidity::Invalid);
            };
            if (invalid(current) || invalid(item.record.assessment)) throw SourceBindingRevalidation(item.id);
            observations.push_back({item.id, current ? current->version : 0});
            records.emplace(item.id, &item.record.object);
        }
        const auto found = records.find(binding);
        require(found != records.end(), ErrorCode::Integrity, "Source binding closure lost its root");
        const auto& object = *found->second;
        rows = decode(object);
        std::set<ObjectId> modules;
        for (const auto& row : rows) { modules.insert(row.left.module); modules.insert(row.right.module); }
        std::vector<SourceBindingScope> scopes;
        for (const auto& id : modules) {
            const auto module = records.find(id);
            require(module != records.end() && host_contexts.contains(id), ErrorCode::Missing,
                    "Source binding lacks its module or independent host context");
            const auto& links = module->second->links;
            require(links.size() == 2 && records.contains(links[0].target) && records.contains(links[1].target),
                    ErrorCode::Integrity, "Source binding scope dependency differs");
            scopes.push_back({*records.at(links[0].target), *module->second, *records.at(links[1].target),
                              host_contexts.at(id)});
        }
        require(propose_source_bindings(scopes, rows, object.label) == object, ErrorCode::Integrity,
                "Source binding record differs from its canonical scoped contract");
    });
    return ActiveSourceBindings(binding, std::move(rows), std::move(observations));
}

} // namespace axiom::knowledge
