#pragma once

#include "task_plan.hpp"
#include "source_reasoner.hpp"
#include "model_manager.hpp"

namespace axiom::runtime {

struct NativeTaskLimits {
    knowledge::KnowledgeRunLimits knowledge;
    knowledge::SourceLogicLimits logic;
    std::size_t query_page = 64;
    std::size_t response_bytes = 65536, response_objects = 128;
};
struct NativeTaskStep {
    knowledge::ObjectId receipt, artifact;
    knowledge::KnowledgeRunStatus status = knowledge::KnowledgeRunStatus::Unresolved;
    knowledge::GoalOutcome outcome;
    // Proof graphs remain in native CoK and exact certificates; results contains
    // retrieved objects, not an unbounded concatenation of every inference node.
    std::vector<knowledge::ObjectId> certificates, results, unavailable, revalidation_required;
    std::size_t native_executions = 0;
    // Deterministic response receipt JSON, never a new proof or neural answer.
    std::optional<std::string> response;
};
struct TaskExecutionState {
    // Exact admitted DSL/context capture; never an interpreter-supplied outcome.
    std::optional<knowledge::Digest> capture;
    std::map<std::string, knowledge::GoalOutcome> outcomes;
    std::map<std::string, knowledge::ObjectId> artifacts;
    std::vector<knowledge::ObjectId> certificates;
    // Actual operation outputs reconstructed by replay, not receipt JSON flags.
    std::map<std::string, NativeTaskStep> steps;
};

// Actual core/source-proof/retrieval and deterministic result-view operators.
// Code, neural generation and publication are separate profiles. Every call
// requires an unloaded model.
class NativeTaskOperators {
public:
    NativeTaskOperators(knowledge::UnifiedKnowledge& knowledge, knowledge::Journal& journal,
                        neural::NativeModelManager& model);
    NativeTaskStep execute(const knowledge::TaskPlanContext& current_context,
        const knowledge::AdmittedTaskPlan& plan, std::string_view action, TaskExecutionState& state,
        const knowledge::GoalConditions& environment,
        const std::set<knowledge::ObjectId>& accepted_assumptions = {},
        const std::map<knowledge::ObjectId, ProblemContext>& source_contexts = {},
        NativeTaskLimits limits = {});
private:
    knowledge::UnifiedKnowledge& knowledge_;
    knowledge::Journal& journal_;
    neural::NativeModelManager& model_;
};

} // namespace axiom::runtime
