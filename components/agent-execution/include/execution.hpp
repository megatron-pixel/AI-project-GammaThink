#pragma once

#include "journal.hpp"
#include "session_tasks.hpp"
#include "model_manager.hpp"
#include <thread>

namespace axiom::runtime { class TaskSession; }

namespace axiom::execution {

struct Test {
    std::string input, output;
};
struct Limits {
    std::uint32_t attempts = 3, seconds = 10;
    std::size_t output_bytes = 65536, source_bytes = 65536;
    std::uint64_t resident_bytes = 512 * 1024 * 1024;
};
struct Toolchain {
    // Trusted deployment configuration, never supplied by a model or archive.
    std::filesystem::path compiler, sdk, helper;
};
enum class Status { Passed, CompileFailed, TestFailed, ResourceLimit };
struct Result {
    Status status;
    knowledge::ObjectId artifact, receipt;
    std::string diagnostics;
    std::uint32_t attempt;
};

// A host-created capability, not an importable knowledge object. Tests and
// scope are fixed before any generated code runs. No shell or public effects.
class Permit {
    friend class Executor;
    friend class runtime::TaskSession;
    Permit() = default;
    std::shared_ptr<const std::uint8_t> owner_;
    context::SessionTask task_;
    std::vector<Test> tests_;
    std::vector<context::Constraint> pins_;
    Limits limits_;
    std::uint32_t used_ = 0;
    std::optional<Result> previous_;
    knowledge::ObjectId specification_;
    std::string action_;
public:
    Permit(const Permit&) = delete;
    Permit& operator=(const Permit&) = delete;
    Permit(Permit&& other) noexcept
        : owner_(std::exchange(other.owner_, nullptr)), task_(std::move(other.task_)),
          tests_(std::move(other.tests_)), pins_(std::move(other.pins_)), limits_(other.limits_), used_(other.used_),
          previous_(std::move(other.previous_)), specification_(other.specification_),
          action_(std::move(other.action_)) {}
    Permit& operator=(Permit&&) = delete;
    std::uint64_t revision() const noexcept { return task_.revision; }
};

// Single owner thread; journal and session connections retain their own
// ownership rules. Operational test results never become logical certificates.
class Executor {
public:
    Executor(context::ConversationMemory& memory, context::SessionTasks& tasks,
             knowledge::Journal& journal, neural::NativeModelManager& model, Toolchain tools);
    ~Executor();
    Executor(const Executor&) = delete;
    Executor& operator=(const Executor&) = delete;
    Permit permit(std::string_view session, std::uint64_t expected_revision,
                  std::vector<Test> tests, Limits limits = {});
    Result attempt(Permit& permit, std::string_view source,
                   std::optional<knowledge::ObjectId> repairs = {},
                   std::optional<knowledge::ObjectId> source_artifact = {});
private:
    friend class runtime::TaskSession;
    // Only the owning session can share its existing OS lock description.
    // Independent public executors still acquire exclusive ownership.
    Executor(context::ConversationMemory& memory, context::SessionTasks& tasks,
        knowledge::Journal& journal, neural::NativeModelManager& model, Toolchain tools,
        int session_lock);
    context::ConversationMemory& memory_;
    context::SessionTasks& tasks_;
    knowledge::Journal& journal_;
    neural::NativeModelManager& model_;
    Toolchain tools_;
    std::thread::id owner_ = std::this_thread::get_id();
    int execution_lock_ = -1;
    const std::shared_ptr<const std::uint8_t> capability_ = std::make_shared<const std::uint8_t>(0);
    void current(const context::SessionTask& task, const std::vector<context::Constraint>* pins = nullptr);
};

} // namespace axiom::execution
