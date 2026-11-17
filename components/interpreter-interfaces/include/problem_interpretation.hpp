#pragma once

#include "problem_transaction.hpp"
#include "semantic_meter.hpp"

namespace axiom {

enum class ProblemInterpretationArm {
    Quote, Handles, Typed, Staged, ObjectTyped, ObjectStaged, QuantityFrame, QuantityDecisions
};

// Uses the caller's one request-wide ledger. No core execution, gold feedback,
// correction loop, or implicit retry occurs during interpretation.
CompiledProblemTransaction interpret_problem_source(const ProblemContext& context,
    ProblemInterpretationArm arm, MeteredSemanticSession& session);

} // namespace axiom
