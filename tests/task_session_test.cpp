#include "task_session.hpp"

#include <filesystem>
#include <iostream>
#include <json-c/json.h>
#include <source_location>
#include <stdlib.h>
#include <unistd.h>

namespace {
using namespace axiom;
using namespace axiom::context;
using namespace axiom::knowledge;
using namespace axiom::neural;
using namespace axiom::runtime;
std::size_t checks = 0;
void check(bool value, const std::source_location where = std::source_location::current()) {
    ++checks;
    if (!value) throw std::runtime_error("Task session assertion at " + std::to_string(where.line()));
}
template<class F> void rejects(ErrorCode code, F&& operation) {
    ++checks;
    try { operation(); }
    catch (const Error& error) { if (error.code == code) return; throw; }
    throw std::runtime_error("Expected task session rejection");
}
struct Directory {
    std::filesystem::path path;
    Directory() {
        auto pattern = (std::filesystem::temp_directory_path() / "task-session-XXXXXX").string();
        const auto made = mkdtemp(pattern.data());
        if (!made) throw std::runtime_error("Cannot create session directory");
        path = made;
    }
    ~Directory() { std::filesystem::remove_all(path); }
};
ProblemSourceCatalog catalog(std::string text) {
    ProblemContext context;
    context.unit_registry_id = "session";
    context.unit_registry_version = "1";
    context.views.push_back(make_evidence_view("question", PremiseBasis::Question,
        std::move(text), "", "question", "literal-v1"));
    return ProblemSourceCatalog(std::move(context));
}
std::string dsl(bool negative = false, bool missing = false) {
    std::string result = "GAMMATHINK_DSL 1\nENTITY \"robin\"\nPREDICATE [\"flies\",1]\n";
    if (!missing) result += "RULE [0,[0,false,[[\"c\",0]]],[],[\"s0\"]]\n";
    if (negative) result += "RULE [0,[0,true,[[\"c\",0]]],[],[\"s0\"]]\n";
    result += "GOAL {\"goal\":[\"atom\",[0,false,[[\"c\",0]]]],\"supports\":[\"s0\"]}\n";
    result += "GOAL {\"goal\":[\"atom\",[0,false,[[\"c\",0]]]],\"supports\":[\"s0\"]}\nEND\n";
    return result;
}
std::string quote(std::string_view text) {
    const auto value = std::unique_ptr<json_object, decltype(&json_object_put)>(
        json_object_new_string_len(text.data(), static_cast<int>(text.size())), json_object_put);
    return json_object_to_json_string_ext(value.get(), JSON_C_TO_STRING_PLAIN);
}
}

int main() {
    try {
        Directory directory;
        const auto conversation_path = directory.path / "conversation.db";
        const auto state_path = directory.path / "tasks.db";
        const auto journal_path = directory.path / "journal.db";
        const auto source = catalog("Robin flies. Is Robin flying? Ask twice.");
        SessionTask saved;
        {
            ConversationMemory memory(conversation_path);
            memory.open_session("a", "project");
            const auto user = memory.append("a", Role::User, "1", source.context().views[0].text);
            SessionTasks tasks(state_path);
            Journal journal(journal_path);
            NativeModelManager model;
            {
                TaskSession controller(memory, tasks, journal, model);
                rejects(ErrorCode::Permission, [&] { TaskSession duplicate(memory, tasks, journal, model); });
                const auto wrong = catalog("An unrelated question.");
                rejects(ErrorCode::Schema, [&] { controller.accept("a", user.id, wrong, wrong.snapshot(), dsl(), 0); });
                check(model.state() == ModelState::Unloaded && !tasks.active("a"));
                rejects(ErrorCode::Schema, [&] { controller.accept("a", user.id, source, source.snapshot(), "bad DSL", 0); });
                check(model.state() == ModelState::Unloaded && journal.objects().objects.empty());
                const auto decline = controller.accept("a", user.id, source, source.snapshot(),
                    "GAMMATHINK_DSL 1\nCLARIFY \"Which referent?\"\nEND\n", 0);
                check(std::holds_alternative<TaskDecline>(decline));
                check(!tasks.active("a") && journal.objects().objects.empty());
                saved = std::get<SessionTask>(controller.accept("a", user.id, source, source.snapshot(), dsl(), 0));
                check(saved.phase == TaskPhase::Interpreted);
                check(model.state() == ModelState::Unloaded);
                check(journal.by_kind(ObjectKind::TestResult).objects.empty());
                check(restore_task(journal, saved.ordered_task).dsl() == dsl());
                const auto result = controller.execute("a", saved.revision);
                check(result.evaluation.root_satisfied && result.slots.size() == 2);
                check(!result.slots[0].execution.reused && result.slots[1].execution.reused);
                check(tasks.active("a")->phase == TaskPhase::Checked);
                check(tasks.active("a")->certificates.size() == 2);
                check(memory.recent("a").messages.size() == 1);
                rejects(ErrorCode::Schema, [&] { controller.execute("a", saved.revision); });

                const auto next = memory.append("a", Role::User, "2", source.context().views[0].text);
                saved = std::get<SessionTask>(controller.accept("a", next.id, source, source.snapshot(),
                    dsl(), tasks.active("a")->revision));
                saved = tasks.transition("a", saved.revision, TaskPhase::Reasoning);
                // Simulate exit after the pure core persisted its certificate,
                // before the session metadata received the finished outcome.
                const auto partial = execute_task(journal, restore_task(journal, saved.ordered_task));
                check(partial.evaluation.root_satisfied);
            }
        }
        {
            ConversationMemory memory(conversation_path);
            SessionTasks tasks(state_path);
            Journal journal(journal_path);
            NativeModelManager model;
            TaskSession recovered(memory, tasks, journal, model);
            check(tasks.active("a")->phase == TaskPhase::Reasoning);
            const auto resumed = recovered.execute("a", saved.revision);
            check(resumed.evaluation.root_satisfied);
            check(resumed.slots[0].execution.reused && resumed.slots[1].execution.reused);
            check(tasks.active("a")->phase == TaskPhase::Checked);
            check(tasks.active("a")->message_hash == saved.message_hash);
            check(model.state() == ModelState::Unloaded);

            const auto newer = memory.append("a", Role::User, "3", source.context().views[0].text);
            const auto waiting = std::get<SessionTask>(recovered.accept("a", newer.id, source, source.snapshot(),
                dsl(), tasks.active("a")->revision));
            memory.append("a", Role::User, "4", "A later correction changes the request.");
            rejects(ErrorCode::Schema, [&] { recovered.execute("a", waiting.revision); });
            check(tasks.active("a")->phase == TaskPhase::Interpreted);

            const auto conflicting = catalog("Robin flies and does not fly. Is Robin flying?");
            const auto conflict_user = memory.append("a", Role::User, "5", conflicting.context().views[0].text);
            const auto conflict_task = std::get<SessionTask>(recovered.accept("a", conflict_user.id, conflicting,
                conflicting.snapshot(), dsl(true), tasks.active("a")->revision));
            const auto conflict = recovered.execute("a", conflict_task.revision);
            check(!conflict.evaluation.root_satisfied && conflict.slots[0].execution.check.conflict);
            check(tasks.active("a")->phase == TaskPhase::Conflict);

            const auto unknown = catalog("Is Robin flying?");
            const auto unknown_user = memory.append("a", Role::User, "6", unknown.context().views[0].text);
            const auto unknown_task = std::get<SessionTask>(recovered.accept("a", unknown_user.id, unknown,
                unknown.snapshot(), dsl(false, true), tasks.active("a")->revision));
            const auto unresolved = recovered.execute("a", unknown_task.revision);
            check(!unresolved.evaluation.root_satisfied && !unresolved.slots[0].execution.check.conclusive);
            check(tasks.active("a")->phase == TaskPhase::Unresolved);
            check(!memory.summary("a") && memory.constraints("a").empty());

            const auto bounded_user = memory.append("a", Role::User, "7", source.context().views[0].text);
            const auto bounded_task = std::get<SessionTask>(recovered.accept("a", bounded_user.id, source,
                source.snapshot(), dsl(), tasks.active("a")->revision));
            rejects(ErrorCode::Budget, [&] { recovered.execute("a", bounded_task.revision, {}, {1, 1}); });
            check(tasks.active("a")->phase == TaskPhase::Failed);
            check(tasks.active("a")->certificates.empty() && model.state() == ModelState::Unloaded);

            SessionTasks ephemeral(":memory:");
            rejects(ErrorCode::Schema, [&] { TaskSession invalid(memory, ephemeral, journal, model); });
        }
        {
            Directory plans;
            const auto memory_path = plans.path / "conversation.db";
            const auto tasks_path = plans.path / "tasks.db";
            const auto objects_path = plans.path / "journal.db";
            std::optional<TaskPlanContext> capture;
            SessionTask state;
            std::string proposal;
            {
                ConversationMemory memory(memory_path);
                memory.open_session("plan", "project");
                const auto user = memory.append("plan", Role::User, "1", "Generate code.");
                auto context = catalog(user.content).context();
                context.views[0].archive_id = "private-conversation:" + evidence_digest(user.session);
                context.views[0].entry_path = "message:" + std::to_string(user.id);
                capture.emplace(ProblemSourceCatalog(std::move(context)), 0);
                proposal = "GAMMATHINK_TASK_IR 1\nCONTEXT \"" + capture->snapshot()
                    + "\"\nROOT \"write\"\nENTITY [\"input\",\"text\",\"s0\",[\"s0\"]]\n"
                    "ENTITY [\"output\",\"artifact\",null,[\"s0\"]]\n"
                    "GOAL [\"write\",[],[],[],[],[\"s0\"]]\n"
                    "ACTION [\"write\",\"write\",\"generate-code\",[\"input\"],[\"output\"],[\"s0\"]]\nEND\n";
                SessionTasks tasks(tasks_path);
                Journal journal(objects_path);
                NativeModelManager model;
                TaskSession controller(memory, tasks, journal, model);
                ProblemContext host;
                host.unit_registry_id = "session"; host.unit_registry_version = "1";
                ModelSpec missing{"missing-interpreter", plans.path / "missing.gguf", Digest{1}, ModelRole::Interpreter};
                rejects(ErrorCode::Storage, [&] { (void)controller.interpret_plan("plan", missing, host, 0); });
                check(model.state() == ModelState::Unloaded && !tasks.active("plan") &&
                      journal.objects().objects.empty());
                auto wrong_role = missing;
                wrong_role.role = ModelRole::CodeGeneration;
                rejects(ErrorCode::Schema, [&] { (void)controller.interpret_plan("plan", wrong_role, host, 0); });
                TaskInterpretationOptions invalid;
                invalid.output_tokens = 0;
                rejects(ErrorCode::Budget, [&] { (void)controller.interpret_plan("plan", missing, host, 0, invalid); });
                rejects(ErrorCode::Schema, [&] {
                    (void)controller.accept_plan("plan", user.id, *capture, capture->snapshot(), "bad", 0);
                });
                check(model.state() == ModelState::Unloaded && !tasks.active("plan"));
                const auto declined = controller.accept_plan("plan", user.id, *capture, capture->snapshot(),
                    "GAMMATHINK_TASK_IR 1\nCONTEXT \"" + capture->snapshot() +
                    "\"\nCLARIFY [\"Missing language\",[\"s0\"]]\nEND\n", 0);
                check(std::holds_alternative<TaskPlanDecline>(declined));
                check(!tasks.active("plan") && journal.objects().objects.empty());
                state = std::get<SessionTask>(controller.accept_plan("plan", user.id, *capture,
                    capture->snapshot(), proposal, 0));
                check(model.state() == ModelState::Unloaded && state.phase == TaskPhase::Interpreted);
                check(journal.contains(state.ordered_task) && journal.by_kind(ObjectKind::TestResult).objects.empty());
                const auto ready = controller.active_plan("plan", state.revision, *capture);
                check(ready.dsl() == proposal);
                check(evaluate_task_plan(*capture, ready, {}, {}).ready_actions == std::vector<std::string>{"write"});
                rejects(ErrorCode::Schema, [&] { (void)controller.execute("plan", state.revision); });
                check(tasks.active("plan")->phase == TaskPhase::Interpreted);
            }
            {
                ConversationMemory memory(memory_path);
                SessionTasks tasks(tasks_path);
                Journal journal(objects_path);
                NativeModelManager model;
                TaskSession controller(memory, tasks, journal, model);
                check(controller.active_plan("plan", state.revision, *capture).dsl() == proposal);
                TaskPlanContext changed(capture->sources(), capture->current_question(), {}, {{"offline", GoalComparison::Equal, true}});
                rejects(ErrorCode::Integrity, [&] { (void)controller.active_plan("plan", state.revision, changed); });
                rejects(ErrorCode::Schema, [&] { (void)controller.active_plan("plan", state.revision + 1, *capture); });
                const auto later = memory.append("plan", Role::User, "2", "Generate code.");
                rejects(ErrorCode::Schema, [&] { (void)controller.active_plan("plan", state.revision, *capture); });
                rejects(ErrorCode::Schema, [&] {
                    (void)controller.accept_plan("plan", later.id, *capture, capture->snapshot(), proposal, state.revision);
                });
                check(tasks.active("plan")->revision == state.revision);
                check(model.state() == ModelState::Unloaded && journal.by_kind(ObjectKind::TestResult).objects.empty());
            }
        }
        {
            Directory native;
            ConversationMemory memory(native.path / "conversation.db");
            memory.open_session("native", "project");
            const auto user = memory.append("native", Role::User, "1",
                "Robin flies. Check twice and explain the result.");
            memory.revise_constraint("native", {"network", "Remain offline.", 0, user.id, true}, 0);
            auto host = source.context();
            host.views.clear();
            const auto prompt = prepare_task_prompt(retrieve_context(memory, "native"), host,
                [](std::string_view input) { return input.size(); }, {1000000, 262144, 256});
            SessionTasks tasks(native.path / "tasks.db");
            Journal journal(native.path / "journal.db");
            JournalArchive local(journal);
            UnifiedKnowledge knowledge({&local});
            NativeModelManager model;
            auto controller = std::make_unique<TaskSession>(memory, tasks, journal, model);
            const auto proposal = "GAMMATHINK_TASK_IR 1\nCONTEXT " + quote(prompt.context.snapshot()) +
                "\nROOT \"report\"\nENTITY [\"input\",\"problem\"," + quote(dsl()) + ",[\"s0\"]]\n"
                "ENTITY [\"result\",\"artifact\",null,[\"s0\"]]\n"
                "ENTITY [\"reply\",\"artifact\",null,[\"s0\"]]\n"
                "GOAL [\"work\",[],[],[],[],[\"s0\"]]\n"
                "GOAL [\"report\",[],[],[],[],[\"s0\"]]\n"
                "ACTION [\"compute\",\"work\",\"reason\",[\"input\"],[\"result\"],[\"s0\"]]\n"
                "ACTION [\"describe\",\"report\",\"explain\",[\"result\"],[\"reply\"],[\"s0\"]]\nEND\n";
            const auto admitted = std::get<SessionTask>(controller->accept_plan("native", user.id,
                prompt.context, prompt.context.snapshot(), proposal, 0));
            TaskExecutionState execution;
            const auto result = controller->execute_native_action("native", admitted.revision,
                prompt.context, "compute", execution, knowledge, {});
            check(result.status == KnowledgeRunStatus::Checked && result.certificates.size() == 2);
            check(result.native_executions == 1 && result.outcome.verified && result.outcome.succeeded);
            check(execution.artifacts.at("result") == result.receipt);
            auto waiting = tasks.active("native").value();
            check(waiting.phase == TaskPhase::Unresolved && waiting.certificates == result.certificates);
            const auto plan = controller->active_plan("native", waiting.revision, prompt.context);
            const auto evaluation = evaluate_task_plan(prompt.context, plan, execution.outcomes, {}, execution.artifacts);
            check(!evaluation.goals.root_satisfied && evaluation.ready_actions == std::vector<std::string>{"describe"});
            check(model.state() == ModelState::Unloaded && model.metrics().file_bytes == 0);
            const auto completed = tasks.completed_actions("native", waiting.ordered_task, waiting.revision);
            check(completed.size() == 1 && completed[0].action == "compute" && completed[0].receipt == result.receipt);
            TaskExecutionState forgotten;
            rejects(ErrorCode::Integrity, [&] {
                (void)controller->execute_native_action("native", waiting.revision, prompt.context,
                    "compute", forgotten, knowledge, {});
            });
            check(tasks.active("native")->revision == waiting.revision);
            // Crash after the next phase begins: its metadata intentionally has
            // no result certificates. Replay must reconstruct the prior proofs.
            waiting = tasks.transition("native", waiting.revision, TaskPhase::Reasoning);
            controller.reset();
            TaskSession recovered(memory, tasks, journal, model);
            const auto restored = recovered.restore_native_execution("native", waiting.revision,
                prompt.context, knowledge, {});
            check(restored.outcomes.at("work").verified && restored.outcomes.at("work").succeeded);
            check(restored.certificates == result.certificates);
            check(restored.artifacts.at("result") != result.receipt);
            const auto replayed = journal.get(restored.artifacts.at("result")).value();
            auto replay_payload = std::unique_ptr<json_object, decltype(&json_object_put)>(
                json_tokener_parse(std::string(replayed.payload.begin(), replayed.payload.end()).c_str()), json_object_put);
            check(json_object_get_int64(json_object_object_get(replay_payload.get(), "native_executions")) == 0);
            check(tasks.active("native")->revision == waiting.revision &&
                  tasks.completed_actions("native", waiting.ordered_task, waiting.revision).size() == 1);
            check(evaluate_task_plan(prompt.context, plan, restored.outcomes, {}, restored.artifacts).ready_actions ==
                  std::vector<std::string>{"describe"});
            {
                memory.open_session("delivery", "project");
                const auto question = memory.append("delivery", Role::User, "1", user.content);
                const auto delivery_prompt = prepare_task_prompt(retrieve_context(memory, "delivery"), host,
                    [](std::string_view input) { return input.size(); }, {1000000, 262144, 256});
                auto delivery_proposal = proposal;
                delivery_proposal.replace(delivery_proposal.find(prompt.context.snapshot()),
                    prompt.context.snapshot().size(), delivery_prompt.context.snapshot());
                const auto delivery_task = std::get<SessionTask>(recovered.accept_plan("delivery", question.id,
                    delivery_prompt.context, delivery_prompt.context.snapshot(), delivery_proposal, 0));
                TaskExecutionState delivery;
                recovered.execute_native_action("delivery", delivery_task.revision, delivery_prompt.context,
                    "compute", delivery, knowledge, {});
                auto revision = tasks.active("delivery")->revision;
                rejects(ErrorCode::Permission, [&] {
                    (void)recovered.deliver_native_response("delivery", revision, delivery_prompt.context,
                        "describe", knowledge, {}, "2");
                });
                check(memory.recent("delivery").messages.size() == 1 && tasks.active("delivery")->revision == revision);
                const auto explanation = recovered.execute_native_action("delivery", revision,
                    delivery_prompt.context, "describe", delivery, knowledge, {});
                revision = tasks.active("delivery")->revision;
                check(explanation.response.has_value() && tasks.active("delivery")->phase == TaskPhase::Checked);
                check(delivery.certificates.size() == 2 && delivery.steps.size() == 2);
                NativeTaskLimits tiny; tiny.response_bytes = 64;
                rejects(ErrorCode::Budget, [&] {
                    (void)recovered.deliver_native_response("delivery", revision, delivery_prompt.context,
                        "describe", knowledge, {}, "2", {}, {}, tiny);
                });
                check(memory.recent("delivery").messages.size() == 1 && tasks.active("delivery")->revision == revision);
                memory.revise_constraint("delivery", {"network", "offline", 0, question.id, true}, 0);
                rejects(ErrorCode::Integrity, [&] {
                    (void)recovered.deliver_native_response("delivery", revision, delivery_prompt.context,
                        "describe", knowledge, {}, "2");
                });
                memory.revise_constraint("delivery", {"network", "withdrawn", 0, question.id, false}, 1);
                const auto reply = recovered.deliver_native_response("delivery", revision, delivery_prompt.context,
                    "describe", knowledge, {}, "2");
                check(reply.role == Role::Assistant && reply.session == "delivery");
                check(memory.get("delivery", question.id) == question &&
                      memory.get("delivery", reply.id) == reply);
                auto document = std::unique_ptr<json_object, decltype(&json_object_put)>(
                    json_tokener_parse(reply.content.c_str()), json_object_put);
                check(std::string_view(json_object_get_string(json_object_object_get(document.get(), "profile"))) ==
                      "gammathink.conversation-response.v1");
                check(json_object_get_uint64(json_object_object_get(document.get(), "source_message")) == question.id);
                check(json_object_get_boolean(json_object_object_get(document.get(), "root_satisfied")));
                auto* details = json_object_object_get(json_object_object_get(document.get(), "result"), "response");
                check(json_object_get_int(json_object_object_get(details, "source_status")) ==
                      static_cast<int>(KnowledgeRunStatus::Checked));
                check(json_object_get_uint64(json_object_object_get(details, "source_native_executions")) == 0);
                check(!json_object_get_boolean(json_object_object_get(details, "empirical_truth_verified")) &&
                      !json_object_get_boolean(json_object_object_get(details, "source_translation_qualified")));
                check(json_object_array_length(json_object_object_get(details, "native_results")) == 2);
                rejects(ErrorCode::Schema, [&] {
                    (void)recovered.deliver_native_response("delivery", revision, delivery_prompt.context,
                        "describe", knowledge, {}, "3");
                });
                check(memory.recent("delivery").messages.size() == 2 && model.state() == ModelState::Unloaded);
            }
            journal.invalidate(result.receipt, "Synthetic local withdrawal");
            const auto before_invalid_replay = journal.objects().objects.size();
            rejects(ErrorCode::Integrity, [&] {
                (void)recovered.restore_native_execution("native", waiting.revision, prompt.context, knowledge, {});
            });
            auto valid_state = restored;
            rejects(ErrorCode::Integrity, [&] {
                (void)recovered.execute_native_action("native", waiting.revision, prompt.context,
                    "describe", valid_state, knowledge, {});
            });
            check(tasks.active("native")->revision == waiting.revision &&
                  journal.objects().objects.size() == before_invalid_replay);
            const auto object_count = journal.objects().objects.size();
            memory.revise_constraint("native", {"network", "Keep all execution local.", 0, user.id, true}, 1);
            rejects(ErrorCode::Integrity, [&] { (void)recovered.active_plan("native", waiting.revision, prompt.context); });
            rejects(ErrorCode::Integrity, [&] {
                (void)recovered.execute_native_action("native", waiting.revision, prompt.context,
                    "compute", execution, knowledge, {});
            });
            rejects(ErrorCode::Integrity, [&] {
                (void)recovered.restore_native_execution("native", waiting.revision, prompt.context, knowledge, {});
            });
            check(tasks.active("native")->revision == waiting.revision &&
                  journal.objects().objects.size() == object_count);
            check(execution.outcomes.size() == 1 && execution.artifacts.size() == 1);
            rejects(ErrorCode::Integrity, [&] {
                (void)recovered.accept_plan("native", user.id, prompt.context,
                    prompt.context.snapshot(), proposal, waiting.revision);
            });
        }
        std::cout << "task session integration passed: " << checks << " deterministic assertions\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
