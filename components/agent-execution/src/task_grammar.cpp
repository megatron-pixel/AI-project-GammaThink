#include "task_grammar.hpp"

#include <json-c/json.h>

namespace axiom::knowledge {
namespace {
std::string quote(std::string_view value) {
    std::unique_ptr<json_object, decltype(&json_object_put)> encoded(
        json_object_new_string_len(value.data(), static_cast<int>(value.size())), json_object_put);
    if (!encoded) throw Error(ErrorCode::Storage, "Cannot encode Task IR grammar literal");
    const auto* result = json_object_to_json_string_ext(encoded.get(), JSON_C_TO_STRING_PLAIN);
    if (!result) throw Error(ErrorCode::Storage, "Cannot serialize Task IR grammar literal");
    return result;
}
}

std::string task_plan_grammar(const TaskPlanContext& context) {
    std::string result = "root ::= "
        + quote("GAMMATHINK_TASK_IR 1\nCONTEXT " + quote(context.snapshot()) + "\n")
        + " (decline | plan) \"END\" \"\\n\"?\n";
    result += R"GBNF(
plan ::= "ROOT " id nl entity* goal+ action*
decline ::= ("CLARIFY [" | "UNSUPPORTED [") ws string comma supports "]" nl
entity ::= "ENTITY [" ws id comma entity-kind comma value comma supports "]" nl
goal ::= "GOAL [" ws id comma methods comma predicates comma predicates comma strings comma supports "]" nl
action ::= "ACTION [" ws id comma id comma action-kind comma ids comma "[" ws id "]" ws comma supports "]" nl
entity-kind ::= ("\"text\"" | "\"object\"" | "\"artifact\"" | "\"problem\"" | "\"query\"" | "\"source-query\"") ws
action-kind ::= ("\"reason\"" | "\"source-reason\"" | "\"retrieve\"" | "\"compile-source\"" | "\"generate-code\"" | "\"execute-code\"" | "\"explain\"" | "\"visualize\"" | "\"request-publication\"") ws
id ::= "\"" [a-zA-Z0-9_-]{1,64} "\"" ws
ids ::= "[" ws (id (comma id)*)? "]" ws
methods ::= "[" ws (ids (comma ids)*)? "]" ws
strings ::= "[" ws (string (comma string)*)? "]" ws
predicates ::= "[" ws (predicate (comma predicate)*)? "]" ws
predicate ::= "[" ws string comma comparison comma scalar "]" ws
comparison ::= ("\"eq\"" | "\"ne\"" | "\"lt\"" | "\"le\"" | "\"gt\"" | "\"ge\"") ws
supports ::= "[" ws span (comma span)* "]" ws
span ::= handle ws | "[" ws handle ws comma handle ws "]" ws
value ::= scalar | "null" ws | array
scalar ::= string | integer | ("true" | "false") ws
array ::= "[" ws (value (comma value)*)? "]" ws
integer ::= ("0" | "-"? [1-9] [0-9]{0,18}) ws
string ::= "\"" char* "\"" ws
char ::= [^"\\\x00-\x1f] | "\\" (["\\/bfnrt] | "u" [0-9a-fA-F]{4})
comma ::= "," ws
ws ::= [ \t]*
nl ::= ws "\n"
)GBNF";
    result += "handle ::= ";
    for (std::size_t i = 0; i < context.sources().entries().size(); ++i) {
        if (i) result += " | ";
        result += quote(quote(ProblemSourceCatalog::source_handle(i)));
    }
    result += "\n";
    validate_text(result, 262144);
    return result;
}

} // namespace axiom::knowledge
