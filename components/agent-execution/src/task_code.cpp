#include "task_session.hpp"

#include <algorithm>
#include <json-c/json.h>

namespace axiom::runtime {
namespace {
using namespace knowledge;
void require(bool condition, ErrorCode code, const char* message) {
    if (!condition) throw Error(code, message);
}
void current_object(Journal& journal, const ObjectId& id) {
    const auto assessment = journal.assessment(id);
    require(!assessment || (assessment->state != KnowledgeState::Invalidated &&
        assessment->logical != LogicalValidity::Invalid), ErrorCode::Integrity,
        "Code input requires explicit revalidation");
}
const TaskAction& code_action(const AdmittedTaskPlan& plan, std::string_view id) {
    const auto found = std::find_if(plan.actions().begin(), plan.actions().end(),
        [&](const auto& value) { return value.id == id; });
    require(found != plan.actions().end() && found->kind == TaskActionKind::ExecuteCode,
        ErrorCode::Schema, "Expected an admitted execute-code action");
    return *found;
}
void ready(const TaskPlanContext& context, const AdmittedTaskPlan& plan, std::string_view action,
    const TaskExecutionState& state, const GoalConditions& environment, const std::set<ObjectId>& accepted) {
    const auto evaluated = evaluate_task_plan(context, plan, state.outcomes, environment, state.artifacts, accepted);
    require(std::find(evaluated.ready_actions.begin(), evaluated.ready_actions.end(), action) !=
        evaluated.ready_actions.end(), ErrorCode::Permission, "Code action prerequisites or live policy are not ready");
}
using Json = std::unique_ptr<json_object, decltype(&json_object_put)>;
Json document(const Object& object) {
    std::unique_ptr<json_tokener, decltype(&json_tokener_free)> parser(json_tokener_new_ex(16), json_tokener_free);
    require(bool(parser), ErrorCode::Storage, "Cannot allocate execution record parser");
    json_tokener_set_flags(parser.get(), JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
    Json result(json_tokener_parse_ex(parser.get(), reinterpret_cast<const char*>(object.payload.data()),
        static_cast<int>(object.payload.size())), json_object_put);
    require(result && json_object_is_type(result.get(), json_type_object) &&
        json_tokener_get_error(parser.get()) == json_tokener_success &&
        json_tokener_get_parse_end(parser.get()) == object.payload.size(), ErrorCode::Integrity,
        "Malformed recorded execution document");
    return result;
}
std::string string(json_object* document, const char* key) {
    auto* value = json_object_object_get(document, key);
    require(value && json_object_is_type(value, json_type_string), ErrorCode::Integrity,
        "Missing execution document string");
    return {json_object_get_string(value), static_cast<std::size_t>(json_object_get_string_len(value))};
}
std::int64_t integer(json_object* document, const char* key) {
    auto* value = json_object_object_get(document, key);
    require(value && json_object_is_type(value, json_type_int), ErrorCode::Integrity,
        "Missing execution document integer");
    return json_object_get_int64(value);
}
void false_flag(json_object* document, const char* key) {
    auto* value = json_object_object_get(document, key);
    require(value && json_object_is_type(value, json_type_boolean) && !json_object_get_boolean(value),
        ErrorCode::Integrity, "Operational execution record claims logical or empirical truth");
}
Object record(Journal& journal, const ObjectId& id, ObjectKind kind, std::string_view format) {
    current_object(journal, id);
    const auto object = journal.get(id);
    require(object && object->visibility == Visibility::Private && object->kind == kind && object->format == format,
        ErrorCode::Integrity, "Execution record is unavailable or has the wrong private profile");
    return *object;
}
bool linked(const Object& object, Relationship relationship, const ObjectId& id) {
    return std::find(object.links.begin(), object.links.end(), Link{relationship, id}) != object.links.end();
}
json_object* identities(const std::vector<ObjectId>& values) {
    auto* array = json_object_new_array();
    for (const auto& id : values) json_object_array_add(array, json_object_new_string(hex(id).c_str()));
    return array;
}
std::vector<ObjectId> identity_array(json_object* object, const char* key) {
    auto* array = json_object_object_get(object, key);
    require(array && json_object_is_type(array, json_type_array) && json_object_array_length(array) <= 8,
        ErrorCode::Integrity, "Invalid recorded repair chain");
    std::vector<ObjectId> result;
    for (std::size_t i = 0; i < json_object_array_length(array); ++i) {
        auto* item = json_object_array_get_idx(array, i);
        require(item && json_object_is_type(item, json_type_string), ErrorCode::Integrity,
            "Invalid recorded repair identity");
        result.push_back(parse_id({json_object_get_string(item),
            static_cast<std::size_t>(json_object_get_string_len(item))}));
    }
    return result;
}
struct RecordedAttempt {
    ObjectId receipt, artifact;
    Object code, result;
    execution::Status status;
};
RecordedAttempt checked_attempt(Journal& journal, const ObjectId& id, const ObjectId& specification,
    std::size_t ordinal, json_object* tests) {
    auto result = record(journal, id, ObjectKind::TestResult, "gammathink.execution-result.v1");
    const auto fields = document(result);
    const auto status = integer(fields.get(), "status");
    require(integer(fields.get(), "attempt") == static_cast<std::int64_t>(ordinal) &&
        status >= static_cast<int>(execution::Status::Passed) &&
        status <= static_cast<int>(execution::Status::ResourceLimit) &&
        integer(fields.get(), "recorded_unix_ms") > 0 &&
        string(fields.get(), "method") == "darwin-sandbox-exact-output-v1",
        ErrorCode::Integrity, "Invalid recorded repair attempt");
    false_flag(fields.get(), "logical_proof"); false_flag(fields.get(), "empirical_truth");
    std::optional<ObjectId> artifact;
    for (const auto& link : result.links) if (link.relationship == Relationship::Tests) {
        require(!artifact, ErrorCode::Integrity, "Repair attempt has ambiguous code identity");
        artifact = link.target;
    }
    require(artifact.has_value(), ErrorCode::Integrity, "Repair attempt lost its exact tested code");
    auto code = record(journal, *artifact, ObjectKind::Action, "gammathink.cpp-source.v1");
    require(linked(result, Relationship::DependsOn, specification) &&
        linked(code, Relationship::DependsOn, specification), ErrorCode::Integrity,
        "Repair attempt changed the fixed test specification");
    const auto count = integer(fields.get(), "tests_passed");
    const auto total = json_object_array_length(tests);
    require(count >= 0 && static_cast<std::uint64_t>(count) <= total, ErrorCode::Integrity,
        "Invalid repair test completion count");
    const auto stage = string(fields.get(), "stage");
    require(stage == "compile" || stage == "test", ErrorCode::Integrity, "Unknown repair execution stage");
    if (status == static_cast<int>(execution::Status::Passed))
        require(static_cast<std::uint64_t>(count) == total && stage == "test" &&
            integer(fields.get(), "exit_code") == 0 && integer(fields.get(), "signal") == 0,
            ErrorCode::Integrity, "Repair success did not finish the fixed tests");
    if (status == static_cast<int>(execution::Status::CompileFailed))
        require(stage == "compile" && count == 0 && integer(fields.get(), "exit_code") != 0,
            ErrorCode::Integrity, "Recorded compiler failure is inconsistent");
    if (status == static_cast<int>(execution::Status::TestFailed)) {
        require(stage == "test" && static_cast<std::uint64_t>(count) < total, ErrorCode::Integrity,
            "Recorded test failure is inconsistent");
        if (integer(fields.get(), "exit_code") == 0 && integer(fields.get(), "signal") == 0)
            require(string(fields.get(), "diagnostics_hex") != string(json_object_array_get_idx(tests,
                static_cast<std::size_t>(count)), "output_hex"), ErrorCode::Integrity,
                "Recorded test failure actually matched the fixed expected output");
    }
    return {id, *artifact, std::move(code), std::move(result), static_cast<execution::Status>(status)};
}
NativeTaskStep observed(execution::Status status, const ObjectId& artifact, const ObjectId& execution,
    const ObjectId& specification, bool restored, json_object* record) {
    NativeTaskStep step;
    step.status = status == execution::Status::Passed ? KnowledgeRunStatus::Checked
        : status == execution::Status::ResourceLimit ? KnowledgeRunStatus::ResourceLimited
        : KnowledgeRunStatus::Unresolved;
    step.results = {artifact, execution, specification};
    step.outcome.verified = true;
    step.outcome.succeeded = status == execution::Status::Passed;
    step.outcome.observed = {
        {"execution_status", static_cast<std::int64_t>(status)}, {"execution_checked", step.outcome.succeeded},
        {"execution_record_restored", restored}, {"native_checked", false}, {"empirical_truth_verified", false},
        {"execution_attempts", std::int64_t{1}}, {"repair_requested", false},
        {"repair_generations", std::int64_t{0}}, {"repair_budget_exhausted", false},
        {"exit_code", integer(record, "exit_code")}, {"signal", integer(record, "signal")},
        {"tests_passed", integer(record, "tests_passed")}, {"execution_stage", string(record, "stage")},
    };
    return step;
}
void applied(const AdmittedTaskPlan& plan, const TaskAction& action, const NativeTaskStep& step,
    TaskExecutionState& state) {
    state.capture = digest({reinterpret_cast<const std::uint8_t*>(plan.dsl().data()), plan.dsl().size()});
    state.outcomes[action.goal] = step.outcome;
    state.artifacts[action.outputs.front()] = step.artifact;
    state.steps[action.id] = step;
}
}

void TaskSession::configure_execution(execution::Toolchain tools) {
    require(!executor_, ErrorCode::Permission, "Session execution is already configured");
    require(model_.state() == neural::ModelState::Unloaded, ErrorCode::Permission,
        "Unload neural models before configuring execution");
    executor_.reset(new execution::Executor(memory_, tasks_, journal_, model_, std::move(tools), execution_lock_));
}

execution::Permit TaskSession::permit_code_action(std::string_view session, std::uint64_t expected_revision,
    const TaskPlanContext& context, std::string_view action_id, const TaskExecutionState& state,
    const GoalConditions& environment, std::vector<execution::Test> tests,
    const std::set<ObjectId>& accepted, execution::Limits limits) {
    require(bool(executor_), ErrorCode::Permission, "Host has not configured execution");
    const auto plan = active_plan(session, expected_revision, context);
    const auto& action = code_action(plan, action_id);
    require(action.inputs.size() == 2 || limits.attempts == 1, ErrorCode::Schema,
        "Changing a fixed source requires an explicit admitted repair instruction");
    (void)check_execution_state(session, expected_revision, plan, state);
    ready(context, plan, action_id, state, environment, accepted);
    auto current = tasks_.active(session);
    require(current && current->revision == expected_revision, ErrorCode::Schema, "Session task revision changed");
    if (current->phase != context::TaskPhase::Reasoning)
        current = tasks_.transition(session, current->revision, context::TaskPhase::Reasoning);
    try {
        auto permit = executor_->permit(session, current->revision, std::move(tests), limits);
        permit.action_ = std::string(action_id);
        return permit;
    } catch (...) {
        const auto latest = tasks_.active(session);
        if (latest && latest->revision == current->revision)
            tasks_.transition(session, current->revision, context::TaskPhase::Failed);
        throw;
    }
}

NativeTaskStep TaskSession::execute_code_action(std::string_view session, std::uint64_t expected_revision,
    const TaskPlanContext& context, std::string_view action_id, TaskExecutionState& state,
    execution::Permit& permit, UnifiedKnowledge& sources, const GoalConditions& environment,
    const std::set<ObjectId>& accepted, const std::optional<CodeRepairOptions>& repair) {
    require(executor_ && permit.owner_ == executor_->capability_ &&
        permit.action_ == action_id && permit.task_.session == session &&
        permit.task_.revision == expected_revision, ErrorCode::Permission, "Foreign or stale code-action permit");
    const auto plan = active_plan(session, expected_revision, context);
    const auto& action = code_action(plan, action_id);
    const bool repair_requested = action.inputs.size() == 2;
    require(repair_requested || permit.limits_.attempts == 1, ErrorCode::Permission,
        "This admitted source has no repair authorization");
    if (repair_requested && permit.limits_.attempts > 1)
        require(repair && repair->model.role == neural::ModelRole::CodeGeneration &&
            repair->generation.output_tokens > 0 &&
            repair->generation.output_tokens < model_.limits().context_tokens, ErrorCode::Schema,
            "Bounded repair requires an explicit code-generation model and valid output reservation");
    (void)check_execution_state(session, expected_revision, plan, state);
    ready(context, plan, action_id, state, environment, accepted);
    const auto entity = std::find_if(plan.entities().begin(), plan.entities().end(),
        [&](const auto& value) { return value.id == action.inputs.front(); });
    require(entity != plan.entities().end(), ErrorCode::Integrity, "Code action lost its admitted source");
    std::string source;
    std::optional<ObjectId> original;
    if (const auto* text = std::get_if<TaskText>(&entity->value)) source = text->value;
    else {
        const TaskObject* advertised = std::get_if<TaskObject>(&entity->value);
        if (advertised) {
            require(advertised->kind == ObjectKind::Action && advertised->format == "gammathink.cpp-source.v1",
                ErrorCode::Schema, "Code action object is not an admitted C++ source artifact");
            original = advertised->id;
        } else {
            require(std::holds_alternative<TaskArtifact>(entity->value) && state.artifacts.contains(entity->id),
                ErrorCode::Missing, "Code action artifact is unavailable");
            original = state.artifacts.at(entity->id);
        }
        current_object(journal_, *original);
        auto object = journal_.get(*original);
        if (!object) {
            const auto found = sources.get(*original);
            require(found.has_value(), ErrorCode::Missing, "Advertised code source is unavailable");
            require(!found->record.assessment || (found->record.assessment->state != KnowledgeState::Invalidated &&
                found->record.assessment->logical != LogicalValidity::Invalid), ErrorCode::Integrity,
                "Advertised code source requires revalidation");
            object = found->record.object;
        }
        require(object->kind == ObjectKind::Action && object->format == "gammathink.cpp-source.v1",
            ErrorCode::Schema, "Execution artifact does not contain the exact C++ source profile");
        require(object->payload.size() <= permit.limits_.source_bytes, ErrorCode::Budget,
            "Code source exceeds its host execution budget");
        if (!journal_.contains(*original)) journal_.put(*object);
        source.assign(object->payload.begin(), object->payload.end());
    }
    try {
        auto result = executor_->attempt(permit, source, {}, original);
        std::vector<ObjectId> attempts{result.receipt}, generations;
        while (repair_requested && result.status != execution::Status::Passed &&
            permit.used_ < permit.limits_.attempts) {
            (void)active_plan(session, expected_revision, context);
            const auto candidate = repair_code_candidate(session, expected_revision, context, plan, action,
                state, permit, result, source, *repair);
            const auto code = record(journal_, candidate.artifact, ObjectKind::Action, "gammathink.cpp-source.v1");
            const std::string repaired(code.payload.begin(), code.payload.end());
            generations.push_back(candidate.receipt);
            result = executor_->attempt(permit, repaired, result.receipt, candidate.artifact);
            attempts.push_back(result.receipt);
        }
        (void)active_plan(session, expected_revision, context);
        if (original) current_object(journal_, *original);
        const auto execution_record = record(journal_, result.receipt, ObjectKind::TestResult, "gammathink.execution-result.v1");
        const auto observation = document(execution_record);
        auto step = observed(result.status, result.artifact, result.receipt, permit.specification_, false, observation.get());
        const bool exhausted = repair_requested && result.status != execution::Status::Passed &&
            permit.used_ == permit.limits_.attempts;
        if (exhausted) step.status = KnowledgeRunStatus::ResourceLimited;
        step.outcome.observed.insert_or_assign("execution_attempts", static_cast<std::int64_t>(result.attempt));
        step.outcome.observed.insert_or_assign("repair_requested", repair_requested);
        step.outcome.observed.insert_or_assign("repair_generations", static_cast<std::int64_t>(generations.size()));
        step.outcome.observed.insert_or_assign("repair_budget_exhausted", exhausted);
        if (repair_requested) {
            step.results.insert(step.results.end(), attempts.begin(), attempts.end() - 1);
            step.results.insert(step.results.end(), generations.begin(), generations.end());
        }
        Object receipt;
        receipt.kind = ObjectKind::TestResult; receipt.label = action.id;
        receipt.format = repair_requested ? "gammathink.task-code-repair.v1" : "gammathink.task-code-operator.v1";
        receipt.sources = plan.graph().nodes.at(plan.graph().root).definition->sources;
        Json payload(json_object_new_object(), json_object_put);
        require(bool(payload), ErrorCode::Storage, "Cannot allocate code-action receipt");
        const auto string = [&](const char* key, std::string_view value) {
            json_object_object_add(payload.get(), key,
                json_object_new_string_len(value.data(), static_cast<int>(value.size())));
        };
        string("manifest", hex(permit.task_.ordered_task)); string("action", action.id); string("goal", action.goal);
        string("specification", hex(permit.specification_)); string("artifact", hex(result.artifact));
        string("execution", hex(result.receipt));
        json_object_object_add(payload.get(), "execution_status", json_object_new_int(static_cast<int>(result.status)));
        if (original) string("input", hex(*original));
        else json_object_object_add(payload.get(), "input", nullptr);
        if (repair_requested) {
            const auto instruction = std::find_if(plan.entities().begin(), plan.entities().end(),
                [&](const auto& value) { return value.id == action.inputs[1]; });
            require(instruction != plan.entities().end() && std::holds_alternative<TaskText>(instruction->value),
                ErrorCode::Integrity, "Repair lost its exact admitted instruction");
            string("repair_instruction", std::get<TaskText>(instruction->value).value);
            json_object_object_add(payload.get(), "attempts", identities(attempts));
            json_object_object_add(payload.get(), "repair_generations", identities(generations));
        }
        const std::string wire = json_object_to_json_string_ext(payload.get(), JSON_C_TO_STRING_PLAIN);
        receipt.payload.assign(wire.begin(), wire.end());
        receipt.links = {{Relationship::DependsOn, permit.task_.ordered_task},
            {Relationship::DependsOn, permit.specification_}, {Relationship::DependsOn, result.artifact},
            {Relationship::DependsOn, result.receipt}};
        if (original) receipt.links.push_back({Relationship::DependsOn, *original});
        for (const auto& id : generations) receipt.links.push_back({Relationship::DependsOn, id});
        step.receipt = journal_.put(receipt); step.artifact = step.receipt;
        step.outcome.evidence = step.receipt;
        auto pending = state;
        applied(plan, action, step, pending);
        const auto evaluated = evaluate_task_plan(context, plan, pending.outcomes, environment, pending.artifacts, accepted);
        const auto phase = evaluated.goals.root_satisfied ? context::TaskPhase::Checked : context::TaskPhase::Unresolved;
        tasks_.transition(session, expected_revision, phase, pending.certificates,
            context::CompletedTaskAction{expected_revision + 1, action.id, step.receipt});
        state = std::move(pending);
        return step;
    } catch (...) {
        const auto current = tasks_.active(session);
        if (current && current->revision == expected_revision)
            tasks_.transition(session, expected_revision, context::TaskPhase::Failed);
        throw;
    }
}

NativeTaskStep TaskSession::restore_code_action(const TaskPlanContext& context, const AdmittedTaskPlan& plan,
    const context::SessionTask& task, const context::CompletedTaskAction& marker, TaskExecutionState& state,
    const GoalConditions& environment, const std::set<ObjectId>& accepted) {
    const auto& action = code_action(plan, marker.action);
    if (action.inputs.size() == 2)
        return restore_repair_action(context, plan, task, marker, state, environment, accepted);
    ready(context, plan, action.id, state, environment, accepted);
    const auto wrapper = record(journal_, marker.receipt, ObjectKind::TestResult, "gammathink.task-code-operator.v1");
    const auto fields = document(wrapper);
    require(string(fields.get(), "manifest") == hex(task.ordered_task) &&
        string(fields.get(), "action") == action.id && string(fields.get(), "goal") == action.goal,
        ErrorCode::Integrity, "Code action receipt changed task identity");
    const auto specification_id = parse_id(string(fields.get(), "specification"));
    const auto artifact_id = parse_id(string(fields.get(), "artifact"));
    const auto execution_id = parse_id(string(fields.get(), "execution"));
    const auto specification = record(journal_, specification_id, ObjectKind::Action, "gammathink.execution-tests.v1");
    const auto artifact = record(journal_, artifact_id, ObjectKind::Action, "gammathink.cpp-source.v1");
    const auto execution = record(journal_, execution_id, ObjectKind::TestResult, "gammathink.execution-result.v1");
    require(linked(wrapper, Relationship::DependsOn, task.ordered_task) &&
        linked(wrapper, Relationship::DependsOn, specification_id) &&
        linked(wrapper, Relationship::DependsOn, artifact_id) &&
        linked(wrapper, Relationship::DependsOn, execution_id) &&
        linked(specification, Relationship::DependsOn, task.ordered_task) &&
        linked(artifact, Relationship::DependsOn, specification_id) &&
        linked(execution, Relationship::Tests, artifact_id) &&
        linked(execution, Relationship::DependsOn, specification_id), ErrorCode::Integrity,
        "Code observation lost its fixed tests or exact artifact");
    const auto spec = document(specification);
    const auto run = document(execution);
    auto* tests = json_object_object_get(spec.get(), "tests");
    require(string(spec.get(), "session") == task.session &&
        integer(spec.get(), "user_message") == static_cast<std::int64_t>(task.user_message) &&
        string(spec.get(), "message_hash") == hex({task.message_hash}) &&
        marker.revision > 1 && integer(spec.get(), "revision") == static_cast<std::int64_t>(marker.revision - 1) &&
        integer(spec.get(), "attempts") == 1 && integer(run.get(), "attempt") == 1 &&
        integer(spec.get(), "source_bytes") > 0 &&
        artifact.payload.size() <= static_cast<std::uint64_t>(integer(spec.get(), "source_bytes")) &&
        tests && json_object_is_type(tests, json_type_array) && json_object_array_length(tests) > 0 &&
        json_object_array_length(tests) <= 16, ErrorCode::Integrity, "Recorded code-test scope changed");
    false_flag(run.get(), "logical_proof"); false_flag(run.get(), "empirical_truth");
    const auto status = integer(run.get(), "status");
    require(status >= static_cast<int>(execution::Status::Passed) &&
        status <= static_cast<int>(execution::Status::ResourceLimit) &&
        integer(fields.get(), "execution_status") == status &&
        integer(run.get(), "recorded_unix_ms") > 0 &&
        string(run.get(), "method") == "darwin-sandbox-exact-output-v1", ErrorCode::Integrity,
        "Invalid recorded code-execution observation");
    const auto completed = integer(run.get(), "tests_passed");
    require(completed >= 0 && static_cast<std::uint64_t>(completed) <= json_object_array_length(tests),
        ErrorCode::Integrity, "Invalid recorded test count");
    if (status == static_cast<int>(execution::Status::Passed))
        require(static_cast<std::uint64_t>(completed) == json_object_array_length(tests) &&
            integer(run.get(), "exit_code") == 0 && integer(run.get(), "signal") == 0 &&
            string(run.get(), "stage") == "test", ErrorCode::Integrity,
            "Recorded success did not finish the fixed tests");
    const auto entity = std::find_if(plan.entities().begin(), plan.entities().end(),
        [&](const auto& value) { return value.id == action.inputs.front(); });
    require(entity != plan.entities().end(), ErrorCode::Integrity, "Recorded code action lost its source");
    if (const auto* text = std::get_if<TaskText>(&entity->value)) {
        require(std::string(artifact.payload.begin(), artifact.payload.end()) == text->value &&
            json_object_object_get(fields.get(), "input") == nullptr, ErrorCode::Integrity,
            "Recorded source differs from admitted code text");
    } else {
        const auto input = parse_id(string(fields.get(), "input"));
        const auto advertised = std::get_if<TaskObject>(&entity->value);
        require(advertised ? advertised->id == input
            : state.artifacts.contains(entity->id) && state.artifacts.at(entity->id) == input,
            ErrorCode::Integrity, "Recorded source differs from admitted code object");
        current_object(journal_, input);
        const auto source = journal_.get(input);
        require(source && source->kind == ObjectKind::Action && source->format == "gammathink.cpp-source.v1" &&
            source->payload == artifact.payload && linked(wrapper, Relationship::DependsOn, input),
            ErrorCode::Integrity, "Recorded code input is unavailable or changed");
    }
    // This is an authenticated historical test observation, not another code
    // execution or a native logical proof. A fresh execution needs a new permit.
    auto step = observed(static_cast<execution::Status>(status), artifact_id, execution_id, specification_id, true, run.get());
    step.receipt = marker.receipt; step.artifact = step.receipt; step.outcome.evidence = step.receipt;
    applied(plan, action, step, state);
    return step;
}

NativeTaskStep TaskSession::restore_repair_action(const TaskPlanContext& context, const AdmittedTaskPlan& plan,
    const context::SessionTask& task, const context::CompletedTaskAction& marker, TaskExecutionState& state,
    const GoalConditions& environment, const std::set<ObjectId>& accepted) {
    const auto& action = code_action(plan, marker.action);
    require(action.inputs.size() == 2, ErrorCode::Integrity, "Recorded repair has no admitted repair instruction");
    ready(context, plan, action.id, state, environment, accepted);
    const auto wrapper = record(journal_, marker.receipt, ObjectKind::TestResult, "gammathink.task-code-repair.v1");
    const auto fields = document(wrapper);
    const auto instruction = std::find_if(plan.entities().begin(), plan.entities().end(),
        [&](const auto& value) { return value.id == action.inputs[1]; });
    require(instruction != plan.entities().end() && std::holds_alternative<TaskText>(instruction->value) &&
        string(fields.get(), "repair_instruction") == std::get<TaskText>(instruction->value).value &&
        string(fields.get(), "manifest") == hex(task.ordered_task) &&
        string(fields.get(), "action") == action.id && string(fields.get(), "goal") == action.goal,
        ErrorCode::Integrity, "Recorded repair changed its goal or original instruction");
    const auto specification_id = parse_id(string(fields.get(), "specification"));
    const auto specification = record(journal_, specification_id, ObjectKind::Action, "gammathink.execution-tests.v1");
    const auto spec = document(specification);
    const auto attempts = identity_array(fields.get(), "attempts");
    const auto generations = identity_array(fields.get(), "repair_generations");
    const auto limit = integer(spec.get(), "attempts");
    auto* tests = json_object_object_get(spec.get(), "tests");
    require(!attempts.empty() && limit >= 1 && limit <= 8 && attempts.size() <= static_cast<std::uint64_t>(limit) &&
        generations.size() + 1 == attempts.size() && tests && json_object_is_type(tests, json_type_array) &&
        json_object_array_length(tests) > 0 && json_object_array_length(tests) <= 16 &&
        marker.revision > 1 && integer(spec.get(), "revision") == static_cast<std::int64_t>(marker.revision - 1) &&
        string(spec.get(), "session") == task.session &&
        integer(spec.get(), "user_message") == static_cast<std::int64_t>(task.user_message) &&
        string(spec.get(), "message_hash") == hex({task.message_hash}) &&
        linked(wrapper, Relationship::DependsOn, task.ordered_task) &&
        linked(wrapper, Relationship::DependsOn, specification_id) &&
        linked(specification, Relationship::DependsOn, task.ordered_task), ErrorCode::Integrity,
        "Recorded repair changed its bounded fixed-test scope");
    const auto maximum_source = integer(spec.get(), "source_bytes");
    require(maximum_source > 0 && maximum_source <= 65536, ErrorCode::Integrity, "Invalid repair source budget");
    std::vector<RecordedAttempt> records;
    for (std::size_t i = 0; i < attempts.size(); ++i) {
        auto attempt = checked_attempt(journal_, attempts[i], specification_id, i + 1, tests);
        require(attempt.code.payload.size() <= static_cast<std::uint64_t>(maximum_source),
            ErrorCode::Integrity, "Repair exceeded its fixed source budget");
        if (i > 0) {
            const auto& previous = records.back();
            require(previous.status != execution::Status::Passed &&
                linked(attempt.result, Relationship::DependsOn, previous.receipt) &&
                linked(attempt.code, Relationship::DependsOn, previous.receipt) &&
                linked(wrapper, Relationship::DependsOn, generations[i - 1]), ErrorCode::Integrity,
                "Repair did not follow the exact previous failed attempt");
            const auto& initial = records.front().code.payload;
            const auto candidate = verify_repair_generation(context, plan, task, action, state, specification_id,
                previous.receipt, previous.artifact, generations[i - 1], std::string(initial.begin(), initial.end()));
            const auto code = record(journal_, candidate, ObjectKind::Action, "gammathink.cpp-source.v1");
            require(code.payload == attempt.code.payload &&
                linked(attempt.code, Relationship::DependsOn, candidate) &&
                linked(attempt.result, Relationship::DependsOn, candidate), ErrorCode::Integrity,
                "Tested repair differs from its recorded generated candidate");
        }
        records.push_back(std::move(attempt));
    }
    const auto entity = std::find_if(plan.entities().begin(), plan.entities().end(),
        [&](const auto& value) { return value.id == action.inputs.front(); });
    require(entity != plan.entities().end(), ErrorCode::Integrity, "Repair lost its immutable initial source");
    const auto& initial = records.front().code.payload;
    if (const auto* text = std::get_if<TaskText>(&entity->value)) {
        json_object* input = nullptr;
        require(std::string(initial.begin(), initial.end()) == text->value &&
            json_object_object_get_ex(fields.get(), "input", &input) && input == nullptr,
            ErrorCode::Integrity, "Repair changed its admitted starting source");
    } else {
        const auto input = parse_id(string(fields.get(), "input"));
        const auto advertised = std::get_if<TaskObject>(&entity->value);
        require(advertised ? advertised->id == input
            : state.artifacts.contains(entity->id) && state.artifacts.at(entity->id) == input,
            ErrorCode::Integrity, "Repair changed its starting code object");
        current_object(journal_, input);
        const auto source = journal_.get(input);
        require(source && source->kind == ObjectKind::Action && source->format == "gammathink.cpp-source.v1" &&
            source->payload == initial && linked(wrapper, Relationship::DependsOn, input), ErrorCode::Integrity,
            "Repair initial source is unavailable or changed");
    }
    const auto& final = records.back();
    require(string(fields.get(), "execution") == hex(final.receipt) &&
        string(fields.get(), "artifact") == hex(final.artifact) &&
        integer(fields.get(), "execution_status") == static_cast<int>(final.status) &&
        linked(wrapper, Relationship::DependsOn, final.receipt) &&
        linked(wrapper, Relationship::DependsOn, final.artifact), ErrorCode::Integrity,
        "Repair final result differs from its recorded chain");
    const bool exhausted = final.status != execution::Status::Passed &&
        attempts.size() == static_cast<std::uint64_t>(limit);
    require(final.status == execution::Status::Passed || exhausted, ErrorCode::Integrity,
        "Repair stopped before its declared completion or bounded exhaustion");
    const auto observation = document(final.result);
    auto step = observed(final.status, final.artifact, final.receipt, specification_id, true, observation.get());
    if (exhausted) step.status = KnowledgeRunStatus::ResourceLimited;
    step.outcome.observed.insert_or_assign("execution_attempts", static_cast<std::int64_t>(attempts.size()));
    step.outcome.observed.insert_or_assign("repair_requested", true);
    step.outcome.observed.insert_or_assign("repair_generations", static_cast<std::int64_t>(generations.size()));
    step.outcome.observed.insert_or_assign("repair_budget_exhausted", exhausted);
    step.results.insert(step.results.end(), attempts.begin(), attempts.end() - 1);
    step.results.insert(step.results.end(), generations.begin(), generations.end());
    step.receipt = marker.receipt; step.artifact = step.receipt; step.outcome.evidence = step.receipt;
    applied(plan, action, step, state);
    return step;
}
} // namespace axiom::runtime
