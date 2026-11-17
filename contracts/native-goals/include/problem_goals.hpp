#pragma once

#include "problem.hpp"

namespace axiom {

inline constexpr std::size_t max_problem_goals = 8;

struct SupportedProblemGoal {
    ProblemGoal goal;
    std::vector<EvidenceSpan> supports;
    bool operator==(const SupportedProblemGoal&) const = default;
};

// first contains the actual first goal and the total host-owned limits.
// Every slot shares its context and complete proposal except for the goal.
struct ProblemGoalsRequest {
    ProblemRequest first;
    std::vector<SupportedProblemGoal> goals;
    bool operator==(const ProblemGoalsRequest&) const = default;
};

struct ProblemGoalSlotResult {
    std::uint32_t ordinal = 0;
    ProblemResult result;
};
struct ProblemGoalsResult {
    std::string request_digest;
    std::vector<ProblemGoalSlotResult> slots;
};

// Admission compiles no answers. All slots are admitted before execution.
std::vector<ProblemRequest> admit_problem_goals(const ProblemGoalsRequest& request);
ProblemGoalsResult execute_problem_goals(const ProblemGoalsRequest& request);
bool verify_problem_goals_result(const ProblemGoalsRequest& request,
    const ProblemGoalsResult& result);
std::string render_problem_goals(const ProblemGoalsRequest& request,
    const ProblemGoalsResult& result);

// Isolated AXGL v1, kinds 1/2. Legacy AXRM v3 remains unchanged.
Bytes encode_problem_goals_request(const ProblemGoalsRequest& request);
ProblemGoalsRequest decode_problem_goals_request(std::span<const std::uint8_t> bytes);
Bytes encode_problem_goals_result(const ProblemGoalsResult& result);
ProblemGoalsResult decode_problem_goals_result(std::span<const std::uint8_t> bytes);
Bytes run_problem_goals_binary(std::span<const std::uint8_t> bytes);

} // namespace axiom
