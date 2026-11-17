#include "task_session.hpp"

#include <iostream>
#include <json-c/json.h>
#include <unistd.h>

namespace {
using namespace axiom;
using namespace axiom::context;
using namespace axiom::knowledge;
using namespace axiom::runtime;
std::size_t assertions = 0;
void check(bool condition) {
    ++assertions;
    if (!condition) throw std::runtime_error("Refinement assertion " + std::to_string(assertions));
}
template<class Function> void rejects(ErrorCode code, Function function) {
    try { function(); } catch (const Error& error) { check(error.code == code); return; }
    throw std::runtime_error("Expected refinement rejection");
}
struct Directory {
    std::filesystem::path path;
    Directory() {
        auto pattern = (std::filesystem::temp_directory_path() / "task-refinement-XXXXXX").string();
        const auto* created = mkdtemp(pattern.data());
        if (!created) throw std::runtime_error("Cannot create refinement fixture");
        path = created;
    }
    ~Directory() { std::filesystem::remove_all(path); }
};
std::string quote(std::string_view text) {
    std::unique_ptr<json_object, decltype(&json_object_put)> value(
        json_object_new_string_len(text.data(), static_cast<int>(text.size())), json_object_put);
    return json_object_to_json_string_ext(value.get(), JSON_C_TO_STRING_PLAIN);
}
std::string problem(bool negative = false) {
    return "GAMMATHINK_DSL 1\nENTITY \"robin\"\nPREDICATE [\"flies\",1]\n"
        "RULE [0,[0," + std::string(negative ? "true" : "false") + ",[[\"c\",0]]],[],[\"s0\"]]\n"
        "GOAL {\"goal\":[\"atom\",[0,false,[[\"c\",0]]]],\"supports\":[\"s0\"]}\n"
        "GOAL {\"goal\":[\"atom\",[0,false,[[\"c\",0]]]],\"supports\":[\"s0\"]}\nEND\n";
}
std::string plan(const TaskPlanContext& context, bool audit = false, bool changed = false) {
    std::string result = "GAMMATHINK_TASK_IR 1\nCONTEXT " + quote(context.snapshot()) + "\nROOT \"report\"\n"
        "ENTITY [\"input\",\"problem\"," + quote(problem(changed)) + ",[\"s0\"]]\n"
        "ENTITY [\"result\",\"artifact\",null,[\"s0\"]]\n"
        "ENTITY [\"reply\",\"artifact\",null,[\"s0\"]]\n"
        "GOAL [\"work\",[],[],[],[],[\"s0\"]]\n"
        "GOAL [\"report\"," + std::string(audit ? "[[\"audit\",\"work\",\"work\"]]" : "[[\"work\",\"work\"]]") +
        ",[],[],[],[\"s0\"]]\n"
        "ACTION [\"compute\",\"work\",\"reason\",[\"input\"],[\"result\"],[\"s0\"]]\n"
        "ACTION [\"describe\",\"report\",\"explain\",[\"result\"],[\"reply\"],[\"s0\"]]\n";
    if (audit) result +=
        "ENTITY [\"audited\",\"artifact\",null,[\"s0\"]]\n"
        "GOAL [\"audit\",[],[],[],[],[\"s0\"]]\n"
        "ACTION [\"audit-action\",\"audit\",\"reason\",[\"input\"],[\"audited\"],[\"s0\"]]\n";
    return result + "END\n";
}
}

int main() {
    try {
        Directory directory;
        ConversationMemory memory(directory.path / "conversation.db");
        SessionTasks tasks(directory.path / "tasks.db");
        Journal journal(directory.path / "journal.db");
        JournalArchive local(journal); UnifiedKnowledge knowledge({&local});
        neural::NativeModelManager model;
        ProblemContext host; host.unit_registry_id = "refinement"; host.unit_registry_version = "1";
        const auto capture = [&](std::string_view session) {
            return prepare_task_prompt(retrieve_context(memory, session), host,
                [](std::string_view text) { return text.size(); }, {1000000, 262144, 256}, {},
                {{"network", GoalComparison::Equal, false}});
        };
        memory.open_session("main", "test");
        const auto user = memory.append("main", Role::User, "1", "Robin flies. Check this twice and report.");
        const auto pin = memory.revise_constraint("main", {"network", "offline", 0, user.id, true}, 0);
        const auto initial = capture("main");
        std::uint64_t revision = 0;
        ObjectId original_task, refined_task;
        std::vector<ObjectId> original_certificates;
        {
            TaskSession controller(memory, tasks, journal, model);
            const auto accepted = std::get<SessionTask>(controller.accept_plan("main", user.id, initial.context,
                initial.context.snapshot(), plan(initial.context), 0));
            original_task = accepted.ordered_task;
            TaskExecutionState state;
            const auto computed = controller.execute_native_action("main", accepted.revision, initial.context,
                "compute", state, knowledge, {{"network", false}});
            check(computed.certificates.size() == 2);
            original_certificates = computed.certificates;
            revision = tasks.active("main")->revision;
            memory.revise_constraint("main", {"network", "local-only", 0, user.id, true}, pin.revision);
            const auto fresh = capture("main");
            rejects(ErrorCode::Integrity, [&] { (void)controller.refine_plan("main", revision, fresh.context,
                fresh.context.snapshot(), plan(fresh.context, true, true)); });
            check(tasks.active("main")->revision == revision);
            const auto refined = std::get<SessionTask>(controller.refine_plan("main", revision, fresh.context,
                fresh.context.snapshot(), plan(fresh.context, true)));
            check(refined.user_message == user.id && refined.message_hash == user.content_hash);
            check(refined.ordered_task != original_task && refined.phase == TaskPhase::Interpreted);
            check(refined.certificates.empty());
            check(tasks.completed_actions("main", refined.ordered_task, refined.revision).empty());
            check(tasks.completed_actions("main", original_task, refined.revision).size() == 1);
            check(tasks.refinement("main", refined.ordered_task)->parent == original_task);
            for (const auto& certificate : original_certificates) check(journal.contains(certificate));
            rejects(ErrorCode::Schema, [&] { (void)controller.active_plan("main", revision, initial.context); });
            rejects(ErrorCode::Integrity, [&] { (void)controller.execute_native_action("main", refined.revision, fresh.context,
                "compute", state, knowledge, {{"network", false}}); });
            refined_task = refined.ordered_task; revision = refined.revision;
        }
        {
            const auto fresh = capture("main");
            TaskSession restored(memory, tasks, journal, model);
            const auto admitted = restored.active_plan("main", revision, fresh.context);
            check(admitted.graph().nodes[admitted.graph().root].id == "report");
            auto state = restored.restore_native_execution("main", revision, fresh.context, knowledge, {{"network", false}});
            check(state.steps.empty() && state.outcomes.empty() && state.certificates.empty());
            const auto audited = restored.execute_native_action("main", revision, fresh.context,
                "audit-action", state, knowledge, {{"network", false}});
            check(audited.status == KnowledgeRunStatus::Checked && audited.certificates.size() == 2);
            revision = tasks.active("main")->revision;
            (void)restored.execute_native_action("main", revision, fresh.context, "compute", state, knowledge, {{"network", false}});
            revision = tasks.active("main")->revision;
            const auto explained = restored.execute_native_action("main", revision, fresh.context,
                "describe", state, knowledge, {{"network", false}});
            check(explained.response.has_value() && tasks.active("main")->phase == TaskPhase::Checked);
            check(tasks.active("main")->ordered_task == refined_task);
            const auto reply = restored.deliver_native_response("main", tasks.active("main")->revision,
                fresh.context, "describe", knowledge, {{"network", false}}, "2");
            check(reply.role == Role::Assistant && model.state() == neural::ModelState::Unloaded);
        }
        // A metadata reference is not evidence of a checked refinement.
        memory.open_session("forged", "test");
        const auto forged_user = memory.append("forged", Role::User, "1", "Robin flies. Check this twice and report.");
        const auto original = capture("forged");
        {
            TaskSession controller(memory, tasks, journal, model);
            const auto accepted = std::get<SessionTask>(controller.accept_plan("forged", forged_user.id, original.context,
                original.context.snapshot(), plan(original.context), 0));
            const auto next = persist_task_plan(journal, std::get<AdmittedTaskPlan>(
                admit_task_plan(original.context, original.context.snapshot(), plan(original.context, true))));
            Object fake; fake.kind = ObjectKind::TestResult; fake.label = "Task refinement";
            fake.format = "gammathink.task-refinement.v1";
            const std::string payload = "{\"typed_intent_preserved\":true}";
            fake.payload.assign(payload.begin(), payload.end());
            fake.links = {{Relationship::DependsOn, accepted.ordered_task}, {Relationship::DependsOn, next.manifest}};
            const auto forged = tasks.refine(memory, "forged", next.manifest, journal.put(fake), accepted.revision);
            rejects(ErrorCode::Integrity, [&] { (void)controller.active_plan("forged", forged.revision, original.context); });
        }
        std::cout << "Task refinement checks passed: " << assertions << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
