#pragma once

#include "conversation.hpp"

namespace axiom::context {

enum class TaskPhase : std::uint8_t {
    Interpreted = 1, Reasoning, Checked, Unresolved, Failed, Cancelled, Conflict
};
struct SessionTask {
    std::string session;
    std::uint64_t revision = 0, user_message = 0;
    knowledge::Digest message_hash{};
    // Legacy ordered request or distinct Task IR manifest. The controller
    // restores the exact typed record; task-state metadata is not executable.
    knowledge::ObjectId ordered_task;
    TaskPhase phase = TaskPhase::Interpreted;
    // References only; the native checker, not task-state metadata, establishes
    // certificate validity. Repeated ordered goals may repeat a certificate.
    // Operational completion may be Checked with a recorded action and no
    // native certificate; this never asserts logical or empirical truth.
    std::vector<knowledge::ObjectId> certificates;
};
struct TaskHistory {
    std::vector<SessionTask> events;
    bool complete = true;
    std::uint64_t next = 0;
};
struct CompletedTaskAction {
    std::uint64_t revision = 0;
    std::string action;
    knowledge::ObjectId receipt;
};
struct TaskRefinement {
    std::uint64_t revision = 0;
    knowledge::ObjectId parent, task, receipt;
};
struct SessionTaskLimits {
    std::size_t cache_kib = 1024;
    std::uint64_t sql_instructions = 1000000;
};

// Separate private database: never substitutes task status for knowledge truth,
// modifies original conversation messages, or makes model output authoritative.
// Append-only revisions and expected-revision writes detect stale writers.
class SessionTasks {
public:
    explicit SessionTasks(const std::filesystem::path& path, SessionTaskLimits limits = {});
    ~SessionTasks();
    SessionTasks(const SessionTasks&) = delete;
    SessionTasks& operator=(const SessionTasks&) = delete;
    const std::filesystem::path& path() const { return path_; }
    std::optional<SessionTask> active(std::string_view session) const;
    TaskHistory history(std::string_view session, std::uint64_t after = 0,
                        std::size_t limit = 64) const;
    // Operational completion markers only. Never deserialize them as verified
    // outcomes; restoring execution requires rechecking the actual operation.
    std::vector<CompletedTaskAction> completed_actions(std::string_view session,
        const knowledge::ObjectId& task, std::uint64_t through_revision,
        std::size_t limit = 128) const;
    SessionTask begin(ConversationMemory& memory, std::string_view session,
        std::uint64_t user_message, const knowledge::ObjectId& ordered_task,
        std::uint64_t expected_revision);
    // Same original turn, new captured plan; no previous operation is reused.
    // This stores references, not an assertion of preserved meaning. The
    // controller must independently validate the immutable refinement receipt.
    SessionTask refine(ConversationMemory& memory, std::string_view session,
        const knowledge::ObjectId& task, const knowledge::ObjectId& receipt,
        std::uint64_t expected_revision);
    std::optional<TaskRefinement> refinement(std::string_view session,
        const knowledge::ObjectId& task) const;
    SessionTask transition(std::string_view session, std::uint64_t expected_revision,
        TaskPhase phase, const std::vector<knowledge::ObjectId>& certificates = {},
        const std::optional<CompletedTaskAction>& action = {});
private:
    struct Close { void operator()(sqlite3* db) const noexcept; };
    std::unique_ptr<sqlite3, Close> database_;
    SessionTaskLimits limits_;
    std::filesystem::path path_;
};

} // namespace axiom::context
