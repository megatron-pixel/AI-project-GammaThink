#include "task_session.hpp"

#include <filesystem>
#include <iostream>
#include <json-c/json.h>
#include <source_location>
#include <unistd.h>

namespace {
using namespace axiom;
using namespace axiom::context;
using namespace axiom::knowledge;
using namespace axiom::runtime;
std::size_t checks = 0;
void check(bool value, const std::source_location where = std::source_location::current()) {
    ++checks;
    if (!value) throw std::runtime_error("Task code assertion at " + std::to_string(where.line()));
}
template<class F> void rejects(ErrorCode code, F&& operation) {
    ++checks;
    try { operation(); }
    catch (const Error& error) { if (error.code == code) return; throw; }
    throw std::runtime_error("Expected code-action rejection");
}
struct Directory {
    std::filesystem::path path;
    Directory() {
        auto pattern = (std::filesystem::temp_directory_path() / "task-code-XXXXXX").string();
        const auto* created = mkdtemp(pattern.data());
        if (!created) throw std::runtime_error("Cannot create code-action fixture");
        path = created;
    }
    ~Directory() { std::filesystem::remove_all(path); }
};
std::string quote(std::string_view text) {
    auto value = std::unique_ptr<json_object, decltype(&json_object_put)>(
        json_object_new_string_len(text.data(), static_cast<int>(text.size())), json_object_put);
    return json_object_to_json_string_ext(value.get(), JSON_C_TO_STRING_PLAIN);
}
TaskPrompt prompt(ConversationMemory& memory, std::string_view session,
    std::map<std::string, TaskObject> objects = {}) {
    ProblemContext host; host.unit_registry_id = "code"; host.unit_registry_version = "1";
    return prepare_task_prompt(retrieve_context(memory, session), host,
        [](std::string_view input) { return input.size(); }, {1000000, 262144, 256},
        std::move(objects), {{"network", GoalComparison::Equal, false}});
}
std::string proposal(const TaskPlanContext& context, std::string input = "\"text\",\"s0\"") {
    return "GAMMATHINK_TASK_IR 1\nCONTEXT " + quote(context.snapshot()) +
        "\nROOT \"report\"\nENTITY [\"input\"," + input + ",[\"s0\"]]\n"
        "ENTITY [\"result\",\"artifact\",null,[\"s0\"]]\n"
        "ENTITY [\"reply\",\"artifact\",null,[\"s0\"]]\n"
        "GOAL [\"work\",[],[],[[\"exit_code\",\"eq\",0]],[],[\"s0\"]]\n"
        "GOAL [\"report\",[],[],[],[],[\"s0\"]]\n"
        "ACTION [\"run\",\"work\",\"execute-code\",[\"input\"],[\"result\"],[\"s0\"]]\n"
        "ACTION [\"describe\",\"report\",\"explain\",[\"result\"],[\"reply\"],[\"s0\"]]\nEND\n";
}
std::string repair_proposal(const TaskPlanContext& context) {
    auto result = proposal(context, "\"object\",\"code\"");
    result.insert(result.find("GOAL "), "ENTITY [\"repair\",\"text\",\"s0\",[\"s0\"]]\n");
    const std::string old_inputs = "\"execute-code\",[\"input\"]";
    result.replace(result.find(old_inputs), old_inputs.size(), "\"execute-code\",[\"input\",\"repair\"]");
    return result;
}
}

int main() {
    try {
        Directory directory;
        ConversationMemory memory(directory.path / "conversation.db");
        SessionTasks tasks(directory.path / "tasks.db");
        Journal journal(directory.path / "journal.db");
        JournalArchive local(journal);
        UnifiedKnowledge knowledge({&local});
        neural::NativeModelManager model;
        execution::Toolchain tools{"/Library/Developer/CommandLineTools/usr/bin/clang++",
            "/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk",
            std::filesystem::canonical("build/execution-helper")};
        const GoalConditions offline{{"network", false}};
        auto controller = std::make_unique<TaskSession>(memory, tasks, journal, model);
        const std::vector<std::pair<std::string, execution::Status>> cases{
            {"#include <stdio.h>\nint main(){puts(\"ok\");}", execution::Status::Passed},
            {"not valid C++", execution::Status::CompileFailed},
            {"int main(){return 7;}", execution::Status::TestFailed},
        };
        for (std::size_t index = 0; index < cases.size(); ++index) {
            const auto session = "case-" + std::to_string(index);
            memory.open_session(session, "project");
            const auto user = memory.append(session, Role::User, "1", cases[index].first);
            const auto context = prompt(memory, session);
            const auto admitted = std::get<SessionTask>(controller->accept_plan(session, user.id, context.context,
                context.context.snapshot(), proposal(context.context), 0));
            TaskExecutionState state;
            rejects(ErrorCode::Permission, [&] {
                (void)controller->permit_code_action(session, admitted.revision, context.context, "run",
                    state, offline, {{"", "ok\n"}});
            });
            controller->configure_execution(tools);
            rejects(ErrorCode::Permission, [&] { controller->configure_execution(tools); });
            rejects(ErrorCode::Permission, [&] {
                execution::Executor unrelated(memory, tasks, journal, model, tools);
            });
            rejects(ErrorCode::Permission, [&] {
                (void)controller->permit_code_action(session, admitted.revision, context.context, "run",
                    state, {{"network", true}}, {{"", "ok\n"}});
            });
            execution::Limits repair; repair.attempts = 2;
            rejects(ErrorCode::Schema, [&] {
                (void)controller->permit_code_action(session, admitted.revision, context.context, "run",
                    state, offline, {{"", "ok\n"}}, {}, repair);
            });
            check(tasks.active(session)->revision == admitted.revision);
            auto permit = controller->permit_code_action(session, admitted.revision, context.context, "run",
                state, offline, {{"", "ok\n"}});
            check(permit.revision() == tasks.active(session)->revision &&
                  tasks.active(session)->phase == TaskPhase::Reasoning);
            const auto before = journal.objects().objects.size();
            rejects(ErrorCode::Permission, [&] {
                (void)controller->execute_code_action(session, permit.revision(), context.context, "other",
                    state, permit, knowledge, offline);
            });
            check(journal.objects().objects.size() == before && state.steps.empty());
            const auto result = controller->execute_code_action(session, permit.revision(), context.context, "run",
                state, permit, knowledge, offline);
            check(result.outcome.verified && result.certificates.empty() && result.native_executions == 0);
            check(std::get<std::int64_t>(result.outcome.observed.at("execution_status")) ==
                  static_cast<std::int64_t>(cases[index].second));
            check(result.outcome.succeeded == (cases[index].second == execution::Status::Passed));
            check(std::get<std::int64_t>(result.outcome.observed.at("tests_passed")) ==
                  (cases[index].second == execution::Status::Passed ? 1 : 0));
            check(std::get<std::string>(result.outcome.observed.at("execution_stage")) ==
                  (cases[index].second == execution::Status::CompileFailed ? "compile" : "test"));
            check(!std::get<bool>(result.outcome.observed.at("native_checked")) &&
                  !std::get<bool>(result.outcome.observed.at("empirical_truth_verified")));
            check(result.results.size() == 3 && state.artifacts.at("result") == result.receipt);
            check(journal.get(result.receipt)->format == "gammathink.task-code-operator.v1" &&
                  !journal.assessment(result.receipt));
            const auto code = journal.get(result.results[0]).value();
            check(std::string(code.payload.begin(), code.payload.end()) == cases[index].first);
            auto current = tasks.active(session).value();
            check(current.phase == TaskPhase::Unresolved && current.certificates.empty());
            rejects(ErrorCode::Permission, [&] {
                (void)controller->execute_code_action(session, current.revision, context.context, "run",
                    state, permit, knowledge, offline);
            });
            controller.reset();
            controller = std::make_unique<TaskSession>(memory, tasks, journal, model);
            const auto objects_before_restore = journal.objects().objects.size();
            auto restored = controller->restore_native_execution(session, current.revision, context.context,
                knowledge, offline);
            check(journal.objects().objects.size() == objects_before_restore);
            check(restored.steps.at("run").receipt == result.receipt &&
                  std::get<bool>(restored.outcomes.at("work").observed.at("execution_record_restored")));
            check(restored.outcomes.at("work").succeeded == result.outcome.succeeded);
            check(restored.outcomes.at("work").observed.at("exit_code") == result.outcome.observed.at("exit_code"));
            const auto explanation = controller->execute_native_action(session, current.revision, context.context,
                "describe", restored, knowledge, offline);
            check(explanation.response.has_value() && tasks.active(session)->phase == TaskPhase::Checked &&
                  tasks.active(session)->certificates.empty());
            current = tasks.active(session).value();
            const auto reply = controller->deliver_native_response(session, current.revision, context.context,
                "describe", knowledge, offline, "2");
            check(reply.role == Role::Assistant && memory.get(session, user.id) == user &&
                  memory.recent(session).messages.size() == 2);
            auto body = std::unique_ptr<json_object, decltype(&json_object_put)>(
                json_tokener_parse(reply.content.c_str()), json_object_put);
            auto* view = json_object_object_get(json_object_object_get(body.get(), "result"), "response");
            check(json_object_get_int(json_object_object_get(view, "source_status")) == static_cast<int>(result.status));
            check(json_object_get_int(json_object_object_get(json_object_object_get(view, "source_observations"),
                "execution_status")) == static_cast<int>(cases[index].second));
            check(json_object_array_length(json_object_object_get(view, "native_results")) == 0 &&
                  json_object_array_length(json_object_object_get(view, "retrieved_objects")) == 3);
            check(!json_object_get_boolean(json_object_object_get(view, "empirical_truth_verified")));
            check(model.state() == neural::ModelState::Unloaded && model.metrics().file_bytes == 0);
        }
        memory.open_session("pins", "project");
        const auto user = memory.append("pins", Role::User, "1", cases[0].first);
        memory.revise_constraint("pins", {"network", "offline", 0, user.id, true}, 0);
        const auto context = prompt(memory, "pins");
        const auto admitted = std::get<SessionTask>(controller->accept_plan("pins", user.id, context.context,
            context.context.snapshot(), proposal(context.context), 0));
        controller->configure_execution(tools);
        TaskExecutionState state;
        auto permit = controller->permit_code_action("pins", admitted.revision, context.context,
            "run", state, offline, {{"", "ok\n"}});
        memory.revise_constraint("pins", {"network", "local-only", 0, user.id, true}, 1);
        const auto before = journal.objects().objects.size();
        rejects(ErrorCode::Integrity, [&] {
            (void)controller->execute_code_action("pins", permit.revision(), context.context, "run",
                state, permit, knowledge, offline);
        });
        check(journal.objects().objects.size() == before && state.steps.empty() &&
              tasks.active("pins")->revision == permit.revision());
        for (unsigned variant = 0; variant < 3; ++variant) {
            controller.reset();
            controller = std::make_unique<TaskSession>(memory, tasks, journal, model);
            controller->configure_execution(tools);
            const auto session = "object-" + std::to_string(variant);
            memory.open_session(session, "project");
            const auto question = memory.append(session, Role::User, "1", "compile-host-object");
            Object source;
            source.label = session; source.kind = variant == 1 ? ObjectKind::Fact : ObjectKind::Action;
            source.format = variant == 1 ? "text/plain" : "gammathink.cpp-source.v1";
            const auto& code = cases[0].first;
            source.payload.assign(code.begin(), code.end());
            const auto source_id = journal.put(source);
            const auto captured = prompt(memory, session, {{"code", {source_id, source.kind, source.format}}});
            const auto admitted_object = std::get<SessionTask>(controller->accept_plan(session, question.id,
                captured.context, captured.context.snapshot(), proposal(captured.context, "\"object\",\"code\""), 0));
            TaskExecutionState execution;
            auto grant = controller->permit_code_action(session, admitted_object.revision, captured.context,
                "run", execution, offline, {{"", "ok\n"}});
            if (variant == 0) {
                SessionTasks foreign_tasks(directory.path / "foreign.db");
                foreign_tasks.begin(memory, session, question.id, admitted_object.ordered_task, 0);
                execution::Executor foreign(memory, foreign_tasks, journal, model, tools);
                auto foreign_grant = foreign.permit(session, 1, {{"", "ok\n"}});
                const auto count = journal.objects().objects.size();
                rejects(ErrorCode::Permission, [&] {
                    (void)controller->execute_code_action(session, grant.revision(), captured.context, "run",
                        execution, foreign_grant, knowledge, offline);
                });
                check(journal.objects().objects.size() == count);
                const auto result = controller->execute_code_action(session, grant.revision(), captured.context, "run",
                    execution, grant, knowledge, offline);
                check(result.outcome.succeeded && !journal.assessment(result.receipt));
                const auto recorded = journal.get(result.receipt).value();
                check(std::find(recorded.links.begin(), recorded.links.end(),
                    Link{Relationship::DependsOn, source_id}) != recorded.links.end());
                auto restored = controller->restore_native_execution(session, tasks.active(session)->revision,
                    captured.context, knowledge, offline);
                check(restored.steps.at("run").receipt == result.receipt);
            } else {
                if (variant == 2) journal.invalidate(source_id, "Synthetic code-source withdrawal");
                const auto count = journal.objects().objects.size();
                rejects(variant == 1 ? ErrorCode::Schema : ErrorCode::Integrity, [&] {
                    (void)controller->execute_code_action(session, grant.revision(), captured.context, "run",
                        execution, grant, knowledge, offline);
                });
                check(journal.objects().objects.size() == count && execution.steps.empty());
            }
        }
        for (unsigned variant = 0; variant < 3; ++variant) {
            controller.reset();
            controller = std::make_unique<TaskSession>(memory, tasks, journal, model);
            controller->configure_execution(tools);
            const auto session = "repair-" + std::to_string(variant);
            memory.open_session(session, "project");
            const auto question = memory.append(session, Role::User, "1", "Repair the supplied code without changing its tests.");
            Object source; source.kind = ObjectKind::Action; source.format = "gammathink.cpp-source.v1";
            source.label = session;
            const auto code = variant == 0 ? cases[0].first : "not valid C++";
            source.payload.assign(code.begin(), code.end());
            const auto source_id = journal.put(source);
            const auto captured = prompt(memory, session, {{"code", {source_id, source.kind, source.format}}});
            const auto task = std::get<SessionTask>(controller->accept_plan(session, question.id,
                captured.context, captured.context.snapshot(), repair_proposal(captured.context), 0));
            TaskExecutionState repaired;
            execution::Limits limits; limits.attempts = variant == 1 ? 1 : 3;
            auto grant = controller->permit_code_action(session, task.revision, captured.context, "run",
                repaired, offline, {{"", "ok\n"}}, {}, limits);
            CodeRepairOptions missing{{"missing-repair-model", directory.path / "missing.gguf",
                Digest{1}, neural::ModelRole::CodeGeneration}, {.output_tokens = 128}};
            if (variant == 2) {
                const auto count = journal.objects().objects.size();
                rejects(ErrorCode::Schema, [&] {
                    (void)controller->execute_code_action(session, grant.revision(), captured.context, "run",
                        repaired, grant, knowledge, offline);
                });
                auto wrong_role = missing; wrong_role.model.role = neural::ModelRole::Interpreter;
                rejects(ErrorCode::Schema, [&] {
                    (void)controller->execute_code_action(session, grant.revision(), captured.context, "run",
                        repaired, grant, knowledge, offline, {}, wrong_role);
                });
                check(journal.objects().objects.size() == count && repaired.steps.empty());
                rejects(ErrorCode::Storage, [&] {
                    (void)controller->execute_code_action(session, grant.revision(), captured.context, "run",
                        repaired, grant, knowledge, offline, {}, missing);
                });
                check(tasks.active(session)->phase == TaskPhase::Failed && repaired.steps.empty() &&
                      tasks.completed_actions(session, task.ordered_task, tasks.active(session)->revision).empty());
                check(model.state() == neural::ModelState::Unloaded && model.metrics().file_bytes == 0);
            } else {
                const auto result = controller->execute_code_action(session, grant.revision(), captured.context, "run",
                    repaired, grant, knowledge, offline, {}, variant == 0
                        ? std::optional<CodeRepairOptions>{missing} : std::nullopt);
                check(result.outcome.succeeded == (variant == 0));
                check(std::get<bool>(result.outcome.observed.at("repair_requested")) &&
                      std::get<std::int64_t>(result.outcome.observed.at("repair_generations")) == 0);
                check(std::get<bool>(result.outcome.observed.at("repair_budget_exhausted")) == (variant == 1));
                check(result.status == (variant == 0 ? KnowledgeRunStatus::Checked : KnowledgeRunStatus::ResourceLimited));
                const auto restored = controller->restore_native_execution(session, tasks.active(session)->revision,
                    captured.context, knowledge, offline);
                check(restored.steps.at("run").receipt == result.receipt &&
                      restored.steps.at("run").status == result.status);
                check(std::get<bool>(restored.outcomes.at("work").observed.at("execution_record_restored")));
                check(model.state() == neural::ModelState::Unloaded && model.metrics().file_bytes == 0);
            }
        }
        {
            memory.open_session("generation", "project");
            const auto question = memory.append("generation", Role::User, "1", "generate-code-fixture");
            const auto captured = prompt(memory, "generation");
            const auto source = "GAMMATHINK_TASK_IR 1\nCONTEXT " + quote(captured.context.snapshot()) +
                "\nROOT \"make\"\nENTITY [\"input\",\"text\",\"s0\",[\"s0\"]]\n"
                "ENTITY [\"code\",\"artifact\",null,[\"s0\"]]\n"
                "GOAL [\"make\",[],[],[],[],[\"s0\"]]\n"
                "ACTION [\"generate\",\"make\",\"generate-code\",[\"input\"],[\"code\"],[\"s0\"]]\nEND\n";
            const auto task = std::get<SessionTask>(controller->accept_plan("generation", question.id,
                captured.context, captured.context.snapshot(), source, 0));
            TaskExecutionState generated;
            neural::ModelSpec missing{"missing-code-generator", directory.path / "missing.gguf", Digest{1},
                neural::ModelRole::Interpreter};
            const auto count = journal.objects().objects.size();
            rejects(ErrorCode::Schema, [&] {
                (void)controller->generate_code_action("generation", task.revision, captured.context,
                    "generate", generated, offline, missing);
            });
            missing.role = neural::ModelRole::CodeGeneration;
            CodeGenerationOptions invalid; invalid.output_tokens = 0;
            rejects(ErrorCode::Budget, [&] {
                (void)controller->generate_code_action("generation", task.revision, captured.context,
                    "generate", generated, offline, missing, {}, invalid);
            });
            check(tasks.active("generation")->revision == task.revision && journal.objects().objects.size() == count);
            rejects(ErrorCode::Storage, [&] {
                (void)controller->generate_code_action("generation", task.revision, captured.context,
                    "generate", generated, offline, missing);
            });
            check(tasks.active("generation")->phase == TaskPhase::Failed &&
                  tasks.completed_actions("generation", task.ordered_task, tasks.active("generation")->revision).empty());
            check(generated.steps.empty() && journal.objects().objects.size() == count &&
                  model.state() == neural::ModelState::Unloaded && model.metrics().file_bytes == 0);
        }
        std::cout << "Task code integration passed: " << checks << " deterministic assertions\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
