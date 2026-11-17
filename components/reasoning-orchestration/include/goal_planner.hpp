#pragma once

#include "knowledge.hpp"

#include <map>
#include <set>
#include <variant>

namespace axiom::knowledge {

using GoalValue = std::variant<bool, std::int64_t, std::string>;
using GoalConditions = std::map<std::string, GoalValue>;
enum class GoalComparison { Equal = 1, NotEqual, Less, LessEqual, Greater, GreaterEqual };
struct GoalPredicate {
    std::string field;
    GoalComparison comparison = GoalComparison::Equal;
    GoalValue value;
    bool operator==(const GoalPredicate&) const = default;
};
void validate_goal_predicates(std::span<const GoalPredicate> predicates);

// Parsed revision-2.5 task directives / library goal definitions. This is the
// planning domain, not a substitute for existing problem/quantity meaning IR.
// Prerequisites must come from the source or explicit symbolic library rules.
struct GoalIR {
    std::string id;
    std::vector<std::vector<std::string>> methods;
    GoalConditions constraints, success_conditions;
    std::vector<ObjectId> assumptions;
    std::vector<SourceReference> sources;
    std::optional<ObjectId> formal_request;
    // AND, preserving repeated fields/ranges. No numeric/string/bool coercion.
    std::vector<GoalPredicate> requirements, success_requirements;
};
struct GoalNode {
    std::string id;
    std::optional<GoalIR> definition;
    // AND within a method; OR between methods. Repeated requirements survive.
    std::vector<std::vector<std::size_t>> methods;
};
struct GoalGraph {
    std::size_t root = 0;
    std::vector<GoalNode> nodes;
    std::vector<std::pair<std::size_t, std::size_t>> cycle_edges;
};
struct GoalPlanLimits {
    std::size_t nodes = 256, edges = 4096;
};
enum class GoalState {
    Pending = 1, MissingRequirement, ConstraintBlocked, AssumptionBlocked,
    Ready, UnresolvedOutcome, Failed, Satisfied
};
struct GoalOutcome {
    bool verified = false, succeeded = false;
    GoalConditions observed;
    std::optional<ObjectId> evidence;
};
struct GoalEvaluation {
    std::vector<GoalState> states;
    bool root_satisfied = false;
};

GoalGraph decompose_goals(std::string_view root, const std::vector<GoalIR>& definitions,
                         GoalPlanLimits limits = {});
// Prerequisite completion makes an action ready, not automatically successful.
// Outcomes must be supplied by the execution/proof verifier, not by the model.
GoalEvaluation evaluate_goals(const GoalGraph& graph,
    const std::map<std::string, GoalOutcome>& outcomes, const GoalConditions& environment,
    const std::set<ObjectId>& accepted_assumptions = {},
    std::span<const GoalPredicate> active_constraints = {});

} // namespace axiom::knowledge
