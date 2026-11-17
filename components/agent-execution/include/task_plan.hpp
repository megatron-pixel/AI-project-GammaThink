#pragma once

#include "task_ir.hpp"
#include "retrieval.hpp"

namespace axiom::knowledge {

struct TaskObject {
    ObjectId id;
    ObjectKind kind = ObjectKind::Claim;
    std::string format;
    bool operator==(const TaskObject&) const = default;
};
struct TaskPlanLimits {
    std::size_t wire_bytes = 1024 * 1024, entities = 128, goals = 128, actions = 128;
    GoalPlanLimits planner;
};
class TaskPlanContext {
public:
    TaskPlanContext(ProblemSourceCatalog sources, std::size_t current_question,
                    std::map<std::string, TaskObject> objects = {},
                    std::vector<GoalPredicate> active_constraints = {}, TaskPlanLimits limits = {});
    const ProblemSourceCatalog& sources() const { return sources_; }
    std::size_t current_question() const { return current_question_; }
    const std::map<std::string, TaskObject>& objects() const { return objects_; }
    const std::vector<GoalPredicate>& active_constraints() const { return active_; }
    const TaskPlanLimits& limits() const { return limits_; }
    const std::string& snapshot() const { return snapshot_; }
    const std::string& wire() const { return wire_; }
private:
    ProblemSourceCatalog sources_;
    std::size_t current_question_;
    std::map<std::string, TaskObject> objects_;
    std::vector<GoalPredicate> active_;
    TaskPlanLimits limits_;
    std::string snapshot_, wire_;
};
TaskPlanContext restore_task_plan_context(std::string_view wire);
GoalConditions parse_task_environment(std::string_view wire);

struct TaskText { EvidenceSpan span; std::string value; };
struct TaskArtifact {};
struct TaskSourceQuery { ObjectId module; ProblemGoal goal; };
using TaskEntityValue = std::variant<TaskText, TaskObject, TaskArtifact, AdmittedTask, KnowledgeQuery, TaskSourceQuery>;
struct TaskEntity {
    std::string id;
    TaskEntityValue value;
    std::vector<EvidenceSpan> supports;
};
enum class TaskActionKind {
    Reason = 1, SourceReason, Retrieve, CompileSource, GenerateCode, ExecuteCode,
    Explain, Visualize, RequestPublication
};
std::string_view name(TaskActionKind kind);
struct TaskAction {
    std::string id, goal;
    TaskActionKind kind = TaskActionKind::Reason;
    std::vector<std::string> inputs, outputs;
    std::vector<EvidenceSpan> supports;
};
struct TaskPlanDecline {
    TaskDeclineKind kind;
    std::string reason, snapshot, dsl;
    std::vector<EvidenceSpan> supports;
};

// Construction validates requested work; it does not execute an action, assert
// an outcome, accept an assumption, grant a tool permission or publish data.
class AdmittedTaskPlan {
public:
    const std::string& snapshot() const { return snapshot_; }
    const std::string& dsl() const { return dsl_; }
    const std::string& context_wire() const { return context_wire_; }
    const std::vector<TaskEntity>& entities() const { return entities_; }
    const std::vector<TaskAction>& actions() const { return actions_; }
    const std::vector<GoalIR>& definitions() const { return definitions_; }
    const GoalGraph& graph() const { return graph_; }
    const std::vector<GoalPredicate>& active_constraints() const { return active_; }
    const std::map<std::string, std::string>& artifact_goals() const { return artifact_goals_; }
private:
    std::string snapshot_, dsl_, context_wire_;
    std::vector<TaskEntity> entities_;
    std::vector<TaskAction> actions_;
    std::vector<GoalIR> definitions_;
    GoalGraph graph_;
    std::vector<GoalPredicate> active_;
    std::map<std::string, std::string> artifact_goals_;
    AdmittedTaskPlan(std::string snapshot, std::string dsl, std::string context,
                     std::vector<TaskEntity> entities, std::vector<TaskAction> actions,
                     std::vector<GoalIR> definitions, GoalGraph graph,
                     std::vector<GoalPredicate> active, std::map<std::string, std::string> artifact_goals);
    friend std::variant<AdmittedTaskPlan, TaskPlanDecline> admit_task_plan(
        const TaskPlanContext&, std::string_view, std::string_view);
};
using TaskPlanInterpretation = std::variant<AdmittedTaskPlan, TaskPlanDecline>;
TaskPlanInterpretation admit_task_plan(const TaskPlanContext& context, std::string_view input_snapshot,
                                      std::string_view task_ir);

struct TaskPlanEvaluation {
    GoalEvaluation goals;
    std::vector<std::string> ready_actions, missing_artifacts;
};
// Outcomes and produced artifact identities come only from the host's verified
// operators. Neither is a Task IR input. Active host constraints apply to every
// node, including compiler-created artifact dependencies.
TaskPlanEvaluation evaluate_task_plan(const TaskPlanContext& current_context, const AdmittedTaskPlan& plan,
    const std::map<std::string, GoalOutcome>& verified_outcomes, const GoalConditions& environment,
    const std::map<std::string, ObjectId>& produced_artifacts = {},
    const std::set<ObjectId>& accepted_assumptions = {});

struct PersistedTaskPlan {
    ObjectId context, interpretation, manifest;
    std::vector<ObjectId> entities, actions, goals;
};
PersistedTaskPlan persist_task_plan(Journal& journal, const AdmittedTaskPlan& plan);
AdmittedTaskPlan restore_task_plan(Journal& journal, const ObjectId& manifest);

} // namespace axiom::knowledge
