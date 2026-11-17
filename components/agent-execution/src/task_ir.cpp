#include "task_ir.hpp"
#include "problem_semantic.hpp"
#include "problem_wire.hpp"
#include "archive_dsl/codec.hpp"

#include <json-c/json.h>
#include <memory>

namespace axiom::knowledge {
namespace {
using Json = std::unique_ptr<json_object, decltype(&json_object_put)>;
void require(bool value, const char* message) {
    if (!value) throw Error(ErrorCode::Schema, message);
}
std::string text(json_object* value) {
    require(value && json_object_get_type(value) == json_type_string, "Expected source handle text");
    return {json_object_get_string(value), static_cast<std::size_t>(json_object_get_string_len(value))};
}
EvidenceSpan source_span(json_object* value, const ProblemSourceCatalog& catalog) {
    if (value && json_object_get_type(value) == json_type_string) {
        return catalog.resolve(catalog.snapshot(), text(value));
    }
    require(value && json_object_get_type(value) == json_type_array
        && json_object_array_length(value) == 2, "Invalid source handle range");
    const auto first = text(json_object_array_get_idx(value, 0));
    const auto last = json_object_array_get_idx(value, 1);
    if (!last || json_object_get_type(last) == json_type_null) return catalog.resolve(catalog.snapshot(), first);
    return catalog.resolve(catalog.snapshot(), first, text(last));
}
void map_spans(json_object* theory, const ProblemSourceCatalog& catalog) {
    const auto one = [&](json_object* record, std::size_t index, bool nullable) {
        require(record && json_object_get_type(record) == json_type_array
            && json_object_array_length(record) > index, "Missing source-bearing tuple field");
        const auto value = json_object_array_get_idx(record, index);
        if (nullable && (!value || json_object_get_type(value) == json_type_null)) return;
        const auto span = source_span(value, catalog);
        auto* replacement = json_object_new_array();
        json_object_array_add(replacement, json_object_new_uint64(span.view));
        json_object_array_add(replacement, json_object_new_uint64(span.offset));
        json_object_array_add(replacement, json_object_new_uint64(span.length));
        json_object_array_add(replacement, span.locator == no_step ? nullptr : json_object_new_uint64(span.locator));
        json_object_array_put_idx(record, index, replacement);
    };
    for (const auto section : {"numbers", "bindings", "equations", "rules"}) {
        const auto records = json_object_object_get(theory, section);
        for (std::size_t i = 0; i < json_object_array_length(records); ++i) {
            const auto record = json_object_array_get_idx(records, i);
            if (std::string_view(section) == "numbers") {
                one(record, 0, false); one(record, 1, true); one(record, 4, true);
            } else {
                const std::size_t index = std::string_view(section) == "bindings" ? 2
                    : std::string_view(section) == "equations" ? 4 : 3;
                require(record && json_object_get_type(record) == json_type_array
                    && json_object_array_length(record) > index, "Missing source support field");
                const auto supports = json_object_array_get_idx(record, index);
                require(supports && json_object_get_type(supports) == json_type_array
                    && json_object_array_length(supports) <= 16, "Invalid source supports");
                for (std::size_t j = 0; j < json_object_array_length(supports); ++j) one(supports, j, false);
            }
        }
    }
}
std::string dump(json_object* value) {
    return json_object_to_json_string_ext(value, JSON_C_TO_STRING_PLAIN);
}
Object contextual(ObjectKind kind, std::string format, Bytes payload, const ProblemContext& context) {
    Object object;
    object.kind = kind;
    object.format = std::move(format);
    object.payload = std::move(payload);
    for (const auto& view : context.views) {
        object.sources.push_back({view.archive_id, "", view.entry_path, view.extractor_version,
            digest(std::span(reinterpret_cast<const std::uint8_t*>(view.text.data()), view.text.size())),
            0, view.text.size()});
    }
    return object;
}
Bytes bytes(std::string_view value) { return {value.begin(), value.end()}; }
}

AdmittedTask::AdmittedTask(ProblemGoalsRequest request, std::string dsl, std::string snapshot)
    : request_(std::move(request)), dsl_(std::move(dsl)), snapshot_(std::move(snapshot)) {}

TaskInterpretation admit_task_dsl(const ProblemSourceCatalog& catalog,
    std::string_view input_snapshot, std::string_view archive_dsl) {
    require(input_snapshot == catalog.snapshot(), "Task interpretation context snapshot changed");
    std::string decoded;
    try {
        decoded = gammathink::archive_dsl::decode(archive_dsl);
    } catch (const gammathink::archive_dsl::Error& error) {
        throw Error(ErrorCode::Schema, error.what());
    }
    const auto parser = std::unique_ptr<json_tokener, decltype(&json_tokener_free)>(
        json_tokener_new_ex(gammathink::archive_dsl::max_depth + 2), json_tokener_free);
    if (!parser) throw Error(ErrorCode::Budget, "Cannot allocate task profile parser");
    Json root(json_tokener_parse_ex(parser.get(), decoded.data(), static_cast<int>(decoded.size())), json_object_put);
    require(root && json_tokener_get_error(parser.get()) == json_tokener_success, "Invalid decoded Task IR");
    if (const auto kind = json_object_object_get(root.get(), "kind")) {
        return TaskDecline{text(kind) == "clarify" ? TaskDeclineKind::Clarify : TaskDeclineKind::Unsupported,
            text(json_object_object_get(root.get(), "reason"))};
    }
    const auto theory = json_object_object_get(root.get(), "theory");
    const auto goals = json_object_object_get(root.get(), "goals");
    const auto count = json_object_array_length(goals);
    require(count > 0 && count <= max_problem_goals, "Task requires one to eight ordered goals");
    require(catalog.limits().work >= count && catalog.limits().steps >= count, "Task goal budget is insufficient");
    map_spans(theory, catalog);
    ProblemGoalsRequest request;
    for (std::size_t i = 0; i < count; ++i) {
        const auto goal = json_object_array_get_idx(goals, i);
        require(goal && json_object_get_type(goal) == json_type_object
            && json_object_object_length(goal) == 2, "Invalid ordered task goal");
        const auto value = json_object_object_get(goal, "goal");
        const auto supports = json_object_object_get(goal, "supports");
        require(value && supports && json_object_get_type(supports) == json_type_array
            && json_object_array_length(supports) > 0 && json_object_array_length(supports) <= 16,
            "Task goal needs explicit question supports");
        json_object_object_add(theory, "goal", json_object_get(value));
        auto limits = catalog.limits();
        limits.work = limits.work / count + (i < limits.work % count);
        limits.steps = static_cast<std::uint32_t>(limits.steps / count + (i < limits.steps % count));
        const auto parsed = parse_problem_proposal(dump(theory), catalog.context(), limits);
        if (i == 0) {
            request.first = parsed;
            request.first.limits = catalog.limits();
        }
        SupportedProblemGoal slot{parsed.proposal.goal, {}};
        for (std::size_t j = 0; j < json_object_array_length(supports); ++j) {
            slot.supports.push_back(source_span(json_object_array_get_idx(supports, j), catalog));
        }
        request.goals.push_back(std::move(slot));
    }
    (void)admit_problem_goals(request);
    return AdmittedTask(std::move(request), std::string(archive_dsl), catalog.snapshot());
}

namespace {
std::pair<PersistedTask, std::vector<Object>> task_records(const AdmittedTask& task) {
    const auto& request = task.request();
    const auto slots = admit_problem_goals(request);
    const auto& context = request.first.context;
    const auto host = contextual(ObjectKind::Assumption, "axiom.problem-context.offset-v1.json",
        bytes(serialize_problem_context(context)), context);
    auto interpretation = contextual(ObjectKind::Action, "gammathink.task-ir.archive-dsl.v1",
        bytes(task.dsl()), context);
    interpretation.links.push_back({Relationship::DependsOn, identify(host)});
    auto ordered = contextual(ObjectKind::Goal, "application/x-axiom-ordered-goals",
        encode_problem_goals_request(request), context);
    ordered.links.push_back({Relationship::DependsOn, identify(interpretation)});
    std::vector<Object> goals;
    std::vector<GoalIR> definitions(1);
    definitions[0].id = "task";
    definitions[0].methods.emplace_back();
    PersistedTask result;
    for (std::size_t i = 0; i < slots.size(); ++i) {
        auto goal = contextual(ObjectKind::Goal, std::string(native_request_format),
            encode_problem_request(slots[i]), context);
        goal.links.push_back({Relationship::DependsOn, identify(ordered)});
        const auto id = identify(goal);
        result.requests.push_back(id);
        GoalIR definition;
        definition.id = "goal-" + std::to_string(i);
        definition.formal_request = id;
        definitions[0].methods.front().push_back(definition.id);
        definitions.push_back(std::move(definition));
        goals.push_back(std::move(goal));
    }
    result.plan = decompose_goals("task", definitions);
    // Validate and budget the entire set before beginning journal writes.
    std::size_t total = encode_object(host).size() + encode_object(interpretation).size()
        + encode_object(ordered).size();
    for (const auto& goal : goals) total += encode_object(goal).size();
    if (total > 32 * 1024 * 1024) throw Error(ErrorCode::Budget, "Persisted Task IR exceeds byte budget");
    result.context = identify(host);
    result.interpretation = identify(interpretation);
    result.ordered_request = identify(ordered);
    std::vector<Object> records{host, interpretation, ordered};
    for (auto& goal : goals) records.push_back(std::move(goal));
    return {std::move(result), std::move(records)};
}
}

PersistedTask persist_task(Journal& journal, const AdmittedTask& task) {
    auto [result, records] = task_records(task);
    for (const auto& object : records) journal.put(object);
    return result;
}

AdmittedTask restore_task(Journal& journal, const ObjectId& ordered_request) {
    const auto ordered = journal.get(ordered_request);
    if (!ordered) throw Error(ErrorCode::Unavailable, "Stored ordered task is unavailable");
    require(ordered->kind == ObjectKind::Goal && ordered->format == "application/x-axiom-ordered-goals" &&
        ordered->links.size() == 1 && ordered->links[0].relationship == Relationship::DependsOn,
        "Not a persisted ordered task");
    const auto interpretation_id = ordered->links[0].target;
    const auto interpretation = journal.get(interpretation_id);
    if (!interpretation) throw Error(ErrorCode::Unavailable, "Stored task interpretation is unavailable");
    require(interpretation->kind == ObjectKind::Action &&
        interpretation->format == "gammathink.task-ir.archive-dsl.v1" &&
        interpretation->links.size() == 1 && interpretation->links[0].relationship == Relationship::DependsOn,
        "Not a persisted task interpretation");
    const auto context_id = interpretation->links[0].target;
    const auto context = journal.get(context_id);
    if (!context) throw Error(ErrorCode::Unavailable, "Stored task context is unavailable");
    require(context->kind == ObjectKind::Assumption &&
        context->format == "axiom.problem-context.offset-v1.json", "Not a persisted task context");
    const auto request = decode_problem_goals_request(ordered->payload);
    const ProblemSourceCatalog catalog(
        parse_problem_context(std::string(context->payload.begin(), context->payload.end())),
        request.first.limits);
    auto admitted = admit_task_dsl(catalog, catalog.snapshot(),
        std::string(interpretation->payload.begin(), interpretation->payload.end()));
    require(std::holds_alternative<AdmittedTask>(admitted), "Stored executable task declined on re-admission");
    auto task = std::get<AdmittedTask>(std::move(admitted));
    const auto expected = task_records(task).first;
    if (expected.ordered_request != ordered_request || expected.interpretation != interpretation_id ||
        expected.context != context_id) {
        throw Error(ErrorCode::Integrity, "Stored Task IR differs from re-admitted source-bound interpretation");
    }
    return task;
}

TaskRun execute_task(Journal& journal, const AdmittedTask& task,
    NativeRunLimits limits, NativeCoKLimits materialization) {
    TaskRun result;
    result.task = persist_task(journal, task);
    std::map<std::string, GoalOutcome> outcomes;
    bool all_checked = true;
    for (std::size_t i = 0; i < result.task.requests.size(); ++i) {
        TaskSlotRun slot;
        slot.ordinal = i;
        slot.execution = run_native(journal, result.task.requests[i], limits);
        if (slot.execution.certificate) {
            const auto goal = journal.get(result.task.requests[i]);
            const auto certificate = journal.get(*slot.execution.certificate);
            if (!goal || !certificate) throw Error(ErrorCode::Unavailable, "Task certificate became unavailable");
            const auto graph = materialize_native(*goal, *certificate, materialization);
            std::size_t used_bytes = 0;
            for (const auto& object : graph.objects) used_bytes += encode_object(object).size();
            materialization.objects -= graph.objects.size();
            materialization.bytes -= used_bytes;
            for (const auto& object : graph.objects) journal.put(object);
            slot.conclusions = graph.conclusions;
            slot.derivations = graph.derivations;
        }
        const bool complete = slot.execution.check.valid && slot.execution.check.conclusive
            && !slot.execution.check.conflict;
        outcomes["goal-" + std::to_string(i)] = {
            slot.execution.check.valid && slot.execution.check.conclusive,
            complete, {}, slot.execution.certificate};
        all_checked = all_checked && complete;
        result.slots.push_back(std::move(slot));
    }
    // Satisfied means all requested queries have checked, non-conflicting
    // outcomes. It never asserts empirical truth or that a signed goal is true.
    if (all_checked) outcomes["task"] = {true, true, {}, {}};
    result.evaluation = evaluate_goals(result.task.plan, outcomes, {});
    return result;
}

} // namespace axiom::knowledge
