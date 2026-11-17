#include "problem_signed_decisions.hpp"
#include "problem_signed_plan.hpp"
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
std::string dump(J value) {
    return json_object_to_json_string_ext(value,JSON_C_TO_STRING_PLAIN);
}
J text(const std::string& value) {
    return json_object_new_string_len(value.data(),static_cast<int>(value.size()));
}
J get(J value, const char* key) { return json_object_object_get(value,key); }
Json parse(const std::string& input) {
    signed_plan_detail::validate_json(input);
    return Json(json_tokener_parse(input.c_str()),json_object_put);
}
std::string string(J value) {
    require(json_object_get_type(value) == json_type_string,"Signed policy requires text");
    std::string result(json_object_get_string(value),json_object_get_string_len(value));
    require(!result.empty() && result.size() <= 4096 && result.find('\0') == std::string::npos,
        "Invalid signed policy text");
    return result;
}
J ref_schema(const char* name) {
    auto result = json_object_new_object();
    json_object_object_add(result,"$ref",text(std::string("#/$defs/")+name));
    return result;
}
J enum_schema(std::initializer_list<const char*> values) {
    auto result = json_object_new_object(), options = json_object_new_array();
    for (const auto value : values) json_object_array_add(options,text(value));
    json_object_object_add(result,"enum",options);
    return result;
}

class Controller {
    const ProblemSourceCatalog& catalog;
    const SignedDecisionPolicy& policy;
    Json schema, plan, history;
    std::string question;

    Json ask(const std::string& role, J selection_schema) {
        Json envelope(json_object_new_object(),json_object_put);
        auto properties = json_object_new_object(), required = json_object_new_array();
        json_object_object_add(properties,"value",selection_schema);
        json_object_array_add(required,text("value"));
        json_object_object_add(envelope.get(),"type",text("object"));
        json_object_object_add(envelope.get(),"additionalProperties",json_object_new_boolean(false));
        json_object_object_add(envelope.get(),"required",required);
        json_object_object_add(envelope.get(),"properties",properties);
        json_object_object_add(envelope.get(),"$defs",json_object_get(get(schema.get(),"$defs")));
        const SignedDecisionRequest request{role,question,dump(envelope.get()),dump(plan.get()),
            catalog.input(),dump(history.get())};
        auto response = parse(policy(request));
        J value = nullptr;
        require(json_object_get_type(response.get()) == json_type_object
            && json_object_object_length(response.get()) == 1
            && json_object_object_get_ex(response.get(),"value",&value),
            "Signed policy requires exactly one value envelope");
        auto entry = json_object_new_object();
        json_object_object_add(entry,"role",text(role));
        json_object_object_add(entry,"value",json_object_get(value));
        json_object_array_add(history.get(),entry);
        return Json(json_object_get(value),json_object_put);
    }
    Json select_enum(const std::string& role, std::initializer_list<const char*> values) {
        auto value = ask(role,enum_schema(values));
        const auto selected = string(value.get());
        bool found = false;
        for (const auto option : values) found = found || selected == option;
        require(found,"Invalid signed policy enum selection");
        return value;
    }
    Json select_integer(const std::string& role, std::size_t size) {
        require(size > 0,"No declared references for signed policy role");
        auto specification = json_object_new_object();
        json_object_object_add(specification,"type",text("integer"));
        json_object_object_add(specification,"minimum",json_object_new_int(0));
        json_object_object_add(specification,"maximum",json_object_new_int64(size-1));
        auto value = ask(role,specification);
        require(json_object_get_type(value.get()) == json_type_int
            && json_object_get_int64(value.get()) >= 0
            && static_cast<std::uint64_t>(json_object_get_int64(value.get())) < size,
            "Invalid signed policy integer/reference selection");
        return value;
    }
    // The array and the in-progress object are attached before asking. A callback
    // sees exactly its own earlier selections, never proposed or gold content.
    template<class Action>
    void enumerate(J owner, const char* key, const std::string& path,
                   std::size_t minimum, std::size_t maximum, Action action) {
        auto list = json_object_new_array();
        json_object_object_add(owner,key,list);
        while (true) {
            const auto choice = select_enum(path+".next",{"append","stop"});
            const auto size = json_object_array_length(list);
            if (string(choice.get()) == "stop") {
                require(size >= minimum,"Signed policy stopped required list early");
                return;
            }
            require(size < maximum,"Signed policy list limit exceeded");
            action(list);
        }
    }
    Json span(const std::string& path) {
        auto first = ask(path+".first",ref_schema("source_handle"));
        const auto first_handle = string(first.get());
        (void)catalog.resolve(catalog.snapshot(),first_handle);
        auto specification = json_object_new_object(), alternatives = json_object_new_array();
        auto null_schema = json_object_new_object();
        json_object_object_add(null_schema,"type",text("null"));
        json_object_array_add(alternatives,ref_schema("source_handle"));
        json_object_array_add(alternatives,null_schema);
        json_object_object_add(specification,"anyOf",alternatives);
        auto last = ask(path+".last",specification);
        if (!last) return first;
        (void)catalog.resolve(catalog.snapshot(),first_handle,string(last.get()));
        Json result(json_object_new_array(),json_object_put);
        json_object_array_add(result.get(),first.release());
        json_object_array_add(result.get(),last.release());
        return result;
    }
    void supports(J owner, const std::string& path, bool question_only = false) {
        enumerate(owner,"supports",path+".supports",1,16,[&](J list) {
            auto selected = span(path+".supports");
            if (question_only) {
                const bool range = json_object_get_type(selected.get()) == json_type_array;
                const auto first = string(range ? json_object_array_get_idx(selected.get(),0) : selected.get());
                const auto resolved = range ? catalog.resolve(catalog.snapshot(),first,
                    string(json_object_array_get_idx(selected.get(),1))) : catalog.resolve(catalog.snapshot(),first);
                require(catalog.context().views.at(resolved.view).basis == PremiseBasis::Question,
                    "Signed policy goal support outside question");
            }
            json_object_array_add(list,selected.release());
        });
    }
    J item(J list) {
        const auto value = json_object_new_object();
        json_object_array_add(list,value);
        return value;
    }
    std::size_t size(const char* key) const {
        return json_object_array_length(get(plan.get(),key));
    }
    void fill_atom(J value, const std::string& path, std::size_t variables) {
        auto predicate = select_integer(path+".predicate",size("predicates"));
        const auto index = static_cast<std::size_t>(json_object_get_int64(predicate.get()));
        json_object_object_add(value,"predicate",predicate.release());
        json_object_object_add(value,"scope",select_integer(path+".scope",size("scopes")).release());
        auto sign_schema = json_object_new_object();
        json_object_object_add(sign_schema,"type",text("boolean"));
        auto sign = ask(path+".negative",sign_schema);
        require(json_object_get_type(sign.get()) == json_type_boolean,"Invalid signed policy sign");
        json_object_object_add(value,"negative",sign.release());
        const auto arity = static_cast<std::size_t>(json_object_get_int64(get(
            json_object_array_get_idx(get(plan.get(),"predicates"),index),"arity")));
        enumerate(value,"terms",path+".terms",arity,arity,[&](J terms) {
            const auto term = item(terms);
            auto kind = select_enum(path+".terms.kind",variables
                ? std::initializer_list<const char*>{"constant","variable"}
                : std::initializer_list<const char*>{"constant"});
            const bool variable = string(kind.get()) == "variable";
            json_object_object_add(term,"kind",kind.release());
            json_object_object_add(term,"ref",
                select_integer(path+".terms.ref",variable ? variables : size("entities")).release());
        });
    }
    void atom(J owner, const char* key, const std::string& path, std::size_t variables) {
        const auto value = json_object_new_object();
        json_object_object_add(owner,key,value);
        fill_atom(value,path,variables);
    }
    void validate() const {
        signed_plan_detail::validate_prefix(dump(plan.get()),catalog);
    }
public:
    Controller(const ProblemSourceCatalog& source, const SignedDecisionPolicy& callback)
        : catalog(source), policy(callback), schema(parse(problem_signed_plan_schema(source))),
          plan(json_object_new_object(),json_object_put), history(json_object_new_array(),json_object_put) {
        require(static_cast<bool>(policy),"Missing signed decision policy");
        json_object_object_add(plan.get(),"snapshot",text(catalog.snapshot()));
        for (const auto name : {"entities","predicates","scopes","clauses","goals"})
            json_object_object_add(plan.get(),name,json_object_new_array());
        for (const auto& view : catalog.context().views) if (view.basis == PremiseBasis::Question) {
            if (!question.empty()) question += '\n';
            question += view.text;
        }
    }
    [[noreturn]] void decline(ProblemDeclineKind kind) {
        auto specification = json_object_new_object();
        json_object_object_add(specification,"type",text("string"));
        json_object_object_add(specification,"minLength",json_object_new_int(1));
        json_object_object_add(specification,"maxLength",json_object_new_int(4096));
        const auto reason = ask("reason",specification);
        throw ProblemDecline(kind,string(reason.get()));
    }
    CompiledProblemBatch run() {
        const auto branch = select_enum("branch",{"theory","clarify","unsupported"});
        const auto status = string(branch.get());
        if (status != "theory")
            decline(status == "clarify" ? ProblemDeclineKind::Clarify : ProblemDeclineKind::Unsupported);
        enumerate(plan.get(),"entities","entities",1,64,[&](J list) {
            const auto entity = item(list);
            json_object_object_add(entity,"anchor",span("entities.anchor").release());
            supports(entity,"entities");
            validate();
        });
        enumerate(plan.get(),"predicates","predicates",1,64,[&](J list) {
            const auto predicate = item(list);
            json_object_object_add(predicate,"anchor",span("predicates.anchor").release());
            json_object_object_add(predicate,"arity",select_integer("predicates.arity",5).release());
            supports(predicate,"predicates");
            validate();
        });
        enumerate(plan.get(),"scopes","scopes",1,64,[&](J list) {
            const auto scope = item(list);
            json_object_object_add(scope,"kind",
                select_enum("scopes.kind",{"global","qualified","unresolved"}).release());
            if (string(get(scope,"kind")) == "unresolved") decline(ProblemDeclineKind::Clarify);
            enumerate(scope,"qualifiers","scopes.qualifiers",0,16,[&](J qualifiers) {
                const auto qualifier = item(qualifiers);
                json_object_object_add(qualifier,"key",span("scopes.qualifiers.key").release());
                json_object_object_add(qualifier,"value",span("scopes.qualifiers.value").release());
            });
            supports(scope,"scopes");
            validate();
        });
        enumerate(plan.get(),"clauses","clauses",0,128,[&](J list) {
            const auto clause = item(list);
            enumerate(clause,"variables","clauses.variables",0,8,[&](J variables) {
                json_object_array_add(variables,span("clauses.variables").release());
            });
            const auto variables = json_object_array_length(get(clause,"variables"));
            atom(clause,"head","clauses.head",variables);
            enumerate(clause,"premises","clauses.premises",0,16,[&](J premises) {
                fill_atom(item(premises),"clauses.premises",variables);
            });
            supports(clause,"clauses");
            validate();
        });
        enumerate(plan.get(),"goals","goals",1,8,[&](J list) {
            const auto goal = item(list);
            atom(goal,"atom","goals.atom",0);
            supports(goal,"goals",true);
            validate();
        });
        return compile_problem_signed_plan(dump(plan.get()),catalog);
    }
};
}

CompiledProblemBatch interpret_problem_signed_decisions(
    const ProblemSourceCatalog& catalog, const SignedDecisionPolicy& policy) {
    return Controller(catalog,policy).run();
}

} // namespace axiom
