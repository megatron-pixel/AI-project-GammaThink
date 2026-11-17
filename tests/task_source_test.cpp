#include "task_session.hpp"

#include <iostream>
#include <json-c/json.h>
#include <unistd.h>
#include <zim/writer/creator.h>
#include <zim/writer/item.h>

namespace {
using namespace axiom;
using namespace axiom::context;
using namespace axiom::knowledge;
using namespace axiom::runtime;
std::size_t assertions = 0;
void check(bool value) {
    ++assertions;
    if (!value) throw std::runtime_error("Task source assertion " + std::to_string(assertions));
}
template<class Function> void rejects(ErrorCode code, Function function) {
    try { function(); } catch (const Error& error) {
        if (error.code != code) throw std::runtime_error("Unexpected source rejection code " +
            std::to_string(static_cast<int>(error.code)) + ": " + error.what());
        check(true); return;
    }
    throw std::runtime_error("Expected source action rejection");
}
struct Directory {
    std::filesystem::path path;
    Directory() {
        auto pattern = (std::filesystem::temp_directory_path() / "task-source-XXXXXX").string();
        const auto* created = mkdtemp(pattern.data());
        if (!created) throw std::runtime_error("Cannot create source test directory");
        path = created;
    }
    ~Directory() { std::filesystem::remove_all(path); }
};
void archive(const std::filesystem::path& path) {
    zim::writer::Creator creator;
    creator.configNbWorkers(1).configIndexing(false, "en").configClusterSize(4096);
    creator.startZimCreation(path.string());
    creator.addItem(zim::writer::StringItem::create("article", "text/plain",
        "Synthetic source", zim::writer::Hints{}, "Robin flies."));
    creator.setMainPath("article"); creator.finishZimCreation();
}
std::string quote(std::string_view value) {
    std::unique_ptr<json_object, decltype(&json_object_put)> object(
        json_object_new_string_len(value.data(), static_cast<int>(value.size())), json_object_put);
    return json_object_to_json_string_ext(object.get(), JSON_C_TO_STRING_PLAIN);
}
// Data-only capture framing control, never presented as a real model run.
ObjectId synthetic_candidate(Journal& journal, const Object& source, const SessionTask& task,
    const AdmittedTaskPlan& plan, const TaskSourceCompilation& input, bool complete = true) {
    const auto artifact = [&](std::string_view format, std::string payload) {
        Object value; value.kind = ObjectKind::Evidence; value.label = format; value.format = format;
        value.payload.assign(payload.begin(), payload.end()); value.sources = source.sources;
        value.links = {{Relationship::DependsOn, task.ordered_task}, {Relationship::DependsOn, input.translation.source}};
        return value;
    };
    const auto prompt = journal.put(artifact("gammathink.source-translation-prompt.v1", source_translation_input(input)));
    const auto identity = journal.put(artifact("gammathink.source-translation-model.v1",
        "{\"id\":\"synthetic-not-a-model\",\"path\":" + quote("/synthetic/not-a-model.gguf") + ",\"sha256\":" +
        quote(hex(ObjectId{Digest{1}})) + ",\"system_policy\":" + quote(source_translation_policy) +
        ",\"role\":3,\"enable_thinking\":false,\"file_bytes\":1,\"retained_parameters\":1,\"runtime_parameters\":1,"
        "\"sampled_peak_rss\":1,\"unloaded_rss\":1,\"context_tokens\":8192,\"output_tokens\":512}"));
    const auto& action = plan.actions().front();
    auto value = artifact("gammathink.source-translation-candidate.v1",
        "{\"manifest\":" + quote(hex(task.ordered_task)) + ",\"action\":" + quote(action.id) +
        ",\"goal\":" + quote(action.goal) + ",\"task_snapshot\":" + quote(plan.snapshot()) +
        ",\"source\":" + quote(hex(input.translation.source)) + ",\"source_context\":" +
        quote(serialize_problem_context(input.context)) + ",\"context_snapshot\":" +
        quote(input.translation.context_snapshot) + ",\"prompt\":" + quote(hex(prompt)) +
        ",\"model\":" + quote(hex(identity)) + ",\"archive_dsl\":" + quote(input.translation.archive_dsl) +
        ",\"selectors\":" + std::to_string(static_cast<int>(input.selectors)) +
        ",\"input_tokens\":100,\"output_tokens\":12,\"stop\":" + std::string(complete ? "0" : "1") +
        ",\"source_translation_qualified\":false,\"empirical_truth_verified\":false}");
    value.links.push_back({Relationship::DependsOn, prompt}); value.links.push_back({Relationship::DependsOn, identity});
    return journal.put(value);
}
std::string plan(const TaskPlanContext& context) {
    return "GAMMATHINK_TASK_IR 1\nCONTEXT " + quote(context.snapshot()) + "\nROOT \"report\"\n"
        "ENTITY [\"raw\",\"object\",\"document\",[\"s0\"]]\n"
        "ENTITY [\"compiled\",\"artifact\",null,[\"s0\"]]\n"
        "ENTITY [\"reply\",\"artifact\",null,[\"s0\"]]\n"
        "GOAL [\"work\",[],[],[[\"source_materialized\",\"eq\",true]],[],[\"s0\"]]\n"
        "GOAL [\"report\",[],[],[],[],[\"s0\"]]\n"
        "ACTION [\"compile\",\"work\",\"compile-source\",[\"raw\"],[\"compiled\"],[\"s0\"]]\n"
        "ACTION [\"describe\",\"report\",\"explain\",[\"compiled\"],[\"reply\"],[\"s0\"]]\nEND\n";
}
}

int main(int argc, char** argv) {
    try {
        if (argc == 3 && std::string_view(argv[1]) == "--fixture") {
            archive(argv[2]); return 0;
        }
        check(argc == 1);
        Directory directory;
        archive(directory.path / "source.zim");
        const ZimSourceArchive reader(directory.path / "source.zim");
        const auto source = reader.read("article");
        const auto& reference = source.sources.front();
        ConversationMemory memory(directory.path / "conversation.db");
        SessionTasks tasks(directory.path / "tasks.db");
        Journal journal(directory.path / "journal.db");
        neural::NativeModelManager model;
        JournalArchive local(journal); UnifiedKnowledge knowledge({&local});
        const auto source_id = journal.put(source);
        ProblemContext host; host.unit_registry_id = "source-test"; host.unit_registry_version = "1";
        TaskSourceCompilation input; input.context = host;
        input.context.views.push_back(make_evidence_view("source", PremiseBasis::Source, "Robin flies.",
            reference.archive_id, reference.entry_path, reference.extractor_version));
        input.translation = {source_id, ProblemSourceCatalog(input.context).snapshot(),
            "GAMMATHINK_DSL 1\nENTITY \"robin\"\nPREDICATE [\"flies\",1]\n"
            "RULE [0,[0,false,[[\"c\",0]]],[],[[0,0,12,null]]]\nEND\n"};
        const auto capture = [&](std::string_view session) {
            return prepare_task_prompt(retrieve_context(memory, session), host,
                [](std::string_view text) { return text.size(); }, {1000000, 262144, 256},
                {{"document", {source_id, ObjectKind::Evidence, source.format}}});
        };
        memory.open_session("main", "test");
        const auto message = memory.append("main", Role::User, "1", "Compile this complete library source and report.");
        const auto prepared = capture("main");
        std::uint64_t revision = 0;
        ObjectId module_id, receipt_id;
        {
            TaskSession session(memory, tasks, journal, model);
            const auto accepted = session.accept_plan("main", message.id, prepared.context,
                prepared.context.snapshot(), plan(prepared.context), 0);
            check(std::holds_alternative<SessionTask>(accepted));
            revision = std::get<SessionTask>(accepted).revision;
            TaskExecutionState state;
            const auto unchanged = [&] {
                check(tasks.active("main")->revision == revision);
                check(tasks.history("main").events.size() == 1);
                check(model.state() == neural::ModelState::Unloaded);
            };
            auto wrong = input; wrong.translation.source.digest[0] ^= 1;
            rejects(ErrorCode::Integrity, [&] { (void)session.compile_source_action("main", revision,
                prepared.context, "compile", state, {}, wrong); }); unchanged();
            wrong = input; wrong.translation.context_snapshot = "changed";
            rejects(ErrorCode::Integrity, [&] { (void)session.compile_source_action("main", revision,
                prepared.context, "compile", state, {}, wrong); }); unchanged();
            wrong = input; wrong.context.views[0].text += " Added premise.";
            wrong.context.views[0].digest = evidence_digest(wrong.context.views[0].text);
            rejects(ErrorCode::Integrity, [&] { (void)session.compile_source_action("main", revision,
                prepared.context, "compile", state, {}, wrong); }); unchanged();
            wrong = input; wrong.storage.objects = 3;
            rejects(ErrorCode::Budget, [&] { (void)session.compile_source_action("main", revision,
                prepared.context, "compile", state, {}, wrong); }); unchanged();
            wrong = input; wrong.storage.encoded_bytes = 1;
            rejects(ErrorCode::Budget, [&] { (void)session.compile_source_action("main", revision,
                prepared.context, "compile", state, {}, wrong); }); unchanged();
            wrong = input; wrong.translation.archive_dsl = "GAMMATHINK_DSL 1\nGOAL [\"answer\",true]\nEND\n";
            rejects(ErrorCode::Schema, [&] { (void)session.compile_source_action("main", revision,
                prepared.context, "compile", state, {}, wrong); }); unchanged();
            const auto compiled = session.compile_source_action("main", revision, prepared.context, "compile", state, {}, input);
            revision = tasks.active("main")->revision;
            module_id = compiled.artifact; receipt_id = compiled.receipt;
            check(compiled.status == KnowledgeRunStatus::Checked && compiled.outcome.succeeded);
            check(compiled.certificates.empty() && compiled.results.empty() && compiled.native_executions == 0);
            check(std::get<bool>(compiled.outcome.observed.at("source_materialized")));
            check(!std::get<bool>(compiled.outcome.observed.at("source_translation_qualified")));
            check(!std::get<bool>(compiled.outcome.observed.at("empirical_truth_verified")));
            check(std::get<std::int64_t>(compiled.outcome.observed.at("source_members")) == 1);
            check(!journal.assessment(module_id) && !journal.assessment(source_id));
            const auto module = journal.get(module_id);
            check(module && module->format == "gammathink.archive-dsl.v1" && module->visibility == Visibility::Private);
            check(std::string(module->payload.begin(), module->payload.end()) == input.translation.archive_dsl);
            check(module->sources == source.sources && journal.get(source_id) == source);
            const auto context_id = parse_id(std::get<std::string>(compiled.outcome.observed.at("source_context")));
            const auto context = journal.get(context_id);
            check(context && context->kind == ObjectKind::Assumption && !journal.assessment(context_id));
            const auto query = source_query_request(source, *module, *context, input.context,
                Atom{0, false, {{TermKind::Constant, 0}}});
            const auto result = execute_problem(query);
            check(result.status == ProblemStatus::Checked && verify_problem_result(query, result));
            check(compiled.certificates.empty());
        }
        {
            TaskSession reopened(memory, tasks, journal, model);
            auto state = reopened.restore_native_execution("main", revision, prepared.context, knowledge, {});
            check(state.steps.at("compile").receipt == receipt_id && state.artifacts.at("compiled") == module_id);
            check(std::get<bool>(state.steps.at("compile").outcome.observed.at("source_record_restored")));
            check(state.steps.at("compile").native_executions == 0 && state.certificates.empty());
            const auto explained = reopened.execute_native_action("main", revision, prepared.context,
                "describe", state, knowledge, {});
            check(explained.response.has_value() && explained.certificates.empty());
            const auto delivered = reopened.deliver_native_response("main", tasks.active("main")->revision,
                prepared.context, "describe", knowledge, {}, "2");
            check(delivered.role == Role::Assistant && model.state() == neural::ModelState::Unloaded);
        }
        // A source compiler decline is retained without a materialized module.
        memory.open_session("decline", "test");
        const auto declined_message = memory.append("decline", Role::User, "1", "Compile the source.");
        const auto declined_context = capture("decline");
        {
            TaskSession session(memory, tasks, journal, model);
            const auto accepted = std::get<SessionTask>(session.accept_plan("decline", declined_message.id,
                declined_context.context, declined_context.context.snapshot(), plan(declined_context.context), 0));
            auto declined = input;
            declined.translation.archive_dsl = "GAMMATHINK_DSL 1\nUNSUPPORTED \"outside the representation\"\nEND\n";
            TaskExecutionState state;
            const auto result = session.compile_source_action("decline", accepted.revision,
                declined_context.context, "compile", state, {}, declined);
            check(result.status == KnowledgeRunStatus::Unresolved && !result.outcome.succeeded);
            check(result.artifact == result.receipt && !std::get<bool>(result.outcome.observed.at("source_materialized")));
            const auto restored = session.restore_native_execution("decline", tasks.active("decline")->revision,
                declined_context.context, knowledge, {});
            check(restored.steps.at("compile").receipt == result.receipt);
            check(restored.certificates.empty() && model.state() == neural::ModelState::Unloaded);
        }
        // Host metadata alone cannot forge a source-meaning qualification.
        memory.open_session("model-candidate", "test");
        const auto candidate_message = memory.append("model-candidate", Role::User, "1", "Compile this exact source.");
        const auto candidate_context = capture("model-candidate");
        {
            TaskSession session(memory, tasks, journal, model);
            const auto task = std::get<SessionTask>(session.accept_plan("model-candidate", candidate_message.id,
                candidate_context.context, candidate_context.context.snapshot(), plan(candidate_context.context), 0));
            const auto admitted = session.active_plan("model-candidate", task.revision, candidate_context.context);
            TaskExecutionState state;
            auto selected_input = input;
            auto generation_input = selected_input; generation_input.translation.archive_dsl.clear();
            neural::ModelSpec wrong_role{"synthetic", directory.path / "not-a-model.gguf", {}, neural::ModelRole::Interpreter};
            rejects(ErrorCode::Schema, [&] { (void)session.translate_source_action("model-candidate", task.revision,
                candidate_context.context, "compile", state, {}, generation_input, wrong_role); });
            wrong_role.role = neural::ModelRole::KnowledgeExtraction;
            auto changed_source = generation_input; changed_source.context.views.front().text += " changed";
            rejects(ErrorCode::Integrity, [&] { (void)session.translate_source_action("model-candidate", task.revision,
                candidate_context.context, "compile", state, {}, changed_source, wrong_role); });
            auto tiny = generation_input; tiny.storage.objects = 2;
            rejects(ErrorCode::Budget, [&] { (void)session.translate_source_action("model-candidate", task.revision,
                candidate_context.context, "compile", state, {}, tiny, wrong_role); });
            check(model.state() == neural::ModelState::Unloaded && model.metrics().file_bytes == 0);
            check(tasks.active("model-candidate")->revision == task.revision && state.steps.empty());

            selected_input.candidate = synthetic_candidate(journal, source, task, admitted, selected_input, false);
            rejects(ErrorCode::Budget, [&] { (void)session.compile_source_action("model-candidate", task.revision,
                candidate_context.context, "compile", state, {}, selected_input); });
            selected_input.candidate = synthetic_candidate(journal, source, task, admitted, input);
            auto repaired = selected_input; repaired.translation.archive_dsl += "\n";
            rejects(ErrorCode::Integrity, [&] { (void)session.compile_source_action("model-candidate", task.revision,
                candidate_context.context, "compile", state, {}, repaired); });
            selected_input.translation.archive_dsl.clear();
            const auto result = session.compile_source_action("model-candidate", task.revision, candidate_context.context,
                "compile", state, {}, selected_input);
            check(result.outcome.succeeded && result.certificates.empty() && result.native_executions == 0);
            check(!std::get<bool>(result.outcome.observed.at("source_translation_qualified")));
            check(std::get<std::string>(result.outcome.observed.at("source_model_candidate")) == hex(*selected_input.candidate));
            const auto restored = session.restore_native_execution("model-candidate", tasks.active("model-candidate")->revision,
                candidate_context.context, knowledge, {});
            check(restored.steps.at("compile").receipt == result.receipt);
            check(model.metrics().file_bytes == 0 && !journal.assessment(source_id));
        }

        memory.open_session("forged", "test");
        const auto forged_message = memory.append("forged", Role::User, "1", "Compile the source.");
        const auto forged_context = capture("forged");
        {
            TaskSession session(memory, tasks, journal, model);
            const auto accepted = std::get<SessionTask>(session.accept_plan("forged", forged_message.id,
                forged_context.context, forged_context.context.snapshot(), plan(forged_context.context), 0));
            auto fake = *journal.get(receipt_id);
            std::unique_ptr<json_object, decltype(&json_object_put)> fields(
                json_tokener_parse(std::string(fake.payload.begin(), fake.payload.end()).c_str()), json_object_put);
            json_object_object_add(fields.get(), "manifest", json_object_new_string(hex(accepted.ordered_task).c_str()));
            json_object_object_add(fields.get(), "task_ir", json_object_new_string(plan(forged_context.context).c_str()));
            json_object_object_add(fields.get(), "source_translation_qualified", json_object_new_boolean(true));
            fake.links[0].target = accepted.ordered_task;
            const std::string encoded = json_object_to_json_string_ext(fields.get(), JSON_C_TO_STRING_PLAIN);
            fake.payload.assign(encoded.begin(), encoded.end());
            auto current = tasks.transition("forged", accepted.revision, TaskPhase::Reasoning);
            current = tasks.transition("forged", current.revision, TaskPhase::Unresolved, {},
                CompletedTaskAction{current.revision + 1, "compile", journal.put(fake)});
            rejects(ErrorCode::Integrity, [&] { (void)session.restore_native_execution("forged", current.revision,
                forged_context.context, knowledge, {}); });
        }
        std::cout << "Task source checks passed: " << assertions << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
