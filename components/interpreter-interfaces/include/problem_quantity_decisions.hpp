#pragma once

#include "problem_quantity_frame.hpp"
#include "semantic_meter.hpp"

#include <functional>

namespace axiom {

// Public catalog input and earlier selected decisions only. JSON fields are
// serialized values; response_schema is the full {"value":...} object schema.
struct QuantityDecisionRequest {
    std::string role, question, response_schema, frame_so_far, current, catalog;
};
// Return a JSON object containing exactly one "value" field, including null.
// Policy-owned learned state is independent of the controller and transport.
using QuantityDecisionPolicy = std::function<std::string(const QuantityDecisionRequest&)>;

// Experimental learned role selection. One request-wide session, no solver
// feedback, semantic shortlisting, identity merging, repair, or implicit retry.
CompiledProblemTransaction interpret_problem_quantity_decisions(
    const ProblemSourceCatalog& catalog, MeteredSemanticSession& session);

CompiledProblemTransaction interpret_problem_quantity_decisions(
    const ProblemSourceCatalog& catalog, const QuantityDecisionPolicy& policy);

} // namespace axiom
