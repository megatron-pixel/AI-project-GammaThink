#pragma once
#include "task_source.hpp"
#include "inspection.hpp"

#include "model_manager.hpp"
#include "session_tasks.hpp"
#include "task_ir.hpp"
#include "task_plan.hpp"
#include "task_frontend.hpp"
#include "task_operators.hpp"
#include "execution.hpp"
#include "task_generation.hpp"

namespace axiom::runtime {

using SessionAcceptance = std::variant<context::SessionTask, knowledge::TaskDecline>;
using PlanSessionAcceptance = std::variant<context::SessionTask, knowledge::TaskPlanDecline>;
struct TaskInterpretationOptions {
    context::ContextReadLimits reads;
    context::ContextBudget budget;
    std::vector<std::uint64_t> instruction_messages;
    std::string history_phrase;
    context::ContextDocuments* documents = nullptr;
    std::string document_phrase;
    std::map<std::string, knowledge::TaskObject> objects;
    std::vector<knowledge::GoalPredicate> active_constraints;
    knowledge::TaskPlanLimits plan_limits;
    ProblemLimits problem_limits;
    std::uint32_t output_tokens = 512;
    // Source-extractive, versioned context maintenance before loading a model.
    // Zero disables automatic maintenance for a host-selected manual profile.
    std::size_t summary_interval = 32;
};

// One owner thread and one controller per canonical task-state database.
// Original messages, task-state metadata and knowledge remain distinct stores.
// This boundary accepts interpreter proposals; it does not implement an English
// model, certify translation fidelity or use a model to solve/check the task.
class TaskSession {
public:
    TaskSession(context::ConversationMemory& memory, context::SessionTasks& tasks,
                knowledge::Journal& journal, neural::NativeModelManager& model);
    ~TaskSession();
    TaskSession(const TaskSession&) = delete;
    TaskSession& operator=(const TaskSession&) = delete;
    // Admits and persists DSL/context/ordered goals before unloading the model.
    // Every failure and explicit decline also unloads it. No native execution.
    SessionAcceptance accept(std::string_view session, std::uint64_t user_message,
        const ProblemSourceCatalog& catalog, std::string_view input_snapshot,
        std::string_view archive_dsl, std::uint64_t expected_revision);
    // New action-plan grammar. Persist before unload; no action dispatch or
    // permission grant. The context must capture this exact conversation turn.
    PlanSessionAcceptance accept_plan(std::string_view session, std::uint64_t user_message,
        const knowledge::TaskPlanContext& context, std::string_view input_snapshot,
        std::string_view task_ir, std::uint64_t expected_revision);
    // Additive re-planning of this same original turn. Existing requested
    // entities/actions/goals cannot be replaced or weakened; effects require
    // fresh host permits and historical operations are not copied.
    PlanSessionAcceptance refine_plan(std::string_view session, std::uint64_t expected_revision,
        const knowledge::TaskPlanContext& current_context, std::string_view input_snapshot,
        std::string_view task_ir);
    knowledge::AdmittedTaskPlan active_plan(std::string_view session, std::uint64_t expected_revision,
        const knowledge::TaskPlanContext& current_context);
    // Same exact-turn/pin/manifest checks, allowing read-only delivery of a
    // checked task. This grants no action execution or external capability.
    knowledge::AdmittedTaskPlan review_plan(std::string_view session, std::uint64_t expected_revision,
        const knowledge::TaskPlanContext& current_context);
    // Exact host-selected experimental interpreter; load on demand, count the
    // full prompt, generate once, recheck live memory, persist and unload.
    // Structural admission does not qualify its source-meaning fidelity.
    PlanSessionAcceptance interpret_plan(std::string_view session, const neural::ModelSpec& interpreter,
        const ProblemContext& host_sources, std::uint64_t expected_revision,
        TaskInterpretationOptions options = {});
    NativeTaskStep execute_native_action(std::string_view session, std::uint64_t expected_revision,
        const knowledge::TaskPlanContext& current_context, std::string_view action,
        TaskExecutionState& state, knowledge::UnifiedKnowledge& knowledge,
        const knowledge::GoalConditions& environment,
        const std::set<knowledge::ObjectId>& accepted_assumptions = {},
        const std::map<knowledge::ObjectId, ProblemContext>& source_contexts = {},
        NativeTaskLimits limits = {});
    // Replays only recorded pure computational actions, rechecking native
    // certificates/retrieval under current policy. Receipt JSON is not authority.
    TaskExecutionState restore_native_execution(std::string_view session, std::uint64_t expected_revision,
        const knowledge::TaskPlanContext& current_context, knowledge::UnifiedKnowledge& knowledge,
        const knowledge::GoalConditions& environment,
        const std::set<knowledge::ObjectId>& accepted_assumptions = {},
        const std::map<knowledge::ObjectId, ProblemContext>& source_contexts = {},
        NativeTaskLimits limits = {});
    // Deliver a recorded deterministic explanation after independently replaying
    // its computational inputs. Exact-turn/pin compare-and-append prevents stale
    // or duplicate conversation writes. No neural inference or public write.
    context::Message deliver_native_response(std::string_view session, std::uint64_t expected_revision,
        const knowledge::TaskPlanContext& current_context, std::string_view action,
        knowledge::UnifiedKnowledge& knowledge, const knowledge::GoalConditions& environment,
        std::string_view timestamp, const std::set<knowledge::ObjectId>& accepted_assumptions = {},
        const std::map<knowledge::ObjectId, ProblemContext>& source_contexts = {},
        NativeTaskLimits limits = {});
    // Explicit host toolchain and one-attempt capability for one admitted code
    // action. Neither the interpreter nor an imported object can grant this.
    void configure_execution(execution::Toolchain tools);
    execution::Permit permit_code_action(std::string_view session, std::uint64_t expected_revision,
        const knowledge::TaskPlanContext& current_context, std::string_view action,
        const TaskExecutionState& state, const knowledge::GoalConditions& environment,
        std::vector<execution::Test> tests, const std::set<knowledge::ObjectId>& accepted_assumptions = {},
        execution::Limits limits = {.attempts = 1});
    NativeTaskStep execute_code_action(std::string_view session, std::uint64_t expected_revision,
        const knowledge::TaskPlanContext& current_context, std::string_view action,
        TaskExecutionState& state, execution::Permit& permit, knowledge::UnifiedKnowledge& knowledge,
        const knowledge::GoalConditions& environment,
        const std::set<knowledge::ObjectId>& accepted_assumptions = {},
        const std::optional<CodeRepairOptions>& repair = {});
    NativeTaskStep generate_code_action(std::string_view session, std::uint64_t expected_revision,
        const knowledge::TaskPlanContext& current_context, std::string_view action,
        TaskExecutionState& state, const knowledge::GoalConditions& environment,
        const neural::ModelSpec& model, const std::set<knowledge::ObjectId>& accepted_assumptions = {},
        CodeGenerationOptions options = {});
    NativeTaskStep compile_source_action(std::string_view session, std::uint64_t expected_revision,
        const knowledge::TaskPlanContext& current_context, std::string_view action,
        TaskExecutionState& state, const knowledge::GoalConditions& environment,
        const TaskSourceCompilation& input,
        const std::set<knowledge::ObjectId>& accepted_assumptions = {});
    // Translation-only candidate capture. Does not complete the source action
    // or admit its output; explicit compilation remains a separate operation.
    SourceTranslationCandidate translate_source_action(std::string_view session, std::uint64_t expected_revision,
        const knowledge::TaskPlanContext& current_context, std::string_view action,
        TaskExecutionState& state, const knowledge::GoalConditions& environment,
        const TaskSourceCompilation& input, const neural::ModelSpec& model,
        const std::set<knowledge::ObjectId>& accepted_assumptions = {}, SourceTranslationOptions options = {});
    // Private observational exports and publication requests only. This never
    // changes visibility, prepares a public closure or authorizes an upload.
    NativeTaskStep review_action(std::string_view session, std::uint64_t expected_revision,
        const knowledge::TaskPlanContext& current_context, std::string_view action,
        TaskExecutionState& state, knowledge::UnifiedKnowledge& sources,
        const knowledge::GoalConditions& environment,
        const std::set<knowledge::ObjectId>& accepted_assumptions = {},
        knowledge::InspectionLimits inspection = {});
    // Requires the model already unloaded. Re-admits stored source-bound DSL;
    // interrupted pure-symbolic runs resume through checked certificate replay.
    // Returned results belong to the captured user-message identity, not to a
    // later conversation turn. No response text is automatically published.
    knowledge::TaskRun execute(std::string_view session, std::uint64_t expected_revision,
        knowledge::NativeRunLimits limits = {}, knowledge::NativeCoKLimits materialization = {});
private:
    void check_refinement_history(std::string_view session, const context::SessionTask& task,
        const knowledge::AdmittedTaskPlan& plan);
    NativeTaskStep restore_review_action(const knowledge::TaskPlanContext& current_context,
        const knowledge::AdmittedTaskPlan& plan, const context::SessionTask& task,
        const context::CompletedTaskAction& action, TaskExecutionState& state,
        knowledge::UnifiedKnowledge& sources, const knowledge::GoalConditions& environment,
        const std::set<knowledge::ObjectId>& accepted_assumptions);
    NativeTaskStep restore_source_action(const knowledge::TaskPlanContext& current_context,
        const knowledge::AdmittedTaskPlan& plan, const context::SessionTask& task,
        const context::CompletedTaskAction& action, TaskExecutionState& state,
        const knowledge::GoalConditions& environment,
        const std::set<knowledge::ObjectId>& accepted_assumptions);
    std::vector<context::CompletedTaskAction> check_execution_state(std::string_view session,
        std::uint64_t expected_revision, const knowledge::AdmittedTaskPlan& plan,
        const TaskExecutionState& state);
    NativeTaskStep restore_code_action(const knowledge::TaskPlanContext& current_context,
        const knowledge::AdmittedTaskPlan& plan, const context::SessionTask& task,
        const context::CompletedTaskAction& action, TaskExecutionState& state,
        const knowledge::GoalConditions& environment,
        const std::set<knowledge::ObjectId>& accepted_assumptions);
    NativeTaskStep restore_generation_action(const knowledge::TaskPlanContext& current_context,
        const knowledge::AdmittedTaskPlan& plan, const context::SessionTask& task,
        const context::CompletedTaskAction& action, TaskExecutionState& state,
        const knowledge::GoalConditions& environment,
        const std::set<knowledge::ObjectId>& accepted_assumptions);
    NativeTaskStep repair_code_candidate(std::string_view session, std::uint64_t expected_revision,
        const knowledge::TaskPlanContext& current_context, const knowledge::AdmittedTaskPlan& plan,
        const knowledge::TaskAction& action, const TaskExecutionState& state, const execution::Permit& permit,
        const execution::Result& failed, std::string_view original_source,
        const CodeRepairOptions& options);
    knowledge::ObjectId verify_repair_generation(const knowledge::TaskPlanContext& current_context,
        const knowledge::AdmittedTaskPlan& plan, const context::SessionTask& task,
        const knowledge::TaskAction& action, const TaskExecutionState& state,
        const knowledge::ObjectId& specification, const knowledge::ObjectId& previous_execution,
        const knowledge::ObjectId& previous_source, const knowledge::ObjectId& generation,
        std::string_view original_source);
    NativeTaskStep restore_repair_action(const knowledge::TaskPlanContext& current_context,
        const knowledge::AdmittedTaskPlan& plan, const context::SessionTask& task,
        const context::CompletedTaskAction& action, TaskExecutionState& state,
        const knowledge::GoalConditions& environment,
        const std::set<knowledge::ObjectId>& accepted_assumptions);
    knowledge::AdmittedTaskPlan load_plan(std::string_view session, std::uint64_t expected_revision,
        const knowledge::TaskPlanContext& current_context, bool active_only);
    TaskExecutionState replay_native_execution(std::string_view session, std::uint64_t expected_revision,
        const knowledge::TaskPlanContext& current_context, knowledge::UnifiedKnowledge& knowledge,
        const knowledge::GoalConditions& environment, const std::set<knowledge::ObjectId>& accepted_assumptions,
        const std::map<knowledge::ObjectId, ProblemContext>& source_contexts,
        NativeTaskLimits limits, bool active_only);
    context::Message source(std::string_view session, std::uint64_t user_message);
    context::ConversationMemory& memory_;
    context::SessionTasks& tasks_;
    knowledge::Journal& journal_;
    neural::NativeModelManager& model_;
    int execution_lock_ = -1;
    std::unique_ptr<execution::Executor> executor_;
};

} // namespace axiom::runtime
