#pragma once

#include "problem_quantity_frame.hpp"

namespace axiom {

ProblemQuantityFrame parse_problem_quantity_frame(const std::string& json,
    const ProblemSourceCatalog& catalog);
std::string problem_quantity_frame_schema(const ProblemSourceCatalog& catalog);
std::string problem_quantity_frame_prompt();

} // namespace axiom
