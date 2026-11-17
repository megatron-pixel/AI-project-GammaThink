#include "task_session.hpp"

#include <json-c/json.h>
#include <cstdlib>
#include <iostream>
#include <source_location>
#include <unistd.h>

namespace {
using namespace axiom;
using namespace axiom::context;
using namespace axiom::knowledge;
using namespace axiom::runtime;
using Json = std::unique_ptr<json_object, decltype(&json_object_put)>;
std::size_t checks = 0;
void check(bool value, const std::source_location where = std::source_location::current()) {
    ++checks;
    if (!value) throw std::runtime_error("Quantified source profile assertion at " + std::to_string(where.line()));
}
template<class F> void rejects(ErrorCode code, F operation) {
    ++checks;
    try { operation(); }
    catch (const Error& error) { if (error.code == code) return; throw; }
    throw std::runtime_error("Expected source profile rejection");
}
std::string quote(std::string_view text) {
    Json value(json_object_new_string_len(text.data(), static_cast<int>(text.size())), json_object_put);
    if (!value) throw std::runtime_error("Cannot quote synthetic source");
    return json_object_to_json_string_ext(value.get(), JSON_C_TO_STRING_PLAIN);
}
Json document(const std::string& text) {
    Json value(json_tokener_parse(text.c_str()), json_object_put);
    if (!value) throw std::runtime_error("Invalid synthetic source metadata");
    return value;
}
struct Directory {
    std::filesystem::path path;
    Directory() {
        auto pattern = (std::filesystem::temp_directory_path() / "source-profile-XXXXXX").string();
        const auto* created = mkdtemp(pattern.data());
        if (!created) throw std::runtime_error("Cannot create source profile directory");
        path = created;
    }
    ~Directory() { std::filesystem::remove_all(path); }
};
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
// Data-only framing fixture; no actual model, inference or meaning certification.
ObjectId candidate(Journal& journal, const Object& source, const SessionTask& task,
    const AdmittedTaskPlan& admitted, const TaskSourceCompilation& input, bool wrong_policy = false) {
    const auto artifact = [&](std::string_view format, const std::string& payload) {
        Object result;
        result.kind = ObjectKind::Evidence;
        result.label = format;
        result.format = format;
        result.payload.assign(payload.begin(), payload.end());
        result.sources = source.sources;
        result.links = {{Relationship::DependsOn, task.ordered_task},
            {Relationship::DependsOn, input.translation.source}};
        return result;
    };
    const auto prompt = journal.put(artifact("gammathink.source-translation-prompt.v2", source_translation_input(input)));
    const auto model = journal.put(artifact("gammathink.source-translation-model.v2",
        "{\"id\":\"synthetic-not-a-model\",\"path\":" + quote("/synthetic/not-a-model.gguf") + ",\"sha256\":"
        + quote(hex(ObjectId{Digest{1}})) + ",\"system_policy\":"
        + quote(wrong_policy ? source_translation_policy : source_translation_policy_v2)
        + ",\"role\":3,\"enable_thinking\":false,\"file_bytes\":1,\"retained_parameters\":1,"
        "\"runtime_parameters\":1,\"sampled_peak_rss\":1,\"unloaded_rss\":1,"
        "\"context_tokens\":8192,\"output_tokens\":512}"));
    const auto& action = admitted.actions().front();
    auto result = artifact("gammathink.source-translation-candidate.v2",
        "{\"manifest\":" + quote(hex(task.ordered_task)) + ",\"action\":" + quote(action.id)
        + ",\"goal\":" + quote(action.goal) + ",\"task_snapshot\":" + quote(admitted.snapshot())
        + ",\"source\":" + quote(hex(input.translation.source)) + ",\"source_context\":"
        + quote(serialize_problem_context(input.context)) + ",\"context_snapshot\":"
        + quote(input.translation.context_snapshot) + ",\"prompt\":" + quote(hex(prompt))
        + ",\"model\":" + quote(hex(model)) + ",\"archive_dsl\":" + quote(input.translation.archive_dsl)
        + ",\"selectors\":" + std::to_string(static_cast<int>(input.selectors))
        + ",\"input_tokens\":100,\"output_tokens\":12,\"stop\":0,"
        "\"source_translation_qualified\":false,\"empirical_truth_verified\":false}");
    result.links.push_back({Relationship::DependsOn, prompt});
    result.links.push_back({Relationship::DependsOn, model});
    return journal.put(result);
}
}

int main() {
    try {
        Directory temporary;
        for (const auto selectors : {ProblemSelectorMode::OffsetV1, ProblemSelectorMode::QuoteV1}) {
            const auto directory = temporary.path / (selectors == ProblemSelectorMode::OffsetV1 ? "offset" : "quote");
            std::filesystem::create_directory(directory);
            const std::string text =
                "For every eligible object, output equals 2 times input. A is eligible. A input is 3. B input is 7.";
            Object source;
            source.kind = ObjectKind::Evidence;
            source.label = "Synthetic universal source";
            source.format = "source.zim.raw.v1:text/plain";
            source.payload.assign(text.begin(), text.end());
            source.sources = {{"synthetic", "1", "law", "libzim-raw-entry-v1",
                digest(source.payload), 0, text.size()}};
            ProblemContext host;
            host.unit_registry_id = "synthetic";
            host.unit_registry_version = "1";
            host.units = {{"one", "1", {}, 1, {"1"}}};
            TaskSourceCompilation input;
            input.context = host;
            input.context.views = {make_evidence_view("source", PremiseBasis::Source, text,
                "synthetic", "law", "libzim-raw-entry-v1")};
            input.profile = SourceTranslationProfile::QuantifiedV2;
            input.selectors = selectors;
            const auto span = [&](std::string_view selected) {
                if (selectors == ProblemSelectorMode::QuoteV1)
                    return "[0,null," + quote(selected) + ",null]";
                const auto offset = text.find(selected);
                if (offset == std::string::npos) throw std::runtime_error("Missing synthetic source selector");
                return "[0," + std::to_string(offset) + "," + std::to_string(selected.size()) + ",null]";
            };
            const auto support = "[" + span(text) + "]";
            std::string dsl = "GAMMATHINK_DSL 2\nENTITY \"A\"\nENTITY \"B\"\n"
                "QUANTITY [\"input\",0,[true,[]],0]\nQUANTITY [\"output\",0,[true,[]],0]\n"
                "QUANTITY [\"input\",1,[true,[]],0]\nQUANTITY [\"output\",1,[true,[]],0]\n";
            for (const auto numeral : {"2", "3", "7"})
                dsl += "NUMBER [" + span(numeral) + ",null,0,\"exact\",null]\n";
            dsl += "BINDING [0,1," + support + "]\nBINDING [2,2," + support + "]\n"
                "PREDICATE [\"eligible\",1]\nRULE [0,[0,false,[[\"c\",0]]],[]," + support + "]\n"
                "QUANTIFIED_EQUATION [1,[[\"input\",[\"v\",0],[true,[]],0],"
                "[\"output\",[\"v\",0],[true,[]],0]],"
                "[[\"q\",1],[\"q\",0],[\"n\",0],[\"mul\",1,2]],0,3,[true,[]],"
                "[0,false,[[\"v\",0]]]," + support + "]\nEND\n";
            ConversationMemory memory(directory / "conversation.db");
            SessionTasks tasks(directory / "tasks.db");
            Journal journal(directory / "knowledge.db");
            const auto source_id = journal.put(source);
            input.translation = {source_id, ProblemSourceCatalog(input.context).snapshot(), dsl};
            const auto prompt = document(source_translation_input(input));
            check(std::string(json_object_get_string(json_object_object_get(prompt.get(), "profile")))
                == "gammathink.source-translation-input.v2");
            check(std::string(json_object_get_string(json_object_object_get(prompt.get(), "source_context")))
                == serialize_problem_context(input.context));
            auto legacy = input;
            legacy.profile = SourceTranslationProfile::V1;
            const auto legacy_prompt = document(source_translation_input(legacy));
            check(std::string(json_object_get_string(json_object_object_get(legacy_prompt.get(), "profile")))
                == "gammathink.source-translation-input.v1");
            auto unknown = input;
            unknown.profile = static_cast<SourceTranslationProfile>(3);
            rejects(ErrorCode::Schema, [&] { (void)source_translation_input(unknown); });
            memory.open_session("source", "test");
            const auto message = memory.append("source", Role::User, "1", "Compile the complete source and report.");
            const auto prepared = prepare_task_prompt(retrieve_context(memory, "source"), host,
                [](std::string_view value) { return value.size(); }, {1000000, 262144, 256},
                {{"document", {source_id, ObjectKind::Evidence, source.format}}});
            neural::NativeModelManager model;
            JournalArchive local(journal);
            UnifiedKnowledge knowledge({&local});
            ObjectId receipt_id, module_id;
            {
                TaskSession session(memory, tasks, journal, model);
                const auto task = std::get<SessionTask>(session.accept_plan("source", message.id, prepared.context,
                    prepared.context.snapshot(), plan(prepared.context), 0));
                const auto admitted = session.active_plan("source", task.revision, prepared.context);
                TaskExecutionState state;
                auto selected = input;
                selected.candidate = candidate(journal, source, task, admitted, input);
                auto wrong = selected;
                wrong.profile = SourceTranslationProfile::V1;
                rejects(ErrorCode::Integrity, [&] {
                    (void)session.compile_source_action("source", task.revision, prepared.context, "compile", state, {}, wrong);
                });
                wrong = input;
                wrong.translation.archive_dsl = "GAMMATHINK_DSL 1\nENTITY \"A\"\nEND\n";
                wrong.candidate = candidate(journal, source, task, admitted, wrong);
                rejects(ErrorCode::Schema, [&] {
                    (void)session.compile_source_action("source", task.revision, prepared.context, "compile", state, {}, wrong);
                });
                wrong = input;
                wrong.candidate = candidate(journal, source, task, admitted, input, true);
                rejects(ErrorCode::Integrity, [&] {
                    (void)session.compile_source_action("source", task.revision, prepared.context, "compile", state, {}, wrong);
                });
                check(tasks.active("source")->revision == task.revision && state.steps.empty());
                selected.translation.archive_dsl.clear();
                const auto compiled = session.compile_source_action("source", task.revision,
                    prepared.context, "compile", state, {}, selected);
                receipt_id = compiled.receipt;
                module_id = compiled.artifact;
                check(tasks.active("source")->phase == TaskPhase::Unresolved);
                check(compiled.outcome.succeeded && compiled.native_executions == 0 && compiled.certificates.empty());
                const auto receipt = journal.get(receipt_id);
                const auto module = journal.get(module_id);
                check(receipt && receipt->format == "gammathink.task-source-compilation.v2");
                check(module && module->format == "gammathink.archive-dsl.v2");
                check(!std::get<bool>(compiled.outcome.observed.at("source_translation_qualified")));
                check(!std::get<bool>(compiled.outcome.observed.at("empirical_truth_verified")));
                const auto context_id = parse_id(std::get<std::string>(compiled.outcome.observed.at("source_context")));
                const auto stored_context = journal.get(context_id);
                check(stored_context.has_value());
                const auto request = source_query_request(source, *module, *stored_context, input.context,
                    ProblemGoal{ProblemQuantityGoal{1, 0}});
                const auto answer = execute_problem(request);
                check(answer.answer == Rational(6) && verify_problem_result(request, answer));
            }
            TaskSession reopened(memory, tasks, journal, model);
            auto restored = reopened.restore_native_execution("source", tasks.active("source")->revision,
                prepared.context, knowledge, {});
            check(restored.steps.at("compile").receipt == receipt_id);
            check(std::get<bool>(restored.steps.at("compile").outcome.observed.at("source_record_restored")));
            check(model.state() == neural::ModelState::Unloaded && model.metrics().file_bytes == 0);
            check(!journal.assessment(source_id) && !journal.assessment(module_id));
            const auto report = reopened.execute_native_action("source", tasks.active("source")->revision,
                prepared.context, "describe", restored, knowledge, {});
            check(report.response.has_value() && report.certificates.empty());
            check(tasks.active("source")->phase == TaskPhase::Checked);
        }
        std::cout << "Quantified source profile checks passed: " << checks << "\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << "\n";
        return 1;
    }
}
