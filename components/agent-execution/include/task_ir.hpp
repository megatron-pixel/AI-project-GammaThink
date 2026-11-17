#pragma once

#include "goal_planner.hpp"
#include "native_cok.hpp"
#include "problem_goals.hpp"
#include "problem_sources.hpp"

#include <variant>

namespace axiom::knowledge {

enum class TaskDeclineKind { Clarify, Unsupported };
struct TaskDecline {
    TaskDeclineKind kind;
    std::string reason;
};

// This bounded Task IR profile wraps the existing ordered problem contract.
// It is requested work, not an assertion of knowledge or source truth.
// Construction admits every goal without running the core or loading a model.
class AdmittedTask {
public:
    const ProblemGoalsRequest& request() const { return request_; }
    const std::string& dsl() const { return dsl_; }
    const std::string& snapshot() const { return snapshot_; }
private:
    ProblemGoalsRequest request_;
    std::string dsl_, snapshot_;
    AdmittedTask(ProblemGoalsRequest request, std::string dsl, std::string snapshot);
    friend std::variant<AdmittedTask, TaskDecline> admit_task_dsl(
        const ProblemSourceCatalog&, std::string_view, std::string_view);
};

using TaskInterpretation = std::variant<AdmittedTask, TaskDecline>;
// Source handles refer only to the supplied immutable host catalog. A captured
// interpreter proposal must carry the snapshot used to build its input.
TaskInterpretation admit_task_dsl(const ProblemSourceCatalog& catalog,
    std::string_view input_snapshot, std::string_view archive_dsl);

struct PersistedTask {
    ObjectId context, interpretation, ordered_request;
    std::vector<ObjectId> requests;
    GoalGraph plan;
};
// Stores the validated interpretation before symbolic execution. Each ordered
// occurrence survives, including identical repeated goals. Private by default.
PersistedTask persist_task(Journal& journal, const AdmittedTask& task);
// Re-admits the original DSL/context and compares every task-record identity.
// Does not execute the core, repair missing records or write to the journal.
AdmittedTask restore_task(Journal& journal, const ObjectId& ordered_request);

struct TaskSlotRun {
    std::size_t ordinal = 0;
    NativeRun execution;
    std::vector<ObjectId> conclusions, derivations;
};
struct TaskRun {
    PersistedTask task;
    std::vector<TaskSlotRun> slots;
    GoalEvaluation evaluation;
};
// Runs the untrained native core on the already admitted requests, independently
// replays certificates, then stores their typed CoK. No interpretation repair,
// empirical evidence approval, inferred premises or neural call occurs.
TaskRun execute_task(Journal& journal, const AdmittedTask& task,
    NativeRunLimits limits = {}, NativeCoKLimits materialization = {});

} // namespace axiom::knowledge
