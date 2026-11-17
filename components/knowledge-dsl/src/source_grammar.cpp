#include "source_grammar.hpp"

namespace axiom::knowledge {
std::string source_archive_grammar() {
    return R"GBNF(root ::= "GAMMATHINK_DSL 2\n" (declaration* | decline) "END" "\n"?
decline ::= ("CLARIFY " | "UNSUPPORTED ") string nl
declaration ::= entity | quantity | number | binding | node | equation | predicate | rule | quantified
entity ::= "ENTITY " string nl
quantity ::= "QUANTITY " quantity-body nl
quantity-body ::= "[" ws string comma uint comma scope comma uint "]" ws
number ::= "NUMBER [" ws span comma maybe-span comma uint comma precision comma maybe-span "]" nl
precision ::= ("\"exact\"" | "\"approximate\"" | "\"bound\"" | "\"unspecified\"") ws
binding ::= "BINDING [" ws uint comma uint comma supports "]" nl
node ::= "NODE " node-body nl
node-body ::= "[" ws ("\"q\"" | "\"n\"" | "\"neg\"") ws comma uint "]" ws | "[" ws binary ws comma uint comma uint "]" ws
binary ::= "\"add\"" | "\"sub\"" | "\"mul\"" | "\"div\""
equation ::= "EQUATION [" ws uint comma uint comma scope comma guard comma supports "]" nl
predicate ::= "PREDICATE [" ws string comma uint "]" nl
rule ::= "RULE [" ws uint comma atom comma atoms comma supports "]" nl
quantified ::= "QUANTIFIED_EQUATION [" ws uint comma templates comma nodes comma uint comma uint comma scope comma guard comma supports "]" nl
templates ::= "[" ws (template (comma template)*)? "]" ws
template ::= "[" ws string comma term comma scope comma uint "]" ws
nodes ::= "[" ws (node-body (comma node-body)*)? "]" ws
scope ::= "[" ws boolean comma qualifiers "]" ws
qualifiers ::= "[" ws (qualifier (comma qualifier)*)? "]" ws
qualifier ::= "[" ws string comma string "]" ws
guard ::= atom | "null" ws
atoms ::= "[" ws (atom (comma atom)*)? "]" ws
atom ::= "[" ws uint comma boolean comma terms "]" ws
terms ::= "[" ws (term (comma term)*)? "]" ws
term ::= "[" ws ("\"c\"" | "\"v\"") ws comma uint "]" ws
supports ::= "[" ws (span (comma span)*)? "]" ws
span ::= "[" ws uint comma uint comma uint comma nullable-string "]" ws | "[" ws uint comma nullable-string comma string comma nullable-string "]" ws
maybe-span ::= span | "null" ws
nullable-string ::= string | "null" ws
boolean ::= ("true" | "false") ws
uint ::= ("0" | [1-9] [0-9]{0,18}) ws
string ::= "\"" char* "\"" ws
char ::= [^"\\\x00-\x1f] | "\\" (["\\/bfnrt] | "u" [0-9a-fA-F]{4})
comma ::= "," ws
ws ::= [ \t]*
nl ::= ws "\n"
)GBNF";
}
}
