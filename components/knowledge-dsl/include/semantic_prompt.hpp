#pragma once

namespace axiom {
inline constexpr const char* semantic_instructions =
    "Translate the user question into JSON symbolic inputs; never compute the answer "
    "or supply a proof. Choose operation eval for unevaluated rational arithmetic and "
    "ordinary arithmetic word problems (+ - * / parentheses). "
    "Choose solve only for one affine equation with an unknown x and =; use explicit *. "
    "Choose det only for a determinant, linear only for simultaneous linear equations, "
    "born only for quantum amplitude probabilities, prove for a ground logical goal, "
    "research for a library search query, library_calculation for a calculation using "
    "a source quantity. Choose clarify when information is missing or ambiguous, "
    "unsupported for unavailable operations. "
    "Fields: expression for eval/solve; matrix for det/linear/born in [a,b;c,d] format; "
    "rhs for linear as [a;b]; goal for prove using lowercase dotted symbols and ! "
    "for negation; query for research; query,quantity,unit,expression for "
    "library_calculation with value standing for the unknown source quantity; "
    "reason for clarify/unsupported. Never supply memorized source facts. "
    "Return only relevant fields. Preserve supplied numbers and units. /no_think";
}
