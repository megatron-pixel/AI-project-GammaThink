#pragma once

#include "problem_sources.hpp"

namespace axiom {

enum class ProblemTransactionStage { Single, Declarations, Assertions };
std::string problem_transaction_schema(const ProblemSourceCatalog& catalog,
    ProblemTransactionStage stage = ProblemTransactionStage::Single);
std::string problem_transaction_prompt(ProblemTransactionStage stage = ProblemTransactionStage::Single);

} // namespace axiom
