#include "task_operators.hpp"
#include "binary_dispatch.hpp"

#include <algorithm>
#include <json-c/json.h>
#include <type_traits>

namespace axiom::runtime {
namespace {
using namespace knowledge;
void require(bool value, ErrorCode code, const char* message) {
    if (!value) throw Error(code, message);
}
json_object* ids(const std::vector<ObjectId>& values) {
    auto* result = json_object_new_array();
    for (const auto& id : values) json_object_array_add(result, json_object_new_string(hex(id).c_str()));
    return result;
}
using Json = std::unique_ptr<json_object, decltype(&json_object_put)>;
void text(json_object* parent, const char* key, std::string_view value) {
    json_object_object_add(parent, key, json_object_new_string_len(value.data(), static_cast<int>(value.size())));
}
void current_object(Journal& journal, const ObjectId& id) {
    const auto assessment = journal.assessment(id);
    require(!assessment || (assessment->state != KnowledgeState::Invalidated &&
        assessment->logical != LogicalValidity::Invalid), ErrorCode::Integrity,
        "Response input requires explicit revalidation");
}
Json response(Journal& journal, const NativeTaskStep& source, NativeTaskLimits limits,
    const std::set<ObjectId>& accepted) {
    require(limits.response_bytes > 0 && limits.response_bytes <= 262144 &&
        limits.response_objects > 0 && limits.response_objects <= 256, ErrorCode::Budget,
        "Invalid deterministic response budget");
    require(source.certificates.size() + source.results.size() <= limits.response_objects,
        ErrorCode::Budget, "Deterministic response object budget exceeded");
    current_object(journal, source.receipt);
    Json result(json_object_new_object(), json_object_put);
    require(bool(result), ErrorCode::Storage, "Cannot allocate deterministic response");
    text(result.get(), "profile", "gammathink.native-result-view.v1");
    text(result.get(), "operator_receipt", hex(source.receipt));
    json_object_object_add(result.get(), "source_status", json_object_new_int(static_cast<int>(source.status)));
    json_object_object_add(result.get(), "source_native_executions", json_object_new_uint64(source.native_executions));
    auto* observations = json_object_new_object();
    json_object_object_add(result.get(), "source_observations", observations);
    for (const auto& [key, value] : source.outcome.observed) {
        std::visit([&](const auto& item) {
            using Value = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<Value, bool>)
                json_object_object_add(observations, key.c_str(), json_object_new_boolean(item));
            else if constexpr (std::is_same_v<Value, std::int64_t>)
                json_object_object_add(observations, key.c_str(), json_object_new_int64(item));
            else text(observations, key.c_str(), item);
        }, value);
    }
    json_object_object_add(result.get(), "empirical_truth_verified", json_object_new_boolean(false));
    json_object_object_add(result.get(), "source_translation_qualified", json_object_new_boolean(false));
    json_object_object_add(result.get(), "unavailable", ids(source.unavailable));
    json_object_object_add(result.get(), "revalidation_required", ids(source.revalidation_required));
    auto* proofs = json_object_new_array();
    auto* objects = json_object_new_array();
    json_object_object_add(result.get(), "native_results", proofs);
    json_object_object_add(result.get(), "retrieved_objects", objects);
    const auto bounded = [&] {
        const auto* wire = json_object_to_json_string_ext(result.get(), JSON_C_TO_STRING_PLAIN);
        require(wire && std::string_view(wire).size() <= limits.response_bytes,
            ErrorCode::Budget, "Deterministic response byte budget exceeded");
    };
    for (const auto& id : source.certificates) {
        current_object(journal, id);
        const auto certificate = journal.get(id);
        require(certificate && certificate->format == native_reply_format &&
            certificate->kind == ObjectKind::TestResult, ErrorCode::Integrity, "Response lost a native certificate");
        std::optional<ObjectId> request;
        for (const auto& link : certificate->links) if (link.relationship == Relationship::Tests) {
            require(!request, ErrorCode::Integrity, "Response certificate has ambiguous request identity");
            request = link.target;
        }
        require(request.has_value(), ErrorCode::Integrity, "Response certificate has no original request");
        current_object(journal, *request);
        const auto goal = journal.get(*request);
        require(goal && goal->kind == ObjectKind::Goal && goal->format == native_request_format &&
            check_native_reply(goal->payload, certificate->payload).valid, ErrorCode::Integrity,
            "Response certificate does not verify against its original request");
        std::vector<ObjectId> requirements;
        for (const auto& link : goal->links) if (link.relationship == Relationship::Requires) {
            const auto prerequisite = journal.get(link.target);
            require(prerequisite.has_value(), ErrorCode::Missing, "Response prerequisite is unavailable");
            current_object(journal, link.target);
            require(prerequisite->kind != ObjectKind::Assumption || accepted.contains(link.target),
                ErrorCode::Permission, "Response assumption is not accepted");
            requirements.push_back(link.target);
        }
        auto* item = json_object_new_object();
        json_object_array_add(proofs, item);
        text(item, "request", hex(*request)); text(item, "certificate", hex(id));
        text(item, "rendered_result", render_binary_result(certificate->payload));
        json_object_object_add(item, "requires", ids(requirements));
        auto* sources = json_object_new_array();
        json_object_object_add(item, "sources", sources);
        for (const auto& source_ref : goal->sources) {
            auto* reference = json_object_new_object();
            json_object_array_add(sources, reference);
            text(reference, "archive", source_ref.archive_id); text(reference, "version", source_ref.archive_version);
            text(reference, "entry", source_ref.entry_path); text(reference, "extractor", source_ref.extractor_version);
            text(reference, "digest", hex({source_ref.source_digest}));
            json_object_object_add(reference, "offset", json_object_new_uint64(source_ref.offset));
            json_object_object_add(reference, "length", json_object_new_uint64(source_ref.length));
        }
        bounded();
    }
    for (const auto& id : source.results) {
        current_object(journal, id);
        const auto object = journal.get(id);
        require(object.has_value(), ErrorCode::Missing, "Response lost a retrieved object");
        const auto encoded = encode_object(*object);
        require(encoded.size() <= limits.response_bytes / 2, ErrorCode::Budget,
            "Retrieved object exceeds deterministic response budget");
        constexpr char digits[] = "0123456789abcdef";
        std::string hex_bytes;
        hex_bytes.reserve(encoded.size() * 2);
        for (const auto value : encoded) {
            hex_bytes.push_back(digits[value >> 4]); hex_bytes.push_back(digits[value & 15]);
        }
        auto* item = json_object_new_object();
        json_object_array_add(objects, item);
        text(item, "id", hex(id)); text(item, "kind", name(object->kind));
        text(item, "format", object->format); text(item, "canonical_hex", hex_bytes);
        bounded();
    }
    bounded();
    return result;
}
}

NativeTaskOperators::NativeTaskOperators(UnifiedKnowledge& knowledge, Journal& journal,
    neural::NativeModelManager& model) : knowledge_(knowledge), journal_(journal), model_(model) {}

NativeTaskStep NativeTaskOperators::execute(const TaskPlanContext& context, const AdmittedTaskPlan& plan,
    std::string_view requested, TaskExecutionState& state, const GoalConditions& environment,
    const std::set<ObjectId>& accepted, const std::map<ObjectId, ProblemContext>& source_contexts,
    NativeTaskLimits limits) {
    require(model_.state() == neural::ModelState::Unloaded, ErrorCode::Permission,
            "Unload neural models before native task operations");
    const auto capture = digest({reinterpret_cast<const std::uint8_t*>(plan.dsl().data()), plan.dsl().size()});
    require(!state.capture || *state.capture == capture, ErrorCode::Integrity, "Task execution state belongs to another capture");
    require(state.capture || (state.outcomes.empty() && state.artifacts.empty() && state.certificates.empty() &&
        state.steps.empty()), ErrorCode::Integrity,
            "Unbound task execution state contains outcomes");
    const auto action = std::find_if(plan.actions().begin(), plan.actions().end(),
        [&](const auto& value) { return value.id == requested; });
    require(action != plan.actions().end(), ErrorCode::Missing, "Unknown task action");
    require(action->kind == TaskActionKind::Reason || action->kind == TaskActionKind::SourceReason ||
        action->kind == TaskActionKind::Retrieve || action->kind == TaskActionKind::Explain,
        ErrorCode::Schema, "Action is outside the native computational profile");
    require(limits.query_page > 0 && limits.query_page <= 1024, ErrorCode::Budget, "Invalid native retrieval page bound");
    const auto ready = evaluate_task_plan(context, plan, state.outcomes, environment, state.artifacts, accepted);
    require(std::find(ready.ready_actions.begin(), ready.ready_actions.end(), requested) != ready.ready_actions.end(),
            ErrorCode::Permission, "Task action prerequisites or live constraints are not ready");
    const auto entity = std::find_if(plan.entities().begin(), plan.entities().end(),
        [&](const auto& value) { return value.id == action->inputs.front(); });
    require(entity != plan.entities().end(), ErrorCode::Integrity, "Admitted native action lost its input");
    const auto persisted = persist_task_plan(journal_, plan);
    NativeTaskStep step;
    Json rendered(nullptr, json_object_put);
    bool valid = false;
    std::vector<ObjectId> inputs;
    const auto use = [&](KnowledgeRun run) {
        step.status = run.status;
        valid = run.execution.check.valid;
        if (run.execution.certificate) step.certificates.push_back(*run.execution.certificate);
        step.native_executions += !run.execution.reply.empty() && !run.execution.reused;
        step.unavailable = std::move(run.unavailable);
        step.revalidation_required = std::move(run.revalidation_required);
    };
    if (action->kind == TaskActionKind::Reason) {
        if (const auto* task = std::get_if<AdmittedTask>(&entity->value)) {
            const auto run = execute_task(journal_, *task, limits.knowledge.reasoning, limits.knowledge.materialization);
            inputs = run.task.requests;
            valid = !run.slots.empty();
            bool conflict = false;
            for (const auto& slot : run.slots) {
                valid &= slot.execution.check.valid;
                conflict |= slot.execution.check.conflict;
                step.native_executions += !slot.execution.reply.empty() && !slot.execution.reused;
                if (slot.execution.certificate) step.certificates.push_back(*slot.execution.certificate);
                step.unavailable.insert(step.unavailable.end(), slot.execution.unavailable.begin(), slot.execution.unavailable.end());
            }
            step.status = conflict ? KnowledgeRunStatus::Conflict : run.evaluation.root_satisfied
                ? KnowledgeRunStatus::Checked : !step.unavailable.empty()
                ? KnowledgeRunStatus::DependenciesUnavailable : KnowledgeRunStatus::Unresolved;
        } else {
            const auto& object = std::get<TaskObject>(entity->value);
            inputs.push_back(object.id);
            use(run_knowledge_goal(knowledge_, journal_, object.id, limits.knowledge, accepted));
        }
    } else if (action->kind == TaskActionKind::SourceReason) {
        const auto& query = std::get<TaskSourceQuery>(entity->value);
        require(source_contexts.contains(query.module), ErrorCode::Missing, "Source action lacks an independent host context");
        inputs.push_back(query.module);
        if (const auto* quantity = std::get_if<ProblemQuantityGoal>(&query.goal)) {
            auto run = run_source_quantity(knowledge_, journal_, query.module, source_contexts.at(query.module), *quantity,
                                           limits.knowledge);
            if (run.request) inputs.push_back(*run.request);
            use(std::move(run.outcome));
        } else {
            auto run = run_source_logic(knowledge_, journal_, query.module, source_contexts.at(query.module),
                std::get<ProblemAtomGoal>(query.goal).atom, limits.logic);
            if (run.request) inputs.push_back(*run.request);
            use(std::move(run.outcome));
        }
    } else if (action->kind == TaskActionKind::Retrieve) {
        const auto page = knowledge_.query(std::get<KnowledgeQuery>(entity->value), {}, limits.query_page);
        bool limited = !page.complete;
        std::size_t bytes = 0;
        for (const auto& id : page.objects) {
            if (step.results.size() >= limits.knowledge.retrieval.objects) { limited = true; break; }
            try {
                const auto found = knowledge_.get(id);
                if (!found) { step.unavailable.push_back(id); continue; }
                const auto invalid = [](const std::optional<Assessment>& value) {
                    return value && (value->state == KnowledgeState::Invalidated || value->logical == LogicalValidity::Invalid);
                };
                if (invalid(found->record.assessment) || invalid(journal_.assessment(id))) {
                    step.revalidation_required.push_back(id); continue;
                }
                const auto size = encode_object(found->record.object).size();
                if (size > limits.knowledge.retrieval.encoded_bytes - bytes) { limited = true; break; }
                bytes += size;
                if (!journal_.contains(id)) journal_.put(found->record.object);
                step.results.push_back(id); // Import immutable bytes, never publisher assessments.
            } catch (const Error& error) {
                if (error.code == ErrorCode::Unavailable) step.unavailable.push_back(id);
                else if (error.code == ErrorCode::Budget) { limited = true; break; }
                else throw;
            }
        }
        step.status = !step.revalidation_required.empty() ? KnowledgeRunStatus::RevalidationRequired
            : !step.unavailable.empty() ? KnowledgeRunStatus::DependenciesUnavailable
            : limited ? KnowledgeRunStatus::ResourceLimited : KnowledgeRunStatus::Checked;
        step.outcome.observed.emplace("retrieved_count", static_cast<std::int64_t>(step.results.size()));
        step.outcome.observed.emplace("retrieval_complete", step.status == KnowledgeRunStatus::Checked);
    } else {
        require(action->inputs.size() == 1 && std::holds_alternative<TaskArtifact>(entity->value),
            ErrorCode::Schema, "Deterministic explanation requires one native result artifact");
        const auto producer = std::find_if(plan.actions().begin(), plan.actions().end(), [&](const auto& value) {
            return value.outputs.front() == entity->id && state.steps.contains(value.id);
        });
        require(producer != plan.actions().end() && producer->kind != TaskActionKind::Explain,
            ErrorCode::Schema, "Deterministic explanation requires a completed computational producer");
        const auto& original = state.steps.at(producer->id);
        require(state.outcomes.contains(producer->goal) && state.outcomes.at(producer->goal).verified &&
            state.artifacts.at(entity->id) == original.artifact &&
            state.outcomes.at(producer->goal).evidence == original.receipt,
            ErrorCode::Integrity, "Response state does not match the actual producer result");
        rendered = response(journal_, original, limits, accepted);
        inputs.push_back(original.receipt);
        step.status = KnowledgeRunStatus::Checked; // Rendering completed, not source truth.
        step.outcome.observed.emplace("render_completed", true);
        step.outcome.observed.emplace("empirical_truth_verified", false);
    }
    step.outcome.verified = true; // Native observation of this operation, not neural/source truth.
    step.outcome.succeeded = step.status == KnowledgeRunStatus::Checked;
    step.outcome.observed.emplace("operator_completed", true);
    step.outcome.observed.emplace("native_valid", valid);
    step.outcome.observed.emplace("native_checked", valid && step.outcome.succeeded);
    step.outcome.observed.emplace("conflict", step.status == KnowledgeRunStatus::Conflict);
    Object receipt;
    receipt.kind = ObjectKind::TestResult; receipt.label = action->id;
    receipt.format = rendered ? "gammathink.task-native-response.v1" : "gammathink.task-native-operator.v1";
    receipt.sources = plan.graph().nodes.at(plan.graph().root).definition->sources;
    std::unique_ptr<json_object, decltype(&json_object_put)> payload(json_object_new_object(), json_object_put);
    require(bool(payload), ErrorCode::Storage, "Cannot allocate native task receipt");
    json_object_object_add(payload.get(), "manifest", json_object_new_string(hex(persisted.manifest).c_str()));
    json_object_object_add(payload.get(), "action", json_object_new_string(action->id.c_str()));
    json_object_object_add(payload.get(), "goal", json_object_new_string(action->goal.c_str()));
    json_object_object_add(payload.get(), "kind", json_object_new_int(static_cast<int>(action->kind)));
    json_object_object_add(payload.get(), "status", json_object_new_int(static_cast<int>(step.status)));
    json_object_object_add(payload.get(), "native_valid", json_object_new_boolean(valid));
    json_object_object_add(payload.get(), "native_executions", json_object_new_uint64(step.native_executions));
    json_object_object_add(payload.get(), "certificates", ids(step.certificates));
    json_object_object_add(payload.get(), "results", ids(step.results));
    json_object_object_add(payload.get(), "unavailable", ids(step.unavailable));
    json_object_object_add(payload.get(), "revalidation_required", ids(step.revalidation_required));
    if (rendered) json_object_object_add(payload.get(), "response", rendered.release());
    const std::string wire = json_object_to_json_string_ext(payload.get(), JSON_C_TO_STRING_PLAIN);
    if (action->kind == TaskActionKind::Explain)
        require(wire.size() <= limits.response_bytes, ErrorCode::Budget, "Response receipt exceeds byte budget");
    receipt.payload.assign(wire.begin(), wire.end());
    receipt.links.push_back({Relationship::DependsOn, persisted.manifest});
    for (const auto& id : inputs) receipt.links.push_back({Relationship::DependsOn, id});
    for (const auto& id : step.certificates) receipt.links.push_back({Relationship::DependsOn, id});
    for (const auto& id : step.results) receipt.links.push_back({Relationship::DependsOn, id});
    step.receipt = journal_.put(receipt);
    step.artifact = step.receipt;
    step.outcome.evidence = step.receipt;
    state.capture = capture;
    state.outcomes[action->goal] = step.outcome;
    state.artifacts[action->outputs.front()] = step.artifact;
    state.certificates.insert(state.certificates.end(), step.certificates.begin(), step.certificates.end());
    if (action->kind == TaskActionKind::Explain) step.response = wire;
    state.steps[action->id] = step;
    return step;
}

} // namespace axiom::runtime
