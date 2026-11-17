#include "task_plan.hpp"
#include "problem_semantic.hpp"

#include <json-c/json.h>

#include <charconv>
#include <fstream>
#include <iostream>

namespace {
using namespace axiom;
using namespace axiom::knowledge;
std::string input(const char* path, std::size_t maximum) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw Error(ErrorCode::Missing, "Cannot open Task IR input");
    std::string value(maximum + 1, '\0');
    file.read(value.data(), static_cast<std::streamsize>(value.size()));
    const auto size = file.gcount();
    if (file.bad()) throw Error(ErrorCode::Storage, "Cannot read complete Task IR input");
    if (size < 0 || static_cast<std::size_t>(size) > maximum)
        throw Error(ErrorCode::Budget, "Task IR input exceeds byte budget");
    value.resize(static_cast<std::size_t>(size)); return value;
}
std::size_t index(std::string_view value) {
    std::size_t result = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
    if (error != std::errc{} || end != value.data() + value.size() || value.empty())
        throw Error(ErrorCode::Schema, "Expected current-question index");
    return result;
}
void text(json_object* output, const char* key, std::string_view value) {
    json_object_object_add(output, key, json_object_new_string_len(value.data(), static_cast<int>(value.size())));
}
void list(json_object* output, const char* key, const std::vector<std::string>& values) {
    auto* result = json_object_new_array();
    for (const auto& value : values) json_object_array_add(result, json_object_new_string(value.c_str()));
    json_object_object_add(output, key, result);
}
void summary(json_object* output, const TaskPlanContext& context, const AdmittedTaskPlan& plan,
             const PersistedTaskPlan& persisted, const GoalConditions& environment) {
    const auto state = evaluate_task_plan(context, plan, {}, environment);
    text(output, "profile", "gammathink.task-ir.v1"); text(output, "snapshot", plan.snapshot());
    text(output, "manifest", hex(persisted.manifest));
    text(output, "root", plan.graph().nodes.at(plan.graph().root).id);
    json_object_object_add(output, "admitted", json_object_new_boolean(true));
    json_object_object_add(output, "entities", json_object_new_uint64(plan.entities().size()));
    json_object_object_add(output, "actions", json_object_new_uint64(plan.actions().size()));
    json_object_object_add(output, "goals", json_object_new_uint64(plan.graph().nodes.size()));
    json_object_object_add(output, "root_satisfied", json_object_new_boolean(state.goals.root_satisfied));
    list(output, "ready_actions", state.ready_actions);
    list(output, "missing_artifacts", state.missing_artifacts);
}
}
int main(int argc, char** argv) {
    using namespace axiom;
    using namespace axiom::knowledge;
    try {
        const auto command = argc > 1 ? std::string_view(argv[1]) : std::string_view{};
        if (command == "context" && argc == 4) {
            TaskPlanContext context(ProblemSourceCatalog(parse_problem_context(input(argv[2], 4 * 1024 * 1024))), index(argv[3]));
            std::cout << context.wire() << '\n'; return 0;
        }
        auto output = std::unique_ptr<json_object, decltype(&json_object_put)>(json_object_new_object(), json_object_put);
        if (command == "inspect" && (argc == 5 || argc == 6)) {
            const auto context = restore_task_plan_context(input(argv[2], 4 * 1024 * 1024));
            const auto proposed = admit_task_plan(context, context.snapshot(), input(argv[3], context.limits().wire_bytes));
            if (const auto* declined = std::get_if<TaskPlanDecline>(&proposed)) {
                json_object_object_add(output.get(), "admitted", json_object_new_boolean(false));
                text(output.get(), "kind", declined->kind == TaskDeclineKind::Clarify ? "clarify" : "unsupported");
                text(output.get(), "reason", declined->reason); text(output.get(), "snapshot", declined->snapshot);
            } else {
                const auto environment = argc == 6 ? parse_task_environment(input(argv[5], 1024 * 1024)) : GoalConditions{};
                const auto& plan = std::get<AdmittedTaskPlan>(proposed);
                Journal journal(argv[4]);
                summary(output.get(), context, plan, persist_task_plan(journal, plan), environment);
            }
        } else if (command == "restore" && (argc == 4 || argc == 5)) {
            if (!std::filesystem::is_regular_file(argv[2])) throw Error(ErrorCode::Missing, "Task IR journal is absent");
            Journal journal(argv[2]);
            const auto manifest = parse_id(argv[3]);
            const auto plan = restore_task_plan(journal, manifest);
            const auto context = restore_task_plan_context(plan.context_wire());
            const auto environment = argc == 5 ? parse_task_environment(input(argv[4], 1024 * 1024)) : GoalConditions{};
            // Inspection of the recorded capture, never activation against a
            // changed live policy or a grant to execute its ready actions.
            PersistedTaskPlan persisted; persisted.manifest = manifest;
            summary(output.get(), context, plan, persisted, environment);
            json_object_object_add(output.get(), "recorded_context_only", json_object_new_boolean(true));
        } else throw Error(ErrorCode::Schema,
            "Usage: knowledge-task-plan context PROBLEM_CONTEXT_JSON CURRENT_QUESTION_INDEX | "
            "inspect TASK_CONTEXT_WIRE TASK_IR JOURNAL_DB [ENVIRONMENT_JSON] | "
            "restore JOURNAL_DB MANIFEST_ID [ENVIRONMENT_JSON]");
        json_object_object_add(output.get(), "neural_calls", json_object_new_int(0));
        json_object_object_add(output.get(), "native_executions", json_object_new_int(0));
        json_object_object_add(output.get(), "action_executions", json_object_new_int(0));
        json_object_object_add(output.get(), "published", json_object_new_boolean(false));
        std::cout << json_object_to_json_string_ext(output.get(), JSON_C_TO_STRING_PLAIN) << '\n';
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
