#include "task_session.hpp"

#include <algorithm>
#include <json-c/json.h>
#include <sstream>

namespace axiom::runtime {
namespace {
using namespace knowledge;
using Json = std::unique_ptr<json_object, decltype(&json_object_put)>;
void require(bool condition, ErrorCode code, const char* message) {
    if (!condition) throw Error(code, message);
}
std::map<std::string, Json> entities(const AdmittedTaskPlan& plan) {
    std::map<std::string, Json> result;
    std::istringstream input(plan.dsl());
    std::string line;
    while (std::getline(input, line)) {
        const auto first = line.find_first_not_of(" \t\r");
        if (first == std::string::npos) continue;
        const auto separator = line.find_first_of(" \t", first);
        if (separator == std::string::npos || line.substr(first, separator - first) != "ENTITY") continue;
        Json value(json_tokener_parse(line.c_str() + separator), json_object_put);
        require(value && json_object_is_type(value.get(), json_type_array), ErrorCode::Integrity,
            "Admitted refinement lost its entity declaration");
        auto* id = json_object_array_get_idx(value.get(), 0);
        require(id && json_object_is_type(id, json_type_string), ErrorCode::Integrity,
            "Admitted refinement lost its entity identity");
        result.emplace(json_object_get_string(id), std::move(value));
    }
    return result;
}
void preserves(const AdmittedTaskPlan& previous, const AdmittedTaskPlan& next) {
    const auto old_context = restore_task_plan_context(previous.context_wire());
    const auto new_context = restore_task_plan_context(next.context_wire());
    auto old_registry = old_context.sources().context(), new_registry = new_context.sources().context();
    const auto old_views = old_registry.views, new_views = new_registry.views;
    old_registry.views.clear(); new_registry.views.clear();
    require(old_registry == new_registry && old_context.current_question() == new_context.current_question() &&
        old_views.size() <= new_views.size(), ErrorCode::Integrity, "Refinement changed the original registry or source handles");
    const auto pin_prefix = "conversation:" + std::to_string(static_cast<unsigned>(context::ContextLayer::Constraint)) + ":";
    for (std::size_t i = 0; i < old_views.size(); ++i) {
        if (old_views[i] == new_views[i]) continue;
        require(i != old_context.current_question() && old_views[i].id.starts_with(pin_prefix) &&
            new_views[i].id.starts_with(pin_prefix) && old_views[i].archive_id == new_views[i].archive_id &&
            old_views[i].archive_id.starts_with("private-conversation:") &&
            old_views[i].extractor_version == "conversation-context-v1" &&
            new_views[i].extractor_version == "conversation-context-v1", ErrorCode::Integrity,
            "Refinement replaced an original non-correction source");
    }
    for (const auto& [alias, object] : old_context.objects()) {
        const auto found = new_context.objects().find(alias);
        require(found != new_context.objects().end() && found->second.id == object.id &&
            found->second.kind == object.kind && found->second.format == object.format,
            ErrorCode::Integrity, "Refinement replaced an advertised original object");
    }
    auto policies = next.active_constraints();
    for (const auto& policy : previous.active_constraints()) {
        const auto found = std::find(policies.begin(), policies.end(), policy);
        require(found != policies.end(), ErrorCode::Integrity, "Refinement weakened an original host constraint");
        policies.erase(found);
    }
    require(previous.graph().nodes.at(previous.graph().root).id == next.graph().nodes.at(next.graph().root).id,
        ErrorCode::Integrity, "Refinement changed the requested root goal");
    const auto old_entities = entities(previous), new_entities = entities(next);
    for (const auto& [id, entity] : old_entities) {
        const auto found = new_entities.find(id);
        require(found != new_entities.end() && json_object_equal(entity.get(), found->second.get()),
            ErrorCode::Integrity, "Refinement replaced or removed an original requested entity");
    }
    for (const auto& entity : previous.entities()) {
        const auto found = std::find_if(next.entities().begin(), next.entities().end(),
            [&](const auto& value) { return value.id == entity.id; });
        require(found != next.entities().end() && found->supports == entity.supports &&
            found->value.index() == entity.value.index(), ErrorCode::Integrity, "Refinement changed original source bindings");
        if (const auto* value = std::get_if<TaskText>(&entity.value)) {
            const auto& replacement = std::get<TaskText>(found->value);
            require(value->value == replacement.value && value->span == replacement.span,
                ErrorCode::Integrity, "Refinement changed original requested text");
        } else if (const auto* value = std::get_if<AdmittedTask>(&entity.value)) {
            const auto& replacement = std::get<AdmittedTask>(found->value);
            require(value->request().first.proposal == replacement.request().first.proposal &&
                value->request().first.limits == replacement.request().first.limits &&
                value->request().goals == replacement.request().goals, ErrorCode::Integrity,
                "Refinement changed original formal premises or requested results");
        }
    }
    for (const auto& action : previous.actions()) {
        const auto found = std::find_if(next.actions().begin(), next.actions().end(),
            [&](const auto& value) { return value.id == action.id; });
        require(found != next.actions().end() && found->goal == action.goal && found->kind == action.kind &&
            found->inputs == action.inputs && found->outputs == action.outputs && found->supports == action.supports,
            ErrorCode::Integrity, "Refinement replaced or removed an original requested action");
    }
    for (const auto& goal : previous.definitions()) {
        const auto found = std::find_if(next.definitions().begin(), next.definitions().end(),
            [&](const auto& value) { return value.id == goal.id; });
        require(found != next.definitions().end() && found->constraints == goal.constraints &&
            found->success_conditions == goal.success_conditions && found->requirements == goal.requirements &&
            found->success_requirements == goal.success_requirements && found->assumptions == goal.assumptions &&
            found->sources == goal.sources && found->formal_request == goal.formal_request, ErrorCode::Integrity,
            "Refinement changed an original requirement, assumption or success condition");
        if (goal.methods.empty()) continue;
        require(!found->methods.empty(), ErrorCode::Integrity, "Refinement removed an original prerequisite");
        for (const auto& method : found->methods) {
            const bool stronger = std::any_of(goal.methods.begin(), goal.methods.end(), [&](const auto& original) {
                std::size_t matched = 0;
                for (const auto& prerequisite : method)
                    if (matched < original.size() && prerequisite == original[matched]) ++matched;
                return matched == original.size();
            });
            require(stronger, ErrorCode::Integrity, "Refinement weakened an original ordered AND/OR method");
        }
    }
}
Object receipt(const ObjectId& parent, const ObjectId& task, const AdmittedTaskPlan& plan,
    const context::SessionTask& source) {
    Json value(json_object_new_object(), json_object_put);
    require(bool(value), ErrorCode::Storage, "Cannot allocate refinement receipt");
    const auto text = [&](const char* key, std::string_view text_value) {
        json_object_object_add(value.get(), key,
            json_object_new_string_len(text_value.data(), static_cast<int>(text_value.size())));
    };
    text("parent", hex(parent)); text("task", hex(task));
    text("snapshot", plan.snapshot()); text("original_message_hash", hex({source.message_hash}));
    json_object_object_add(value.get(), "original_message", json_object_new_uint64(source.user_message));
    json_object_object_add(value.get(), "typed_intent_preserved", json_object_new_boolean(true));
    json_object_object_add(value.get(), "english_translation_qualified", json_object_new_boolean(false));
    json_object_object_add(value.get(), "prior_operations_reused", json_object_new_boolean(false));
    json_object_object_add(value.get(), "empirical_truth_verified", json_object_new_boolean(false));
    Object result;
    result.kind = ObjectKind::TestResult; result.label = "Task refinement";
    result.format = "gammathink.task-refinement.v1";
    const std::string encoded = json_object_to_json_string_ext(value.get(), JSON_C_TO_STRING_PLAIN);
    result.payload.assign(encoded.begin(), encoded.end());
    result.sources = plan.graph().nodes.at(plan.graph().root).definition->sources;
    result.links = {{Relationship::DependsOn, parent}, {Relationship::DependsOn, task}};
    return result;
}
}

void TaskSession::check_refinement_history(std::string_view session, const context::SessionTask& task,
    const AdmittedTaskPlan& plan) {
    auto current = task.ordered_task;
    auto child = plan;
    auto before = task.revision + 1;
    for (std::size_t depth = 0;; ++depth) {
        const auto link = tasks_.refinement(session, current);
        if (!link) return;
        require(depth < 64 && link->revision < before && link->parent != current,
            ErrorCode::Budget, "Task refinement history is cyclic or exceeds its budget");
        before = link->revision;
        const auto parent = restore_task_plan(journal_, link->parent);
        preserves(parent, child);
        const auto expected = receipt(link->parent, current, child, task);
        const auto actual = journal_.get(link->receipt);
        const auto status = journal_.assessment(link->receipt);
        require(actual && *actual == expected && identify(expected) == link->receipt &&
            (!status || (status->state != KnowledgeState::Invalidated && status->logical != LogicalValidity::Invalid)),
            ErrorCode::Integrity, "Refinement lost its checked immutable intent receipt");
        current = link->parent; child = parent;
    }
}

PlanSessionAcceptance TaskSession::refine_plan(std::string_view session, std::uint64_t expected_revision,
    const TaskPlanContext& context, std::string_view snapshot, std::string_view task_ir) {
    require(model_.state() == neural::ModelState::Unloaded, ErrorCode::Permission,
        "Unload models before refining a task plan");
    const auto current = tasks_.active(session);
    require(current && current->revision == expected_revision && current->phase != context::TaskPhase::Checked &&
        current->phase != context::TaskPhase::Cancelled, ErrorCode::Schema, "Task is not available for refinement");
    const auto original = source(session, current->user_message);
    require(original.content_hash == current->message_hash, ErrorCode::Integrity, "Original task message changed");
    const auto& question = context.sources().context().views.at(context.current_question());
    require(question.text == original.content && question.archive_id == "private-conversation:" + evidence_digest(original.session) &&
        question.entry_path == "message:" + std::to_string(original.id) &&
        task_context_pins_current(memory_, session, context), ErrorCode::Integrity,
        "Refinement does not capture the original turn and current corrections");
    const auto parent = restore_task_plan(journal_, current->ordered_task);
    check_refinement_history(session, *current, parent);
    auto interpretation = admit_task_plan(context, snapshot, task_ir);
    if (const auto* decline = std::get_if<TaskPlanDecline>(&interpretation)) return *decline;
    const auto& next = std::get<AdmittedTaskPlan>(interpretation);
    preserves(parent, next);
    const auto stored = persist_task_plan(journal_, next);
    const auto record = receipt(current->ordered_task, stored.manifest, next, *current);
    const auto id = journal_.put(record);
    require(task_context_pins_current(memory_, session, context), ErrorCode::Integrity,
        "Pinned corrections changed during refinement");
    const auto refined = tasks_.refine(memory_, session, stored.manifest, id, expected_revision);
    // Old execution capabilities cannot survive a new captured plan.
    executor_.reset();
    return refined;
}

} // namespace axiom::runtime
