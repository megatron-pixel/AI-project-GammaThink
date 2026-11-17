#pragma once

#include "problem_sources.hpp"

namespace axiom {

// Handles-only control: native graph indices are deliberately unchanged.
// Admission still checks representation, not English fidelity.
ProblemRequest parse_problem_source_proposal(const std::string& json,
                                            const ProblemSourceCatalog& catalog);
std::string problem_source_proposal_schema(const ProblemSourceCatalog& catalog);
std::string problem_source_semantic_prompt();

} // namespace axiom
