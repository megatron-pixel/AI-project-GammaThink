#pragma once

#include "task_plan.hpp"

namespace axiom::knowledge {

// Canonical Task IR framing, tuple shapes and captured source-handle vocabulary.
// Does not establish graph validity, source fidelity, permission or truth.
std::string task_plan_grammar(const TaskPlanContext& context);

} // namespace axiom::knowledge
