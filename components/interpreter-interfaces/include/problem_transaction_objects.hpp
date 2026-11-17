#pragma once

#include "problem_transaction_semantic.hpp"

namespace axiom {

// Representation-only experiment. Lowering never fills missing semantic choices;
// the result must still pass the existing transaction compiler and admission.
std::string problem_object_transaction_schema(const ProblemSourceCatalog& catalog,
    ProblemTransactionStage stage = ProblemTransactionStage::Single);
std::string problem_object_transaction_prompt(
    ProblemTransactionStage stage = ProblemTransactionStage::Single);
std::string lower_problem_object_transaction(const std::string& input,
    const ProblemSourceCatalog& catalog,
    ProblemTransactionStage stage = ProblemTransactionStage::Single);

} // namespace axiom
