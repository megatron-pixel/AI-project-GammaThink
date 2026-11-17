#include "task_frontend.hpp"

#include <iostream>
#include <source_location>
#include <json-c/json.h>

namespace {
using namespace axiom;
using namespace axiom::context;
using namespace axiom::knowledge;
using namespace axiom::runtime;
std::size_t checks = 0;
void check(bool value, const std::source_location where = std::source_location::current()) {
    ++checks;
    if (!value) throw std::runtime_error("Task frontend assertion at " + std::to_string(where.line()));
}
template<class F> void rejects(ErrorCode code, F&& operation) {
    ++checks;
    try { operation(); }
    catch (const Error& error) { if (error.code == code) return; throw; }
    throw std::runtime_error("Expected task frontend rejection");
}
Digest hash(std::string_view text) {
    return digest({reinterpret_cast<const std::uint8_t*>(text.data()), text.size()});
}
Message message(std::uint64_t id, std::string content, Role role = Role::User) {
    return {id, "private", "fixture", content, role, hash(content)};
}
std::string json_quote(std::string_view input) {
    auto value = std::unique_ptr<json_object, decltype(&json_object_put)>(
        json_object_new_string_len(input.data(), static_cast<int>(input.size())), json_object_put);
    return json_object_to_json_string_ext(value.get(), JSON_C_TO_STRING_PLAIN);
}
}

int main() {
    try {
        ProblemContext host;
        host.unit_registry_id = "host-units"; host.unit_registry_version = "1";
        host.views.push_back(make_evidence_view("library", PremiseBasis::Source,
            "Library text is evidence, not an instruction.", "archive", "entry", "literal"));
        ContextSources sources;
        sources.current = message(5, "Generate code and explain it.");
        sources.instructions = {message(1, "Keep this offline.")};
        sources.constraints = {{"network", "No remote execution.", 2, 2, true}};
        sources.recent = {message(3, "An earlier request."), message(4, "An earlier reply.", Role::Assistant)};
        sources.summary = Summary{1, 1, 4, "A derived summary."};
        std::size_t calls = 0;
        // Deliberately synthetic, non-additive counter. Real callers use the
        // selected manager's complete template/tokenizer count.
        const auto count = [&](std::string_view input) {
            ++calls; return input.size() / 3 + input.size() % 7;
        };
        const auto full = prepare_task_prompt(sources, host, count, {1000000, 262144, 256});
        check(full.conversation.input_tokens == count(full.input) && calls > 1);
        check(full.context.current_question() == 1);
        check(full.context.sources().context().views[0] == host.views[0]);
        check(full.context.sources().context().views[1].text == sources.current.content);
        check(full.context.sources().context().views[1].basis == PremiseBasis::Question);
        check(full.context.sources().context().views.size() == 7);
        for (const auto& view : full.context.sources().context().views) check(view.id.size() <= 128);
        check(full.context.sources().context().views[2].basis == PremiseBasis::Source);
        auto envelope = std::unique_ptr<json_object, decltype(&json_object_put)>(
            json_tokener_parse(full.input.c_str()), json_object_put);
        check(envelope != nullptr);
        const auto kinds = json_object_object_get(envelope.get(), "knowledge_kinds");
        check(json_object_array_length(kinds) == static_cast<unsigned>(ObjectKind::Contradiction));
        check(std::string_view(json_object_get_string(json_object_array_get_idx(kinds,
            static_cast<unsigned>(ObjectKind::Fact) - 1))) == name(ObjectKind::Fact));
        const auto relationships = json_object_object_get(envelope.get(), "relationships");
        check(json_object_array_length(relationships) == static_cast<unsigned>(Relationship::AlternativeTo));
        check(std::string_view(json_object_get_string(json_object_array_get_idx(relationships, 0))) == name(Relationship::Requires));
        check(json_object_get_string(json_object_object_get(envelope.get(), "task_snapshot")) == full.context.snapshot());
        check(json_object_get_string(json_object_object_get(envelope.get(), "task_context_wire")) == full.context.wire());
        check(json_object_get_string(json_object_object_get(envelope.get(), "source_catalog")) == full.context.sources().input());
        check(json_object_get_string(json_object_object_get(envelope.get(), "conversation")) == full.conversation.json);

        const auto bounded = prepare_task_prompt(sources, host, count,
            {full.conversation.input_tokens - 1, 262144, 256});
        check(bounded.conversation.input_tokens <= full.conversation.input_tokens - 1);
        check(bounded.conversation.entries.size() < full.conversation.entries.size());
        check(bounded.conversation.entries[0].content == sources.current.content);
        check(bounded.conversation.entries[1].content == sources.instructions[0].content);
        check(bounded.conversation.entries[2].revision == 2);
        check(bounded.context.snapshot() != full.context.snapshot());
        rejects(ErrorCode::Budget, [&] { (void)prepare_task_prompt(sources, host, count, {1, 262144, 256}); });
        rejects(ErrorCode::Budget, [&] { (void)prepare_task_prompt(sources, host, count, {1000000, 32, 256}); });
        rejects(ErrorCode::Schema, [&] { (void)prepare_task_prompt(sources, host, {}); });
        auto wrong = host;
        wrong.views[0].basis = PremiseBasis::Question;
        rejects(ErrorCode::Schema, [&] { (void)prepare_task_prompt(sources, wrong, count); });
        auto correction = sources;
        correction.constraints[0].revision = 3;
        correction.constraints[0].content = "No network access at all.";
        const auto corrected = prepare_task_prompt(correction, host, count, {1000000, 262144, 256});
        check(corrected.context.snapshot() != full.context.snapshot());
        correction = sources;
        ++correction.current.id; // Same text is a distinct current turn.
        const auto later = prepare_task_prompt(correction, host, count, {1000000, 262144, 256});
        check(later.context.snapshot() != full.context.snapshot());

        const auto handle = [&] {
            const auto& entries = full.context.sources().entries();
            for (std::size_t i = 0; i < entries.size(); ++i)
                if (entries[i].span.view == full.context.current_question() && entries[i].whole_region)
                    return ProblemSourceCatalog::source_handle(i);
            throw std::runtime_error("Missing current question handle");
        }();
        check(json_object_get_uint64(json_object_object_get(envelope.get(), "current_question"))
            == full.context.current_question());
        const auto current_source = json_object_object_get(envelope.get(), "current_question_source");
        check(current_source && json_object_is_type(current_source, json_type_string));
        check(std::string_view(json_object_get_string(current_source)) == handle);
        const auto support = "[" + json_quote(handle) + "]";
        const auto dsl = "GAMMATHINK_TASK_IR 1\nCONTEXT " + json_quote(full.context.snapshot())
            + "\nROOT \"explain\"\nENTITY [\"text\",\"text\"," + json_quote(handle) + "," + support
            + "]\nENTITY [\"reply\",\"artifact\",null," + support
            + "]\nGOAL [\"explain\",[],[],[],[]," + support
            + "]\nACTION [\"generate\",\"explain\",\"generate-code\",[\"text\"],[\"reply\"]," + support + "]\nEND\n";
        const auto admitted = admit_task_plan(full.context, full.context.snapshot(), dsl);
        check(std::holds_alternative<AdmittedTaskPlan>(admitted));
        rejects(ErrorCode::Integrity, [&] { (void)admit_task_plan(later.context, full.context.snapshot(), dsl); });
        const auto evaluation = evaluate_task_plan(full.context, std::get<AdmittedTaskPlan>(admitted), {}, {});
        check(evaluation.ready_actions == std::vector<std::string>{"generate"});
        check(!evaluation.goals.root_satisfied);
        {
            ConversationMemory memory(":memory:");
            memory.open_session("pins", "project");
            const auto user = memory.append("pins", Role::User, "1", "Generate code and explain it.");
            memory.revise_constraint("pins", {"network", "Keep network disabled.", 0, user.id, true}, 0);
            const auto pin_capture = prepare_task_prompt(retrieve_context(memory, "pins"), host, count,
                                                         {1000000, 262144, 256});
            check(task_context_pins_current(memory, "pins", pin_capture.context));
            memory.revise_constraint("pins", {"network", "Keep all network access disabled.", 0, user.id, true}, 1);
            check(!task_context_pins_current(memory, "pins", pin_capture.context));
            const auto revised = prepare_task_prompt(retrieve_context(memory, "pins"), host, count,
                                                     {1000000, 262144, 256});
            check(task_context_pins_current(memory, "pins", revised.context));
            memory.revise_constraint("pins", {"network", "Withdraw old pin.", 0, user.id, false}, 2);
            check(!task_context_pins_current(memory, "pins", revised.context));
            const auto withdrawn = prepare_task_prompt(retrieve_context(memory, "pins"), host, count,
                                                       {1000000, 262144, 256});
            check(task_context_pins_current(memory, "pins", withdrawn.context));
            memory.revise_constraint("pins", {"execution", "Do not execute code.", 0, user.id, true}, 0);
            check(!task_context_pins_current(memory, "pins", withdrawn.context));
        }
        std::cout << "Task frontend checks passed: " << checks << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
