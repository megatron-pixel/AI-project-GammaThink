#include "problem_transaction.hpp"
#include "problem_semantic.hpp"

#include <json-c/json.h>

#include <array>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>

namespace axiom {
namespace {
using J = json_object*;
using Json = std::unique_ptr<json_object, decltype(&json_object_put)>;
void require(bool condition, const char* message) {
    if (!condition) throw std::invalid_argument(message);
}
Json parse(const std::string& input) {
    require(!input.empty() && input.size() <= 131072 && input.find('\0') == std::string::npos,
        "Invalid transaction size");
    const auto tokener = std::unique_ptr<json_tokener, decltype(&json_tokener_free)>(
        json_tokener_new_ex(48), json_tokener_free);
    json_tokener_set_flags(tokener.get(), JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
    Json value(json_tokener_parse_ex(tokener.get(), input.data(), static_cast<int>(input.size())), json_object_put);
    require(value && json_tokener_get_error(tokener.get()) == json_tokener_success
        && json_object_get_type(value.get()) == json_type_object, "Malformed transaction JSON");
    for (auto i = json_tokener_get_parse_end(tokener.get()); i < input.size(); ++i)
        require(input[i] == ' ' || input[i] == '\t' || input[i] == '\r' || input[i] == '\n',
            "Trailing transaction JSON");
    return value;
}
J field(J value, const char* key) {
    J result = nullptr;
    require(json_object_get_type(value) == json_type_object
        && json_object_object_get_ex(value, key, &result), "Missing transaction field");
    return result;
}
void fields(J value, std::initializer_list<const char*> keys) {
    require(json_object_get_type(value) == json_type_object
        && static_cast<std::size_t>(json_object_object_length(value)) == keys.size(),
        "Unexpected transaction fields");
    for (const auto key : keys) (void)field(value, key);
}
std::string string(J value, std::size_t maximum = 1024) {
    require(json_object_get_type(value) == json_type_string, "Invalid transaction string");
    std::string result(json_object_get_string(value), json_object_get_string_len(value));
    require(!result.empty() && result.size() <= maximum && result.find('\0') == std::string::npos,
        "Invalid transaction text");
    return result;
}
std::size_t count(J value, std::size_t maximum) {
    require(json_object_get_type(value) == json_type_array, "Invalid transaction array");
    const auto size = json_object_array_length(value);
    require(size <= maximum, "Transaction array bound exceeded");
    return size;
}
J at(J value, std::size_t index) {
    require(index < count(value, 256), "Incomplete transaction tuple");
    return json_object_array_get_idx(value, index);
}
void tuple(J value, std::size_t size) {
    require(count(value, size) == size, "Invalid transaction tuple");
}
bool boolean(J value) {
    require(json_object_get_type(value) == json_type_boolean, "Invalid transaction boolean");
    return json_object_get_boolean(value);
}
std::string dump(J value) { return json_object_to_json_string_ext(value, JSON_C_TO_STRING_PLAIN); }
void envelope(J root, const ProblemSourceCatalog& catalog, std::initializer_list<const char*> keys) {
    require(string(field(root, "snapshot")) == catalog.snapshot(), "Stale transaction snapshot");
    J status = nullptr;
    if (json_object_object_get_ex(root, "status", &status)) {
        fields(root, {"snapshot", "status", "reason"});
        const auto kind = string(status);
        require(kind == "clarify" || kind == "unsupported", "Invalid transaction status");
        throw ProblemDecline(kind == "clarify" ? ProblemDeclineKind::Clarify : ProblemDeclineKind::Unsupported,
            string(field(root, "reason")));
    }
    fields(root, keys);
}

enum Kind { Entity, Scope, Property, Quantity, Number, PredicateAlias, Node, KindCount };
using Aliases = std::map<std::string, std::uint32_t>;
class Compiler {
public:
    const ProblemSourceCatalog& catalog;
    ProblemRequest request;
    std::array<Aliases, KindCount> aliases;
    std::vector<ProblemScope> scopes;
    std::vector<std::uint32_t> arities;
    std::vector<std::uint32_t> native_nodes;
    std::map<std::pair<std::uint32_t,std::uint32_t>,std::uint32_t> scoped_predicates;

    explicit Compiler(const ProblemSourceCatalog& source)
        : catalog(source), request{source.context(), {}, source.limits()} {}
    std::uint32_t declare(Kind kind, J name) {
        auto& table = aliases[kind];
        const auto index = static_cast<std::uint32_t>(table.size());
        require(table.emplace(string(name,64), index).second, "Duplicate typed alias");
        return index;
    }
    std::uint32_t ref(Kind kind, J name) const {
        const auto& table = aliases[kind];
        const auto found = table.find(string(name,64));
        require(found != table.end(), "Unknown or cross-type alias");
        return found->second;
    }
    EvidenceSpan span(J value) const {
        if (json_object_get_type(value) == json_type_string)
            return catalog.resolve(catalog.snapshot(), string(value,64));
        tuple(value,2);
        return catalog.resolve(catalog.snapshot(), string(at(value,0),64), string(at(value,1),64));
    }
    ProblemOrigin origin(J value, const std::string& label) const {
        ProblemOrigin result{PremiseBasis::Question, label, {}};
        const auto size = count(value,16);
        require(size > 0, "Uncited transaction interpretation");
        for (std::size_t i = 0; i < size; ++i) {
            const auto support = span(at(value,i));
            if (catalog.context().views[support.view].basis == PremiseBasis::Source)
                result.basis = PremiseBasis::Source;
            result.supports.push_back(support);
        }
        return result;
    }
    void declarations(J root) {
        fields(root, {"entities","scopes","properties","quantities","numbers","predicates"});
        const auto each = [&](const char* key, std::size_t maximum, auto action) {
            const auto values = field(root,key);
            for (std::size_t i = 0, n = count(values,maximum); i < n; ++i) action(at(values,i));
        };
        each("entities",64,[&](J item) {
            tuple(item,3);
            const auto index = declare(Entity,at(item,0));
            (void)string(at(item,1)); (void)origin(at(item,2),"entity");
            request.proposal.entities.push_back("e"+std::to_string(index));
        });
        bool global = false;
        each("scopes",64,[&](J item) {
            tuple(item,3);
            const auto index = declare(Scope,at(item,0));
            ProblemScope scope{boolean(at(item,1)),{}};
            const auto qualifiers = at(item,2);
            const auto size = count(qualifiers,16);
            std::set<std::string> keys;
            for (std::size_t i = 0; i < size; ++i) {
                const auto qualifier = at(qualifiers,i);
                tuple(qualifier,3);
                require(keys.insert(string(at(qualifier,0))).second, "Duplicate scope qualifier");
                (void)string(at(qualifier,1)); (void)origin(at(qualifier,2),"scope");
            }
            if (size) scope.qualifiers.emplace_back("scope","s"+std::to_string(index));
            else if (scope.resolved) {
                require(!global, "Duplicate explicit global scope");
                global = true;
            } else scope.qualifiers.emplace_back("scope","s"+std::to_string(index));
            scopes.push_back(std::move(scope));
        });
        each("properties",64,[&](J item) {
            tuple(item,3);
            (void)declare(Property,at(item,0));
            (void)string(at(item,1)); (void)origin(at(item,2),"property");
        });
        each("quantities",64,[&](J item) {
            tuple(item,6);
            (void)declare(Quantity,at(item,0));
            request.proposal.quantities.push_back({"p"+std::to_string(ref(Property,at(item,2))),
                ref(Entity,at(item,1)),scopes.at(ref(Scope,at(item,3))),
                catalog.unit(catalog.snapshot(),string(at(item,4),64))});
            (void)origin(at(item,5),"quantity");
        });
        each("numbers",128,[&](J item) {
            tuple(item,6);
            (void)declare(Number,at(item,0));
            const auto precision = string(at(item,4));
            require(precision == "exact" || precision == "approximate" || precision == "bound"
                || precision == "unspecified", "Invalid transaction precision");
            request.proposal.numbers.push_back({span(at(item,1)),
                json_object_get_type(at(item,2)) == json_type_null ? std::nullopt : std::optional{span(at(item,2))},
                catalog.unit(catalog.snapshot(),string(at(item,3),64)),
                precision == "exact" ? QuantityPrecision::Exact : precision == "approximate"
                    ? QuantityPrecision::Approximate : precision == "bound"
                    ? QuantityPrecision::Bound : QuantityPrecision::Unspecified,
                json_object_get_type(at(item,5)) == json_type_null ? std::nullopt : std::optional{span(at(item,5))}});
        });
        each("predicates",64,[&](J item) {
            tuple(item,4);
            (void)declare(PredicateAlias,at(item,0));
            (void)string(at(item,1)); (void)origin(at(item,3),"predicate");
            const auto arity = at(item,2);
            require(json_object_get_type(arity) == json_type_int && json_object_get_int64(arity) >= 0
                && json_object_get_int64(arity) <= 4, "Invalid transaction predicate arity");
            arities.push_back(static_cast<std::uint32_t>(json_object_get_int64(arity)));
        });
    }
    Atom atom(J value, const Aliases& variables) {
        tuple(value,4);
        const auto predicate = ref(PredicateAlias,at(value,0)), scope = ref(Scope,at(value,1));
        require(scopes[scope].resolved, "Unresolved logical scope");
        const auto key = std::pair{predicate,scope};
        auto found = scoped_predicates.find(key);
        if (found == scoped_predicates.end()) {
            const auto index = static_cast<std::uint32_t>(request.proposal.logic.predicates.size());
            require(index < 64, "Scoped predicate limit exceeded");
            request.proposal.logic.predicates.push_back({"p"+std::to_string(predicate)+"s"+std::to_string(scope),
                arities[predicate]});
            found = scoped_predicates.emplace(key,index).first;
        }
        Atom result{found->second,boolean(at(value,2)),{}};
        const auto terms = at(value,3);
        require(count(terms,4) == arities[predicate], "Typed atom arity mismatch");
        for (std::size_t i = 0; i < arities[predicate]; ++i) {
            const auto term = at(terms,i);
            tuple(term,2);
            const auto tag = string(at(term,0));
            if (tag == "c") result.terms.push_back({TermKind::Constant,ref(Entity,at(term,1))});
            else {
                require(tag == "v", "Invalid typed term");
                const auto variable = variables.find(string(at(term,1),64));
                require(variable != variables.end(), "Unknown rule-local variable");
                result.terms.push_back({TermKind::Variable,variable->second});
            }
        }
        return result;
    }
    void assertions(J root) {
        fields(root,{"bindings","nodes","equations","rules","goal"});
        const auto bindings = field(root,"bindings");
        for (std::size_t i = 0, n = count(bindings,128); i < n; ++i) {
            const auto item = at(bindings,i);
            tuple(item,3);
            request.proposal.bindings.push_back({ref(Quantity,at(item,0)),ref(Number,at(item,1)),
                origin(at(item,2),"binding_"+std::to_string(i))});
        }
        const auto nodes = field(root,"nodes");
        const auto node_count = count(nodes,256);
        for (std::size_t i = 0; i < node_count; ++i) {
            require(count(at(nodes,i),4) >= 3, "Incomplete typed node");
            (void)declare(Node,at(at(nodes,i),0));
        }
        std::vector<unsigned> visiting(node_count);
        native_nodes.resize(node_count);
        const std::function<std::uint32_t(std::uint32_t)> compile_node = [&](std::uint32_t index) {
            require(visiting[index] != 1, "Cyclic typed expression");
            if (visiting[index] == 2) return native_nodes[index];
            visiting[index] = 1;
            const auto item = at(nodes,index);
            const auto tag = string(at(item,1));
            ProblemNode node;
            if (tag == "q" || tag == "n" || tag == "neg") {
                tuple(item,3);
                if (tag == "q") node = ProblemQuantityNode{ref(Quantity,at(item,2))};
                else if (tag == "n") node = ProblemNumberNode{ref(Number,at(item,2))};
                else node = ProblemNegateNode{compile_node(ref(Node,at(item,2)))};
            } else {
                tuple(item,4);
                require(tag == "add" || tag == "sub" || tag == "mul" || tag == "div",
                    "Invalid typed arithmetic operation");
                const auto left = compile_node(ref(Node,at(item,2)));
                const auto right = compile_node(ref(Node,at(item,3)));
                node = ProblemBinaryNode{tag == "add" ? Code::Add : tag == "sub" ? Code::Subtract
                    : tag == "mul" ? Code::Multiply : Code::Divide,left,right};
            }
            native_nodes[index] = static_cast<std::uint32_t>(request.proposal.nodes.size());
            request.proposal.nodes.push_back(node);
            visiting[index] = 2;
            return native_nodes[index];
        };
        for (std::size_t i = 0; i < node_count; ++i) (void)compile_node(i);
        const auto equations = field(root,"equations");
        for (std::size_t i = 0, n = count(equations,64); i < n; ++i) {
            const auto item = at(equations,i);
            tuple(item,5);
            request.proposal.equations.push_back({native_nodes.at(ref(Node,at(item,0))),
                native_nodes.at(ref(Node,at(item,1))),scopes.at(ref(Scope,at(item,2))),
                json_object_get_type(at(item,3)) == json_type_null ? std::nullopt
                    : std::optional{atom(at(item,3),{})},origin(at(item,4),"equation_"+std::to_string(i))});
        }
        const auto rules = field(root,"rules");
        for (std::size_t i = 0, n = count(rules,128); i < n; ++i) {
            const auto item = at(rules,i);
            tuple(item,4);
            const auto declared = at(item,0);
            Aliases variables;
            for (std::size_t j = 0, m = count(declared,8); j < m; ++j)
                require(variables.emplace(string(at(declared,j),64),j).second, "Duplicate rule variable");
            RelationalRule rule{static_cast<std::uint32_t>(variables.size()),atom(at(item,1),variables),{},
                "rule_"+std::to_string(i)};
            const auto premises = at(item,2);
            for (std::size_t j = 0, m = count(premises,16); j < m; ++j)
                rule.premises.push_back(atom(at(premises,j),variables));
            request.proposal.logic_origins.push_back(origin(at(item,3),rule.provenance));
            request.proposal.logic.rules.push_back(std::move(rule));
        }
        goal(field(root,"goal"));
        if (!request.proposal.logic.predicates.empty()) {
            request.proposal.logic.name = "typed_interpretation";
            request.proposal.logic.version = "1";
            request.proposal.logic.constants = request.proposal.entities;
        }
    }
    void goal(J goal) {
        const auto tag = string(at(goal,0));
        if (tag == "q") {
            tuple(goal,4);
            request.proposal.goal = ProblemQuantityGoal{ref(Quantity,at(goal,1)),
                catalog.unit(catalog.snapshot(),string(at(goal,2),64))};
            (void)origin(at(goal,3),"goal");
        } else {
            require(tag == "atom", "Invalid typed goal");
            tuple(goal,3);
            request.proposal.goal = ProblemAtomGoal{atom(at(goal,1),{})};
            (void)origin(at(goal,2),"goal");
        }
    }
    CompiledProblemTransaction finish(J declarations, J assertions, bool batch_item = false) {
        // Canonical serialization performs ordinary admission, never execution.
        const auto proposal = serialize_problem_proposal(request);
        auto digest = evidence_digest("typed-transaction-v1\n"+catalog.snapshot()+"\n"+proposal);
        if (batch_item) {
            digest = evidence_digest("typed-signed-batch-item-v1\n"+catalog.snapshot()+"\n"+proposal
                +"\n"+std::to_string(request.limits.work)+"\n"+std::to_string(request.limits.steps));
        }
        Json record(json_object_new_object(),json_object_put);
        json_object_object_add(record.get(),"version",json_object_new_string(
            batch_item ? "typed-signed-batch-item-v1" : "typed-transaction-v1"));
        json_object_object_add(record.get(),"snapshot",json_object_new_string(catalog.snapshot().c_str()));
        json_object_object_add(record.get(),"request_digest",json_object_new_string(digest.c_str()));
        json_object_object_add(record.get(),"declarations",json_object_get(declarations));
        json_object_object_add(record.get(),"assertions",json_object_get(assertions));
        auto mapping = json_object_new_object();
        json_object_object_add(record.get(),"alias_indices",mapping);
        constexpr std::array names{"entities","scopes","properties","quantities","numbers","predicates","nodes"};
        for (std::size_t kind = 0; kind < KindCount; ++kind) {
            auto table = json_object_new_object();
            json_object_object_add(mapping,names[kind],table);
            for (const auto& [alias,index] : aliases[kind])
                json_object_object_add(table,alias.c_str(),json_object_new_int64(kind == Node
                    ? native_nodes.at(index) : index));
        }
        auto predicates = json_object_new_array();
        json_object_object_add(record.get(),"scoped_predicate_indices",predicates);
        for (const auto& [key,index] : scoped_predicates) {
            auto item = json_object_new_array();
            for (const auto value : {key.first,key.second,index})
                json_object_array_add(item,json_object_new_int64(value));
            json_object_array_add(predicates,item);
        }
        return {std::move(request),digest,dump(record.get())};
    }
};
}

ProblemDeclarationDraft prepare_problem_declarations(const std::string& json,
    const ProblemSourceCatalog& catalog) {
    const auto root = parse(json);
    envelope(root.get(),catalog,{"snapshot","declarations"});
    const auto declarations = field(root.get(),"declarations");
    Compiler compiler(catalog);
    compiler.declarations(declarations);
    return ProblemDeclarationDraft(catalog.snapshot(),dump(declarations));
}

CompiledProblemTransaction commit_problem_assertions(const std::string& json,
    const ProblemDeclarationDraft& draft, const ProblemSourceCatalog& catalog) {
    require(draft.snapshot() == catalog.snapshot(), "Draft belongs to another context");
    const auto root = parse(json), declarations = parse(draft.declarations());
    envelope(root.get(),catalog,{"snapshot","assertions"});
    Compiler compiler(catalog);
    compiler.declarations(declarations.get());
    const auto assertions = field(root.get(),"assertions");
    compiler.assertions(assertions);
    return compiler.finish(declarations.get(),assertions);
}

CompiledProblemTransaction compile_problem_transaction(const std::string& json,
    const ProblemSourceCatalog& catalog) {
    const auto root = parse(json);
    envelope(root.get(),catalog,{"snapshot","declarations","assertions"});
    Compiler compiler(catalog);
    const auto declarations = field(root.get(),"declarations"), assertions = field(root.get(),"assertions");
    compiler.declarations(declarations);
    compiler.assertions(assertions);
    return compiler.finish(declarations,assertions);
}

CompiledProblemBatch compile_problem_transaction_batch(const std::string& json,
    const ProblemSourceCatalog& catalog) {
    const auto root = parse(json);
    envelope(root.get(),catalog,{"snapshot","declarations","assertions"});
    const auto declarations = field(root.get(),"declarations"), assertions = field(root.get(),"assertions");
    fields(assertions,{"bindings","nodes","equations","rules","goals"});
    for (const auto key : {"properties","quantities","numbers"})
        require(count(field(declarations,key),0) == 0,"Arithmetic declarations in signed batch");
    for (const auto key : {"bindings","nodes","equations"})
        require(count(field(assertions,key),0) == 0,"Arithmetic assertions in signed batch");
    const auto goals = field(assertions,"goals");
    const auto size = count(goals,8);
    require(size > 0 && catalog.limits().work >= size,"Empty or under-budget signed batch");
    Compiler compiler(catalog);
    compiler.declarations(declarations);
    auto single = parse(dump(assertions));
    json_object_object_del(single.get(),"goals");
    json_object_object_add(single.get(),"goal",json_object_get(at(goals,0)));
    compiler.assertions(single.get());
    std::vector<ProblemGoal> compiled_goals;
    for (std::size_t i = 0; i < size; ++i) {
        const auto goal = at(goals,i);
        tuple(goal,3);
        require(string(at(goal,0)) == "atom","Non-atomic signed batch goal");
        const auto support = compiler.origin(at(goal,2),"goal");
        require(support.basis == PremiseBasis::Question,"Batch goal needs question-only supports");
        // Intern every goal's vocabulary before making any request copy.
        compiler.goal(goal);
        compiled_goals.push_back(compiler.request.proposal.goal);
    }
    auto theory = parse(serialize_problem_proposal(compiler.request));
    json_object_object_del(theory.get(),"goal");
    const auto theory_digest = evidence_digest("typed-signed-theory-v1\n"
        +catalog.snapshot()+"\n"+dump(theory.get()));
    const auto common = compiler.request;
    CompiledProblemBatch result{{},theory_digest,{}};
    Json record(json_object_new_object(),json_object_put);
    json_object_object_add(record.get(),"version",json_object_new_string("typed-signed-batch-v1"));
    json_object_object_add(record.get(),"snapshot",json_object_new_string(catalog.snapshot().c_str()));
    json_object_object_add(record.get(),"theory_digest",json_object_new_string(theory_digest.c_str()));
    json_object_object_add(record.get(),"total_work",json_object_new_int64(catalog.limits().work));
    json_object_object_add(record.get(),"transaction",json_object_get(root.get()));
    auto slots = json_object_new_array();
    json_object_object_add(record.get(),"goals",slots);
    for (std::size_t i = 0; i < size; ++i) {
        compiler.request = common;
        compiler.request.proposal.goal = compiled_goals[i];
        compiler.request.limits.work = catalog.limits().work/size
            + (i < catalog.limits().work%size);
        json_object_object_add(single.get(),"goal",json_object_get(at(goals,i)));
        result.transactions.push_back(compiler.finish(declarations,single.get(),true));
        auto slot = json_object_new_object();
        json_object_object_add(slot,"index",json_object_new_int64(i));
        json_object_object_add(slot,"work",json_object_new_int64(result.transactions.back().request.limits.work));
        json_object_object_add(slot,"request_digest",
            json_object_new_string(result.transactions.back().request_digest.c_str()));
        json_object_array_add(slots,slot);
    }
    result.interpretation_record = dump(record.get());
    return result;
}

} // namespace axiom
