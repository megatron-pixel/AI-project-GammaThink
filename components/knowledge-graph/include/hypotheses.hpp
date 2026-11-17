#pragma once

#include "reasoning.hpp"

namespace axiom::knowledge {

struct HypothesisLimits {
    std::uint64_t work = 1000000;
    std::size_t plans = 64, frontier = 64, obligations = 256, assumptions = 64, hypotheses = 64;
    std::uint32_t depth = 128;
};
struct HypothesisPlan {
    // One plan is a conjunction. Distinct plans are alternatives.
    std::vector<ObjectId> jointly_required;
    std::vector<std::uint32_t> source_rules;
    bool contradicts_known = false;
};
struct HypothesisReport {
    ObjectId request;
    std::vector<Object> hypotheses;
    std::vector<HypothesisPlan> alternatives;
    std::uint64_t work = 0;
    bool formally_supported = false, existing_contradiction = false, limited = false;
};

// Bounded symbolic abduction over the existing finite-Horn request.
// Candidate premises are never inserted into the request's theory or treated
// as facts. Unsupported roots and circular arguments are not assumed true.
// Returned Hypothesis objects have no authoritative assessment.
HypothesisReport propose_hypotheses(const Object& formal_goal, HypothesisLimits limits = {});

} // namespace axiom::knowledge
