#pragma once

#include "core.hpp"

namespace axiom::knowledge {

struct ForwardRun {
    Result reply;
    std::uint64_t work = 0;
};

// Existing signed finite-Horn IR, existing independent certificate checker.
// Counts rule visits and premise checks; no negative fact is inferred merely
// from missing support. CPU-only; the original backward executor is unchanged.
ForwardRun reason_forward(const Request& request, std::uint64_t maximum_work);

} // namespace axiom::knowledge
