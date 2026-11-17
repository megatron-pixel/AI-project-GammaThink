#include "session_tasks.hpp"
#include "journal.hpp"

#include <filesystem>
#include <iostream>
#include <source_location>
#include <sqlite3.h>
#include <stdlib.h>
#include <unistd.h>

namespace {
using namespace axiom::context;
using namespace axiom::knowledge;
std::size_t checks = 0;
void check(bool condition, const std::source_location where = std::source_location::current()) {
    ++checks;
    if (!condition) throw std::runtime_error("Session task assertion at " + std::to_string(where.line()));
}
template<class F> void rejects(ErrorCode code, F&& operation) {
    ++checks;
    try { operation(); }
    catch (const Error& error) { if (error.code == code) return; throw; }
    throw std::runtime_error("Expected session task rejection");
}
struct Directory {
    std::filesystem::path path;
    Directory() {
        auto pattern = (std::filesystem::temp_directory_path() / "session-tasks-XXXXXX").string();
        const auto made = mkdtemp(pattern.data());
        if (!made) throw std::runtime_error("Cannot create task directory");
        path = made;
    }
    ~Directory() { std::filesystem::remove_all(path); }
};
int raw_sql(const std::filesystem::path& path, const char* sql) {
    sqlite3* raw = nullptr;
    const auto code = sqlite3_open(path.c_str(), &raw);
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(raw, sqlite3_close);
    if (code != SQLITE_OK) throw std::runtime_error("Cannot open raw task fixture");
    return sqlite3_exec(raw, sql, nullptr, nullptr, nullptr);
}
}

int main() {
    try {
        Directory directory;
        ConversationMemory memory(directory.path / "conversation.db");
        memory.open_session("a", "project");
        memory.open_session("b", "project");
        const auto first = memory.append("a", Role::User, "1", "First request");
        const auto assistant = memory.append("a", Role::Assistant, "2", "Derived reply");
        const auto foreign = memory.append("b", Role::User, "3", "Other session");
        const auto later = memory.append("a", Role::User, "4", "Corrected request");
        const auto task_id = ObjectId{digest(Bytes{1})};
        const auto certificate = ObjectId{digest(Bytes{2})};
        const auto path = directory.path / "tasks.db";
        {
            SessionTasks tasks(path);
            check(!tasks.active("a"));
            check(tasks.history("a").events.empty());
            rejects(ErrorCode::Schema, [&] { tasks.begin(memory, "a", assistant.id, task_id, 0); });
            rejects(ErrorCode::Schema, [&] { tasks.begin(memory, "a", foreign.id, task_id, 0); });
            const auto begun = tasks.begin(memory, "a", first.id, task_id, 0);
            check(begun.revision == 1 && begun.user_message == first.id);
            check(begun.message_hash == first.content_hash && begun.phase == TaskPhase::Interpreted);
            rejects(ErrorCode::Schema, [&] { tasks.transition("a", 1, TaskPhase::Checked, {certificate}); });
            const auto reasoning = tasks.transition("a", 1, TaskPhase::Reasoning);
            check(reasoning.revision == 2);
            rejects(ErrorCode::Schema, [&] { tasks.transition("a", 2, TaskPhase::Checked); });
            rejects(ErrorCode::Schema, [&] { tasks.transition("a", 2, TaskPhase::Reasoning); });
            const auto unresolved = tasks.transition("a", 2, TaskPhase::Unresolved);
            check(unresolved.phase == TaskPhase::Unresolved);
            tasks.transition("a", 3, TaskPhase::Reasoning);
            const auto checked = tasks.transition("a", 4, TaskPhase::Checked, {certificate, certificate});
            check(checked.certificates == std::vector<ObjectId>({certificate, certificate}));
            check(checked.ordered_task == task_id && checked.message_hash == first.content_hash);
            rejects(ErrorCode::Schema, [&] { tasks.transition("a", 5, TaskPhase::Reasoning); });
            rejects(ErrorCode::Schema, [&] { tasks.begin(memory, "a", first.id, task_id, 5); });
            const auto page = tasks.history("a", 0, 2);
            check(page.events.size() == 2 && !page.complete && page.next == 2);
            const auto rest = tasks.history("a", page.next, 8);
            check(rest.events.size() == 3 && rest.complete && rest.next == 5);
            check(tasks.history("b").events.empty());
            const auto new_request = tasks.begin(memory, "a", later.id, task_id, 5);
            check(new_request.revision == 6 && new_request.user_message == later.id);
            SessionTasks second_connection(path);
            const auto stale = *second_connection.active("a");
            tasks.transition("a", 6, TaskPhase::Cancelled);
            rejects(ErrorCode::Schema, [&] { second_connection.transition("a", stale.revision, TaskPhase::Reasoning); });
            check(second_connection.active("a")->phase == TaskPhase::Cancelled);
            check(raw_sql(path, "UPDATE task_events SET phase=3") == SQLITE_CONSTRAINT);
            check(raw_sql(path, "DELETE FROM task_events") == SQLITE_CONSTRAINT);
            check(tasks.history("a").events.size() == 7);
            rejects(ErrorCode::Budget, [&] { tasks.history("a", 0, 0); });
            rejects(ErrorCode::Schema, [&] { tasks.active(""); });
        }
        {
            SessionTasks reopened(path);
            check(reopened.active("a")->phase == TaskPhase::Cancelled);
            check(reopened.active("a")->message_hash == later.content_hash);
            check(memory.get("a", first.id)->content_hash == first.content_hash);
            check(memory.recent("a").messages.size() == 3);
            SessionTasks limited(path, {.sql_instructions = 1});
            rejects(ErrorCode::Budget, [&] { limited.active("a"); });
            check(reopened.active("a")->revision == 7);
        }
        rejects(ErrorCode::Schema, [&] { SessionTasks wrong(directory.path / "conversation.db"); });
        Journal journal(directory.path / "knowledge.db");
        rejects(ErrorCode::Schema, [&] { SessionTasks wrong(directory.path / "knowledge.db"); });
        check(memory.recent("a").messages.size() == 3);
        const auto failed_path = directory.path / "interrupted-init.db";
        rejects(ErrorCode::Budget, [&] { SessionTasks tight(failed_path, {.sql_instructions = 1}); });
        SessionTasks recovered(failed_path);
        check(!recovered.active("a"));
        {
            memory.open_session("actions", "project");
            const auto user = memory.append("actions", Role::User, "5", "Check and retrieve.");
            const auto action_path = directory.path / "actions.db";
            {
                SessionTasks old(action_path);
                old.begin(memory, "actions", user.id, task_id, 0);
                old.transition("actions", 1, TaskPhase::Reasoning);
            }
            // Schema 1 is precisely the task-event schema without the new
            // completion-marker table. Only this private synthetic DB is changed.
            check(raw_sql(action_path, "DROP TABLE task_refinements; DROP TABLE task_actions; PRAGMA user_version=1;") == SQLITE_OK);
            rejects(ErrorCode::Budget, [&] { SessionTasks interrupted(action_path, {.sql_instructions = 1}); });
            SessionTasks migrated(action_path);
            check(migrated.active("actions")->revision == 2);
            check(migrated.completed_actions("actions", task_id, 2).empty());
            rejects(ErrorCode::Schema, [&] {
                migrated.transition("actions", 2, TaskPhase::Unresolved, {},
                    CompletedTaskAction{4, "compute", certificate});
            });
            check(migrated.active("actions")->revision == 2);
            migrated.transition("actions", 2, TaskPhase::Unresolved, {certificate},
                CompletedTaskAction{3, "compute", certificate});
            const auto recorded = migrated.completed_actions("actions", task_id, 3);
            check(recorded.size() == 1 && recorded[0].revision == 3 &&
                  recorded[0].action == "compute" && recorded[0].receipt == certificate);
            check(migrated.completed_actions("actions", task_id, 2).empty());
            check(migrated.completed_actions("b", task_id, 3).empty());
            check(migrated.completed_actions("actions", certificate, 3).empty());
            rejects(ErrorCode::Schema, [&] {
                migrated.transition("actions", 3, TaskPhase::Reasoning, {},
                    CompletedTaskAction{4, "retrieve", certificate});
            });
            migrated.transition("actions", 3, TaskPhase::Reasoning);
            migrated.transition("actions", 4, TaskPhase::Unresolved, {},
                CompletedTaskAction{5, "retrieve", certificate});
            rejects(ErrorCode::Budget, [&] { (void)migrated.completed_actions("actions", task_id, 5, 1); });
            rejects(ErrorCode::Budget, [&] { (void)migrated.completed_actions("actions", task_id, 5, 0); });
            check(raw_sql(action_path, "UPDATE task_actions SET action='forged'") == SQLITE_CONSTRAINT);
            check(raw_sql(action_path, "DELETE FROM task_actions") == SQLITE_CONSTRAINT);
            SessionTasks reopened(action_path);
            check(reopened.completed_actions("actions", task_id, 5).size() == 2);
            check(reopened.history("actions").events.size() == 5);
        }
        {
            memory.open_session("refinement", "project");
            const auto user = memory.append("refinement", Role::User, "7", "Preserve the original request.");
            const auto refine_path = directory.path / "refinement.db";
            const auto refined_id = ObjectId{digest(Bytes{3})};
            {
                SessionTasks old(refine_path);
                old.begin(memory, "refinement", user.id, task_id, 0);
                old.transition("refinement", 1, TaskPhase::Reasoning);
                old.transition("refinement", 2, TaskPhase::Unresolved, {certificate},
                    CompletedTaskAction{3, "compute", certificate});
            }
            // A genuine schema-2 database retains its operation table, but has
            // no refinement table. The migration is transactional.
            check(raw_sql(refine_path, "DROP TABLE task_refinements; PRAGMA user_version=2;") == SQLITE_OK);
            rejects(ErrorCode::Budget, [&] { SessionTasks interrupted(refine_path, {.sql_instructions = 1}); });
            SessionTasks refinements(refine_path);
            check(refinements.completed_actions("refinement", task_id, 3).size() == 1);
            check(!refinements.refinement("refinement", task_id));
            rejects(ErrorCode::Schema, [&] { refinements.refine(memory, "refinement", refined_id, certificate, 2); });
            rejects(ErrorCode::Schema, [&] { refinements.refine(memory, "refinement", task_id, certificate, 3); });
            const auto refined = refinements.refine(memory, "refinement", refined_id, certificate, 3);
            check(refined.revision == 4 && refined.user_message == user.id && refined.message_hash == user.content_hash);
            check(refined.phase == TaskPhase::Interpreted && refined.certificates.empty());
            check(refinements.completed_actions("refinement", refined_id, 4).empty());
            check(refinements.completed_actions("refinement", task_id, 4).size() == 1);
            const auto record = refinements.refinement("refinement", refined_id);
            check(record && record->revision == 4 && record->parent == task_id && record->receipt == certificate);
            check(!refinements.refinement("b", refined_id));
            check(raw_sql(refine_path, "UPDATE task_refinements SET parent_task='forged'") == SQLITE_CONSTRAINT);
            check(raw_sql(refine_path, "DELETE FROM task_refinements") == SQLITE_CONSTRAINT);
            SessionTasks reopened(refine_path);
            check(reopened.refinement("refinement", refined_id)->receipt == certificate);
            refinements.transition("refinement", 4, TaskPhase::Cancelled);
            rejects(ErrorCode::Schema, [&] { refinements.refine(memory, "refinement", task_id, certificate, 5); });
        }
        {
            memory.open_session("operational", "project");
            const auto user = memory.append("operational", Role::User, "6", "retrieve-fixture");
            SessionTasks operations(directory.path / "operational.db");
            operations.begin(memory, "operational", user.id, task_id, 0);
            operations.transition("operational", 1, TaskPhase::Reasoning);
            rejects(ErrorCode::Schema, [&] {
                operations.transition("operational", 2, TaskPhase::Conflict, {},
                    CompletedTaskAction{3, "retrieve", certificate});
            });
            rejects(ErrorCode::Schema, [&] {
                operations.transition("operational", 2, TaskPhase::Checked, {},
                    CompletedTaskAction{3, "", certificate});
            });
            check(operations.active("operational")->revision == 2);
            const auto complete = operations.transition("operational", 2, TaskPhase::Checked, {},
                CompletedTaskAction{3, "retrieve", certificate});
            check(complete.phase == TaskPhase::Checked && complete.certificates.empty());
            check(operations.completed_actions("operational", task_id, 3).size() == 1);
            rejects(ErrorCode::Schema, [&] { operations.transition("operational", 3, TaskPhase::Reasoning); });
        }
        std::cout << "session task state passed: " << checks << " deterministic assertions\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
