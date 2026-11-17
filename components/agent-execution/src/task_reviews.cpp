#include "task_session.hpp"

#include <algorithm>
#include <json-c/json.h>

namespace axiom::runtime {
namespace {
using namespace knowledge;
using Json = std::unique_ptr<json_object, decltype(&json_object_put)>;
void require(bool condition, ErrorCode code, const char* message) {
    if (!condition) throw Error(code, message);
}
void text(json_object* root, const char* key, std::string_view value) {
    json_object_object_add(root, key, json_object_new_string_len(value.data(), static_cast<int>(value.size())));
}
const TaskAction& review_action(const AdmittedTaskPlan& plan, std::string_view id) {
    const auto found = std::find_if(plan.actions().begin(), plan.actions().end(),
        [&](const auto& value) { return value.id == id; });
    require(found != plan.actions().end() && (found->kind == TaskActionKind::Visualize ||
        found->kind == TaskActionKind::RequestPublication), ErrorCode::Schema, "Expected a private review action");
    return *found;
}
void ready(const TaskPlanContext& context, const AdmittedTaskPlan& plan, const TaskAction& action,
    const TaskExecutionState& state, const GoalConditions& environment, const std::set<ObjectId>& accepted) {
    const auto evaluated = evaluate_task_plan(context, plan, state.outcomes, environment, state.artifacts, accepted);
    require(std::find(evaluated.ready_actions.begin(), evaluated.ready_actions.end(), action.id) !=
        evaluated.ready_actions.end(), ErrorCode::Permission, "Review prerequisites or policy are not ready");
}
std::vector<ObjectId> focuses(const AdmittedTaskPlan& plan, const TaskAction& action, const TaskExecutionState& state) {
    std::vector<ObjectId> result;
    for (const auto& input : action.inputs) {
        const auto entity = std::find_if(plan.entities().begin(), plan.entities().end(),
            [&](const auto& value) { return value.id == input; });
        require(entity != plan.entities().end(), ErrorCode::Integrity, "Review lost its input entity");
        if (const auto* object = std::get_if<TaskObject>(&entity->value)) result.push_back(object->id);
        else {
            require(std::holds_alternative<TaskArtifact>(entity->value) && state.artifacts.contains(entity->id),
                ErrorCode::Integrity, "Review has no produced artifact");
            result.push_back(state.artifacts.at(entity->id));
        }
    }
    return result;
}
Json parse(std::string_view payload) {
    require(!payload.empty() && payload.size() <= 16 * 1024 * 1024, ErrorCode::Budget, "Review exceeds byte budget");
    std::unique_ptr<json_tokener, decltype(&json_tokener_free)> parser(json_tokener_new_ex(64), json_tokener_free);
    require(bool(parser), ErrorCode::Storage, "Cannot allocate review parser");
    json_tokener_set_flags(parser.get(), JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
    Json value(json_tokener_parse_ex(parser.get(), payload.data(), static_cast<int>(payload.size())), json_object_put);
    require(value && json_tokener_get_error(parser.get()) == json_tokener_success &&
        json_tokener_get_parse_end(parser.get()) == payload.size() && json_object_is_type(value.get(), json_type_object),
        ErrorCode::Integrity, "Invalid private review document");
    return value;
}
std::string string(json_object* root, const char* key) {
    require(root && json_object_is_type(root, json_type_object), ErrorCode::Integrity, "Expected a review object");
    auto* value = json_object_object_get(root, key);
    require(value && json_object_is_type(value, json_type_string), ErrorCode::Integrity, "Missing review text");
    return {json_object_get_string(value), static_cast<std::size_t>(json_object_get_string_len(value))};
}
bool boolean(json_object* root, const char* key) {
    require(root && json_object_is_type(root, json_type_object), ErrorCode::Integrity, "Expected a review object");
    auto* value = json_object_object_get(root, key);
    require(value && json_object_is_type(value, json_type_boolean), ErrorCode::Integrity, "Missing review boolean");
    return json_object_get_boolean(value);
}
Object record(Journal& journal, const ObjectId& id, ObjectKind kind, std::string_view format) {
    const auto status = journal.assessment(id);
    require(!status || (status->state != KnowledgeState::Invalidated && status->logical != LogicalValidity::Invalid),
        ErrorCode::Integrity, "Review artifact requires explicit revalidation");
    const auto result = journal.get(id);
    require(result && result->visibility == Visibility::Private && result->kind == kind && result->format == format,
        ErrorCode::Integrity, "Review lost its exact private artifact");
    return *result;
}
Bytes bytes(std::string_view encoded) {
    require(encoded.size() <= 16 * 1024 * 1024 && encoded.size() % 2 == 0,
        ErrorCode::Budget, "Inspection canonical bytes exceed budget");
    const auto nibble = [](char value) -> std::uint8_t {
        if (value >= '0' && value <= '9') return static_cast<std::uint8_t>(value - '0');
        if (value >= 'a' && value <= 'f') return static_cast<std::uint8_t>(value - 'a' + 10);
        throw Error(ErrorCode::Integrity, "Invalid inspection canonical byte");
    };
    Bytes result; result.reserve(encoded.size() / 2);
    for (std::size_t i = 0; i < encoded.size(); i += 2)
        result.push_back(static_cast<std::uint8_t>((nibble(encoded[i]) << 4) | nibble(encoded[i + 1])));
    return result;
}
void identities(json_object* root, const char* key, const std::vector<ObjectId>& expected) {
    auto* values = json_object_object_get(root, key);
    require(values && json_object_is_type(values, json_type_array) &&
        json_object_array_length(values) == expected.size(), ErrorCode::Integrity, "Inspection changed ordered references");
    for (std::size_t i = 0; i < expected.size(); ++i) {
        auto* value = json_object_array_get_idx(values, i);
        require(value && json_object_is_type(value, json_type_string) &&
            std::string_view(json_object_get_string(value)) == hex(expected[i]),
            ErrorCode::Integrity, "Inspection changed an ordered reference occurrence");
    }
}
Object artifact(const AdmittedTaskPlan& plan, const TaskAction& action, const ObjectId& manifest,
    const std::vector<ObjectId>& roots, std::string_view payload) {
    Object result; result.kind = ObjectKind::Evidence; result.label = action.id;
    result.format = action.kind == TaskActionKind::Visualize
        ? "gammathink.task-inspection-view.v1" : "gammathink.publication-request.v1";
    result.payload.assign(payload.begin(), payload.end());
    result.sources = plan.graph().nodes.at(plan.graph().root).definition->sources;
    result.links = {{Relationship::DependsOn, manifest}};
    for (const auto& root : roots) result.links.push_back({Relationship::DependsOn, root});
    return result;
}
void roots_field(json_object* value, const std::vector<ObjectId>& roots) {
    auto* values = json_object_new_array();
    json_object_object_add(value, "selected_roots", values);
    for (const auto& root : roots) json_object_array_add(values, json_object_new_string(hex(root).c_str()));
}
std::string request(const ObjectId& manifest, const TaskAction& action, const std::vector<ObjectId>& roots) {
    Json value(json_object_new_object(), json_object_put);
    require(bool(value), ErrorCode::Storage, "Cannot allocate private publication request");
    text(value.get(), "profile", "gammathink.publication-request.v1");
    text(value.get(), "manifest", hex(manifest)); text(value.get(), "action", action.id);
    roots_field(value.get(), roots); text(value.get(), "status", "pending-host-review");
    json_object_object_add(value.get(), "approved", json_object_new_boolean(false));
    json_object_object_add(value.get(), "published", json_object_new_boolean(false));
    return json_object_to_json_string_ext(value.get(), JSON_C_TO_STRING_PLAIN);
}
Object receipt(const AdmittedTaskPlan& plan, const TaskAction& action, const ObjectId& manifest,
    const std::vector<ObjectId>& roots, const ObjectId& view) {
    Json value(json_object_new_object(), json_object_put);
    require(bool(value), ErrorCode::Storage, "Cannot allocate private review receipt");
    text(value.get(), "manifest", hex(manifest)); text(value.get(), "action", action.id);
    text(value.get(), "goal", action.goal); roots_field(value.get(), roots); text(value.get(), "artifact", hex(view));
    json_object_object_add(value.get(), "kind", json_object_new_int(static_cast<int>(action.kind)));
    json_object_object_add(value.get(), "published", json_object_new_boolean(false));
    json_object_object_add(value.get(), "publication_approved", json_object_new_boolean(false));
    json_object_object_add(value.get(), "empirical_truth_verified", json_object_new_boolean(false));
    const std::string encoded = json_object_to_json_string_ext(value.get(), JSON_C_TO_STRING_PLAIN);
    auto result = artifact(plan, action, manifest, roots, encoded);
    result.kind = ObjectKind::TestResult; result.format = "gammathink.task-review-action.v1";
    result.links.push_back({Relationship::DependsOn, view});
    return result;
}
NativeTaskStep observation(UnifiedKnowledge& sources, const TaskAction& action, const std::vector<ObjectId>& roots,
    const ObjectId& view, const ObjectId& recorded, std::string_view payload, bool restored) {
    NativeTaskStep step;
    step.receipt = recorded; step.artifact = view; step.status = KnowledgeRunStatus::Checked;
    step.outcome.verified = true; step.outcome.succeeded = true; step.outcome.evidence = recorded;
    const auto& root = roots.front();
    step.outcome.observed = {{"review_artifact", hex(view)}, {"review_focus", hex(root)},
        {"review_selected_roots", static_cast<std::int64_t>(roots.size())},
        {"review_record_restored", restored}, {"native_checked", false}, {"published", false},
        {"publication_approved", false}, {"empirical_truth_verified", false}};
    bool available = true;
    for (const auto& selected : roots) {
        bool present = false;
        try {
            if (const auto found = sources.get(selected)) {
                present = true;
                if (found->record.assessment && (found->record.assessment->state == KnowledgeState::Invalidated ||
                    found->record.assessment->logical == LogicalValidity::Invalid)) step.revalidation_required.push_back(selected);
            }
        } catch (const Error& error) {
            if (error.code != ErrorCode::Unavailable) throw;
        }
        if (!present) { available = false; step.unavailable.push_back(selected); }
    }
    step.outcome.observed.emplace("review_focus_available", available);
    if (action.kind == TaskActionKind::RequestPublication) {
        step.outcome.observed.emplace("publication_requested", true);
        return step;
    }
    const auto graph = parse(payload);
    require(string(graph.get(), "format") == "gammathink.inspection.v1" &&
        string(graph.get(), "root") == hex(root) && boolean(graph.get(), "observational_only") &&
        !boolean(graph.get(), "published"), ErrorCode::Integrity, "Inspection changed its observational scope");
    auto* nodes = json_object_object_get(graph.get(), "nodes");
    auto* missing = json_object_object_get(graph.get(), "unavailable");
    require(nodes && missing && json_object_is_type(nodes, json_type_array) &&
        json_object_is_type(missing, json_type_array) && json_object_array_length(nodes) <= 256 &&
        json_object_array_length(missing) <= 4096, ErrorCode::Integrity, "Inspection lost its bounded graph");
    std::set<ObjectId> captured, absent;
    for (std::size_t i = 0; i < json_object_array_length(nodes); ++i) {
        auto* node = json_object_array_get_idx(nodes, i);
        require(node && json_object_is_type(node, json_type_object), ErrorCode::Integrity, "Invalid inspection node");
        const auto id = parse_id(string(node, "id"));
        require(captured.insert(id).second, ErrorCode::Integrity, "Repeated inspection node identity");
        const auto decoded = decode_object(id, bytes(string(node, "canonical_hex")));
        require(string(node, "kind") == name(decoded.kind) && string(node, "visibility") == name(decoded.visibility) &&
            string(node, "label") == decoded.label && string(node, "format") == decoded.format &&
            bytes(string(node, "payload_hex")) == decoded.payload, ErrorCode::Integrity,
            "Inspection display differs from its canonical object");
        auto* concepts = json_object_object_get(node, "concepts");
        auto* references = json_object_object_get(node, "sources");
        auto* links = json_object_object_get(node, "links");
        require(concepts && references && links && json_object_is_type(concepts, json_type_array) &&
            json_object_is_type(references, json_type_array) && json_object_is_type(links, json_type_array) &&
            json_object_array_length(concepts) == decoded.concepts.size() &&
            json_object_array_length(references) == decoded.sources.size() &&
            json_object_array_length(links) == decoded.links.size(), ErrorCode::Integrity, "Inspection changed canonical metadata");
        for (std::size_t j = 0; j < decoded.concepts.size(); ++j) {
            auto* concept_name = json_object_array_get_idx(concepts, j);
            require(concept_name && json_object_is_type(concept_name, json_type_string) &&
                std::string_view(json_object_get_string(concept_name)) == decoded.concepts[j],
                ErrorCode::Integrity, "Inspection changed a canonical concept");
        }
        for (std::size_t j = 0; j < decoded.sources.size(); ++j) {
            auto* source = json_object_array_get_idx(references, j);
            const auto& reference = decoded.sources[j];
            require(string(source, "archive") == reference.archive_id && string(source, "version") == reference.archive_version &&
                string(source, "entry") == reference.entry_path && string(source, "extractor") == reference.extractor_version &&
                string(source, "digest") == hex({reference.source_digest}) &&
                string(source, "offset") == std::to_string(reference.offset) &&
                string(source, "length") == std::to_string(reference.length),
                ErrorCode::Integrity, "Inspection changed canonical provenance");
        }
        for (std::size_t j = 0; j < decoded.links.size(); ++j) {
            auto* link = json_object_array_get_idx(links, j);
            require(string(link, "relationship") == name(decoded.links[j].relationship) &&
                string(link, "target") == hex(decoded.links[j].target), ErrorCode::Integrity, "Inspection changed a canonical edge");
        }
        auto* derivation = json_object_object_get(node, "derivation");
        require(bool(derivation) == decoded.derivation.has_value(), ErrorCode::Integrity, "Inspection changed its derivation");
        if (decoded.derivation) {
            require(string(derivation, "conclusion") == hex(decoded.derivation->conclusion) &&
                string(derivation, "rule") == hex(decoded.derivation->rule), ErrorCode::Integrity,
                "Inspection changed a canonical derivation scope");
            identities(derivation, "premises", decoded.derivation->premises);
            identities(derivation, "assumptions", decoded.derivation->assumptions);
            identities(derivation, "evidence", decoded.derivation->evidence);
        }
    }
    for (std::size_t i = 0; i < json_object_array_length(missing); ++i) {
        auto* value = json_object_array_get_idx(missing, i);
        require(value && json_object_is_type(value, json_type_string), ErrorCode::Integrity, "Invalid unavailable inspection identity");
        const auto id = parse_id(json_object_get_string(value));
        require(!captured.contains(id) && absent.insert(id).second, ErrorCode::Integrity,
            "Inspection both contains and loses an object");
    }
    require(captured.contains(root) || absent.contains(root), ErrorCode::Integrity, "Inspection omitted its selected focus");
    step.outcome.observed.emplace("inspection_recorded", true);
    step.outcome.observed.emplace("inspection_nodes", static_cast<std::int64_t>(json_object_array_length(nodes)));
    step.outcome.observed.emplace("inspection_unavailable", static_cast<std::int64_t>(json_object_array_length(missing)));
    step.outcome.observed.emplace("inspection_limited", boolean(graph.get(), "exploration_limited"));
    step.outcome.observed.emplace("inspection_historical", restored);
    return step;
}
void apply(const AdmittedTaskPlan& plan, const TaskAction& action, const NativeTaskStep& step, TaskExecutionState& state) {
    state.capture = digest({reinterpret_cast<const std::uint8_t*>(plan.dsl().data()), plan.dsl().size()});
    state.outcomes[action.goal] = step.outcome;
    state.artifacts[action.outputs.front()] = step.artifact;
    state.steps[action.id] = step;
}
}

NativeTaskStep TaskSession::review_action(std::string_view session, std::uint64_t revision,
    const TaskPlanContext& context, std::string_view id, TaskExecutionState& state, UnifiedKnowledge& sources,
    const GoalConditions& environment, const std::set<ObjectId>& accepted, InspectionLimits inspection) {
    require(model_.state() == neural::ModelState::Unloaded, ErrorCode::Permission, "Unload models before private review");
    const auto plan = active_plan(session, revision, context);
    const auto& action = runtime::review_action(plan, id);
    (void)check_execution_state(session, revision, plan, state);
    ready(context, plan, action, state, environment, accepted);
    auto task = tasks_.active(session);
    require(task && task->revision == revision, ErrorCode::Schema, "Task revision changed");
    const auto roots = focuses(plan, action, state);
    const auto payload = action.kind == TaskActionKind::Visualize
        ? inspect_knowledge(sources, roots.front(), inspection) : request(task->ordered_task, action, roots);
    const auto view = artifact(plan, action, task->ordered_task, roots, payload);
    const auto view_id = identify(view);
    const auto marker = receipt(plan, action, task->ordered_task, roots, view_id);
    const auto step = observation(sources, action, roots, view_id, identify(marker), payload, false);
    (void)active_plan(session, revision, context);
    if (task->phase != context::TaskPhase::Reasoning) task = tasks_.transition(session, revision, context::TaskPhase::Reasoning);
    journal_.put(view); journal_.put(marker);
    auto pending = state; apply(plan, action, step, pending);
    const auto evaluation = evaluate_task_plan(context, plan, pending.outcomes, environment, pending.artifacts, accepted);
    tasks_.transition(session, task->revision,
        evaluation.goals.root_satisfied ? context::TaskPhase::Checked : context::TaskPhase::Unresolved,
        pending.certificates, context::CompletedTaskAction{task->revision + 1, action.id, step.receipt});
    state = std::move(pending);
    return step;
}

NativeTaskStep TaskSession::restore_review_action(const TaskPlanContext& context, const AdmittedTaskPlan& plan,
    const context::SessionTask& task, const context::CompletedTaskAction& marker, TaskExecutionState& state,
    UnifiedKnowledge& sources, const GoalConditions& environment, const std::set<ObjectId>& accepted) {
    const auto& action = runtime::review_action(plan, marker.action);
    ready(context, plan, action, state, environment, accepted);
    const auto stored = record(journal_, marker.receipt, ObjectKind::TestResult, "gammathink.task-review-action.v1");
    const auto fields = parse(std::string(stored.payload.begin(), stored.payload.end()));
    const auto view_id = parse_id(string(fields.get(), "artifact"));
    const auto roots = focuses(plan, action, state);
    const auto format = action.kind == TaskActionKind::Visualize
        ? "gammathink.task-inspection-view.v1" : "gammathink.publication-request.v1";
    const auto view = record(journal_, view_id, ObjectKind::Evidence, format);
    const std::string payload(view.payload.begin(), view.payload.end());
    const auto expected = artifact(plan, action, task.ordered_task, roots, payload);
    require(view == expected && stored == receipt(plan, action, task.ordered_task, roots, view_id),
        ErrorCode::Integrity, "Review lost its exact selected focus, task or artifact");
    if (action.kind == TaskActionKind::RequestPublication)
        require(payload == request(task.ordered_task, action, roots), ErrorCode::Integrity,
            "Publication request acquired authority or changed its selected root");
    // Revalidate the captured canonical nodes, never rerun a graph exploration:
    // persisting this view itself introduces new incoming references.
    const auto step = observation(sources, action, roots, view_id, marker.receipt, payload, true);
    apply(plan, action, step, state);
    return step;
}

} // namespace axiom::runtime
