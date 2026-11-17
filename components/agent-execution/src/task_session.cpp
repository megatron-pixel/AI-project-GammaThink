#include "task_session.hpp"
#include "summary_maintenance.hpp"

#include <algorithm>
#include <fcntl.h>
#include <json-c/json.h>
#include <sys/file.h>
#include <unistd.h>

namespace axiom::runtime {
namespace {
using knowledge::Error;
using knowledge::ErrorCode;
void require(bool condition, ErrorCode code, const char* message) {
    if (!condition) throw Error(code, message);
}
void binds(const ProblemSourceCatalog& catalog, const context::Message& message) {
    require(std::any_of(catalog.context().views.begin(), catalog.context().views.end(),
        [&](const auto& view) { return view.basis == PremiseBasis::Question && view.text == message.content; }),
        ErrorCode::Schema, "Task context must retain the complete original user question");
}
void binds_plan(const knowledge::TaskPlanContext& context, const context::Message& message) {
    const auto& view = context.sources().context().views.at(context.current_question());
    require(view.text == message.content &&
        view.archive_id == "private-conversation:" + evidence_digest(message.session) &&
        view.entry_path == "message:" + std::to_string(message.id), ErrorCode::Schema,
        "Task plan context does not capture this exact original conversation turn");
}
void check_action_receipt(knowledge::Journal& journal, const knowledge::ObjectId& manifest,
    const context::CompletedTaskAction& action, const knowledge::AdmittedTaskPlan& plan) {
    const auto definition = std::find_if(plan.actions().begin(), plan.actions().end(),
        [&](const auto& value) { return value.id == action.action; });
    require(definition != plan.actions().end(), ErrorCode::Integrity, "Recorded task action is not in the admitted plan");
    const auto format = definition->kind == knowledge::TaskActionKind::Explain
        ? "gammathink.task-native-response.v1"
        : definition->kind == knowledge::TaskActionKind::ExecuteCode
            ? definition->inputs.size() == 2 ? "gammathink.task-code-repair.v1" : "gammathink.task-code-operator.v1"
            : definition->kind == knowledge::TaskActionKind::GenerateCode
                ? "gammathink.task-code-generation.v1"
                : definition->kind == knowledge::TaskActionKind::CompileSource
                    ? "gammathink.task-source-compilation.v1"
                    : definition->kind == knowledge::TaskActionKind::Visualize ||
                        definition->kind == knowledge::TaskActionKind::RequestPublication
                        ? "gammathink.task-review-action.v1" : "gammathink.task-native-operator.v1";
    const auto receipt = journal.get(action.receipt);
    require(receipt && receipt->kind == knowledge::ObjectKind::TestResult &&
        receipt->visibility == knowledge::Visibility::Private &&
        (receipt->format == format ||
            (definition->kind == knowledge::TaskActionKind::CompileSource &&
             receipt->format == "gammathink.task-source-compilation.v2")) &&
        receipt->label == action.action &&
        std::any_of(receipt->links.begin(), receipt->links.end(), [&](const auto& link) {
            return link.relationship == knowledge::Relationship::DependsOn && link.target == manifest;
        }), ErrorCode::Integrity, "Recorded native action lost its exact private receipt");
    const auto assessment = journal.assessment(action.receipt);
    require(!assessment || (assessment->state != knowledge::KnowledgeState::Invalidated &&
        assessment->logical != knowledge::LogicalValidity::Invalid), ErrorCode::Integrity,
        "Recorded native action receipt requires explicit revalidation");
}
}

TaskSession::TaskSession(context::ConversationMemory& memory, context::SessionTasks& tasks,
    knowledge::Journal& journal, neural::NativeModelManager& model)
    : memory_(memory), tasks_(tasks), journal_(journal), model_(model) {
    require(tasks.path() != ":memory:", ErrorCode::Schema, "Session controller requires durable task state");
    const auto path = tasks.path().string() + ".execution.lock";
    const auto descriptor = open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    require(descriptor >= 0, ErrorCode::Storage, "Cannot open session execution lock");
    if (flock(descriptor, LOCK_EX | LOCK_NB) != 0) {
        close(descriptor);
        throw Error(ErrorCode::Permission, "Session task database already has an execution owner");
    }
    execution_lock_ = descriptor;
}
TaskSession::~TaskSession() { if (execution_lock_ >= 0) close(execution_lock_); }

context::Message TaskSession::source(std::string_view session, std::uint64_t user_message) {
    const auto message = memory_.get(session, user_message);
    require(message && message->role == context::Role::User, ErrorCode::Schema,
            "Task source is not an original user message in this session");
    const auto tail = memory_.recent(session, 1, knowledge::maximum_text_bytes);
    require(!tail.messages.empty() && tail.messages.back().id == user_message, ErrorCode::Schema,
            "Task source is not the current conversation turn");
    return *message;
}
SessionAcceptance TaskSession::accept(std::string_view session, std::uint64_t user_message,
    const ProblemSourceCatalog& catalog, std::string_view input_snapshot, std::string_view archive_dsl,
    std::uint64_t expected_revision) {
    try {
        binds(catalog, source(session, user_message));
        auto interpretation = knowledge::admit_task_dsl(catalog, input_snapshot, archive_dsl);
        if (const auto* decline = std::get_if<knowledge::TaskDecline>(&interpretation)) {
            model_.unload();
            return *decline;
        }
        const auto task = knowledge::persist_task(journal_, std::get<knowledge::AdmittedTask>(interpretation));
        const auto state = tasks_.begin(memory_, session, user_message, task.ordered_request, expected_revision);
        model_.unload();
        return state;
    } catch (...) {
        model_.unload();
        throw;
    }
}
PlanSessionAcceptance TaskSession::accept_plan(std::string_view session, std::uint64_t user_message,
    const knowledge::TaskPlanContext& context, std::string_view input_snapshot, std::string_view task_ir,
    std::uint64_t expected_revision) {
    try {
        binds_plan(context, source(session, user_message));
        require(task_context_pins_current(memory_, session, context), ErrorCode::Integrity,
                "Pinned conversation corrections changed before task admission");
        auto interpretation = knowledge::admit_task_plan(context, input_snapshot, task_ir);
        if (const auto* decline = std::get_if<knowledge::TaskPlanDecline>(&interpretation)) {
            model_.unload();
            return *decline;
        }
        const auto plan = knowledge::persist_task_plan(journal_, std::get<knowledge::AdmittedTaskPlan>(interpretation));
        const auto state = tasks_.begin(memory_, session, user_message, plan.manifest, expected_revision);
        model_.unload();
        return state;
    } catch (...) {
        model_.unload();
        throw;
    }
}
knowledge::AdmittedTaskPlan TaskSession::active_plan(std::string_view session, std::uint64_t expected_revision,
    const knowledge::TaskPlanContext& current_context) {
    return load_plan(session, expected_revision, current_context, true);
}
knowledge::AdmittedTaskPlan TaskSession::review_plan(std::string_view session, std::uint64_t expected_revision,
    const knowledge::TaskPlanContext& current_context) {
    return load_plan(session, expected_revision, current_context, false);
}
knowledge::AdmittedTaskPlan TaskSession::load_plan(std::string_view session, std::uint64_t expected_revision,
    const knowledge::TaskPlanContext& current_context, bool active_only) {
    require(model_.state() == neural::ModelState::Unloaded, ErrorCode::Permission,
            "Unload every neural model before activating a task plan");
    const auto state = tasks_.active(session);
    require(state && state->revision == expected_revision, ErrorCode::Schema, "Session task revision changed");
    require(active_only
        ? state->phase == context::TaskPhase::Interpreted || state->phase == context::TaskPhase::Unresolved ||
            state->phase == context::TaskPhase::Reasoning
        : state->phase != context::TaskPhase::Cancelled && state->phase != context::TaskPhase::Failed,
        ErrorCode::Schema, "Session plan is not active");
    const auto message = source(session, state->user_message);
    require(message.content_hash == state->message_hash, ErrorCode::Integrity, "Original task message changed");
    binds_plan(current_context, message);
    require(task_context_pins_current(memory_, session, current_context), ErrorCode::Integrity,
            "Pinned conversation corrections changed since task capture");
    auto plan = knowledge::restore_task_plan(journal_, state->ordered_task);
    require(plan.snapshot() == current_context.snapshot(), ErrorCode::Integrity, "Live task plan context changed");
    check_refinement_history(session, *state, plan);
    return plan;
}
PlanSessionAcceptance TaskSession::interpret_plan(std::string_view session,
    const neural::ModelSpec& interpreter, const ProblemContext& host_sources,
    std::uint64_t expected_revision, TaskInterpretationOptions options) {
    require(model_.state() == neural::ModelState::Unloaded, ErrorCode::Permission,
            "Interpretation requires an unloaded model manager");
    require(interpreter.role == neural::ModelRole::Interpreter, ErrorCode::Schema,
            "Task translation requires an interpreter model role");
    require(options.output_tokens > 0 && options.output_tokens < model_.limits().context_tokens,
            ErrorCode::Budget, "Invalid task interpretation output reservation");
    options.budget.input_tokens = std::min<std::size_t>(options.budget.input_tokens,
        model_.limits().context_tokens - options.output_tokens);
    options.budget.serialized_bytes = std::min(options.budget.serialized_bytes, model_.limits().input_bytes);
    const auto read = [&] {
        if (options.documents)
            return context::retrieve_document_context(memory_, *options.documents, session,
                options.instruction_messages, options.history_phrase, options.document_phrase, options.reads);
        require(options.document_phrase.empty(), ErrorCode::Schema, "Document retrieval requires a private context store");
        return context::retrieve_context(memory_, session, options.instruction_messages, options.history_phrase, options.reads);
    };
    const auto prepare = [&](const context::ContextSources& sources) {
        return prepare_task_prompt(sources, host_sources, [&](std::string_view input) {
            return model_.token_count(input);
        }, options.budget, options.objects, options.active_constraints, options.plan_limits, options.problem_limits);
    };
    try {
        if (options.summary_interval != 0) {
            context::SummaryMaintenanceLimits summary_limits;
            summary_limits.cadence_messages = options.summary_interval;
            context::refresh_summary(memory_, session, summary_limits);
        }
        const auto sources = read();
        model_.load(interpreter, task_translation_policy);
        const auto prompt = prepare(sources);
        const auto generated = model_.generate(prompt.input, options.output_tokens);
        require(generated.stop == neural::GenerationStop::EndOfGeneration, ErrorCode::Budget,
                "Task interpreter exhausted its output budget");
        require(generated.input_tokens == prompt.conversation.input_tokens, ErrorCode::Integrity,
                "Task interpreter token count changed");
        // Concurrent WAL writers may change the current turn or correction
        // revisions while the neural phase runs. Never activate that stale capture.
        const auto current = prepare(read());
        require(current.input == prompt.input, ErrorCode::Integrity,
                "Conversation context changed during task interpretation");
        return accept_plan(session, sources.current.id, prompt.context, prompt.context.snapshot(),
                           generated.text, expected_revision);
    } catch (...) {
        model_.unload();
        throw;
    }
}
TaskExecutionState TaskSession::restore_native_execution(std::string_view session, std::uint64_t expected_revision,
    const knowledge::TaskPlanContext& current_context, knowledge::UnifiedKnowledge& sources,
    const knowledge::GoalConditions& environment, const std::set<knowledge::ObjectId>& accepted,
    const std::map<knowledge::ObjectId, ProblemContext>& source_contexts, NativeTaskLimits limits) {
    return replay_native_execution(session, expected_revision, current_context, sources, environment,
        accepted, source_contexts, limits, true);
}
TaskExecutionState TaskSession::replay_native_execution(std::string_view session, std::uint64_t expected_revision,
    const knowledge::TaskPlanContext& current_context, knowledge::UnifiedKnowledge& sources,
    const knowledge::GoalConditions& environment, const std::set<knowledge::ObjectId>& accepted,
    const std::map<knowledge::ObjectId, ProblemContext>& source_contexts, NativeTaskLimits limits, bool active_only) {
    const auto plan = load_plan(session, expected_revision, current_context, active_only);
    const auto current = tasks_.active(session);
    require(current && current->revision == expected_revision, ErrorCode::Schema, "Session task revision changed");
    const auto actions = tasks_.completed_actions(session, current->ordered_task, expected_revision);
    TaskExecutionState state;
    NativeTaskOperators operators(sources, journal_, model_);
    for (const auto& action : actions) {
        check_action_receipt(journal_, current->ordered_task, action, plan);
        // Never copy a historical success/status flag. Current archive bytes,
        // assumptions and independently checked native certificates determine it.
        const auto definition = std::find_if(plan.actions().begin(), plan.actions().end(),
            [&](const auto& value) { return value.id == action.action; });
        if (definition->kind == knowledge::TaskActionKind::ExecuteCode)
            (void)restore_code_action(current_context, plan, *current, action, state, environment, accepted);
        else if (definition->kind == knowledge::TaskActionKind::GenerateCode)
            (void)restore_generation_action(current_context, plan, *current, action, state, environment, accepted);
        else if (definition->kind == knowledge::TaskActionKind::CompileSource)
            (void)restore_source_action(current_context, plan, *current, action, state, environment, accepted);
        else if (definition->kind == knowledge::TaskActionKind::Visualize ||
            definition->kind == knowledge::TaskActionKind::RequestPublication)
            (void)restore_review_action(current_context, plan, *current, action, state, sources, environment, accepted);
        else
            (void)operators.execute(current_context, plan, action.action, state, environment, accepted, source_contexts, limits);
        (void)load_plan(session, expected_revision, current_context, active_only);
    }
    (void)load_plan(session, expected_revision, current_context, active_only);
    for (const auto& action : actions) check_action_receipt(journal_, current->ordered_task, action, plan);
    return state;
}

context::Message TaskSession::deliver_native_response(std::string_view session, std::uint64_t expected_revision,
    const knowledge::TaskPlanContext& current_context, std::string_view action,
    knowledge::UnifiedKnowledge& sources, const knowledge::GoalConditions& environment,
    std::string_view timestamp, const std::set<knowledge::ObjectId>& accepted,
    const std::map<knowledge::ObjectId, ProblemContext>& source_contexts, NativeTaskLimits limits) {
    const auto plan = load_plan(session, expected_revision, current_context, false);
    const auto current = tasks_.active(session);
    require(current && current->revision == expected_revision, ErrorCode::Schema, "Session task revision changed");
    const auto original = source(session, current->user_message);
    const auto pins = memory_.constraints(session, 256);
    const auto definition = std::find_if(plan.actions().begin(), plan.actions().end(),
        [&](const auto& value) { return value.id == action; });
    require(definition != plan.actions().end() && definition->kind == knowledge::TaskActionKind::Explain,
        ErrorCode::Schema, "Response delivery requires an admitted explanation action");
    const auto state = replay_native_execution(session, expected_revision, current_context, sources,
        environment, accepted, source_contexts, limits, false);
    require(state.steps.contains(std::string(action)) && state.steps.at(std::string(action)).response,
        ErrorCode::Permission, "Response action has not completed");
    const auto& response = state.steps.at(std::string(action));
    using Json = std::unique_ptr<json_object, decltype(&json_object_put)>;
    Json body(json_object_new_object(), json_object_put);
    Json rendered(json_tokener_parse(response.response->c_str()), json_object_put);
    require(body && rendered, ErrorCode::Integrity, "Cannot assemble recorded deterministic response");
    const auto text = [&](const char* key, std::string_view value) {
        json_object_object_add(body.get(), key, json_object_new_string_len(value.data(), static_cast<int>(value.size())));
    };
    text("profile", "gammathink.conversation-response.v1");
    text("task", knowledge::hex(current->ordered_task)); text("receipt", knowledge::hex(response.receipt));
    text("source_message_hash", knowledge::hex({original.content_hash}));
    json_object_object_add(body.get(), "source_message", json_object_new_uint64(original.id));
    json_object_object_add(body.get(), "root_satisfied", json_object_new_boolean(
        knowledge::evaluate_task_plan(current_context, plan, state.outcomes, environment, state.artifacts, accepted)
            .goals.root_satisfied));
    json_object_object_add(body.get(), "result", rendered.release());
    const std::string content = json_object_to_json_string_ext(body.get(), JSON_C_TO_STRING_PLAIN);
    (void)load_plan(session, expected_revision, current_context, false);
    // Receipt persistence and conversation storage are separate transactions.
    // A failed delivery leaves private historical evidence, never a stale reply.
    return memory_.append_response(original, pins, timestamp, content);
}

std::vector<context::CompletedTaskAction> TaskSession::check_execution_state(std::string_view session,
    std::uint64_t expected_revision, const knowledge::AdmittedTaskPlan& plan, const TaskExecutionState& state) {
    const auto capture = knowledge::digest({
        reinterpret_cast<const std::uint8_t*>(plan.dsl().data()), plan.dsl().size()});
    require(!state.capture || *state.capture == capture, ErrorCode::Integrity, "Execution belongs to another task capture");
    require(state.capture || (state.outcomes.empty() && state.artifacts.empty() &&
        state.certificates.empty() && state.steps.empty()), ErrorCode::Integrity, "Unbound task execution state");
    const auto current = tasks_.active(session);
    require(current && current->revision == expected_revision, ErrorCode::Schema, "Session task revision changed");
    const auto completed = tasks_.completed_actions(session, current->ordered_task, expected_revision);
    require(state.outcomes.size() == completed.size() && state.steps.size() == completed.size(), ErrorCode::Integrity,
            "Restore recorded native actions before continuing the session");
    for (const auto& recorded : completed) {
        check_action_receipt(journal_, current->ordered_task, recorded, plan);
        const auto definition = std::find_if(plan.actions().begin(), plan.actions().end(),
            [&](const auto& value) { return value.id == recorded.action; });
        require(definition != plan.actions().end() && state.steps.contains(recorded.action) &&
            state.outcomes.contains(definition->goal) && state.outcomes.at(definition->goal).verified &&
            state.outcomes.at(definition->goal).evidence == state.steps.at(recorded.action).receipt &&
            state.artifacts.contains(definition->outputs.front()) &&
            state.artifacts.at(definition->outputs.front()) == state.steps.at(recorded.action).artifact, ErrorCode::Integrity,
            "Session action state does not match recorded operations");
    }
    return completed;
}

NativeTaskStep TaskSession::execute_native_action(std::string_view session, std::uint64_t expected_revision,
    const knowledge::TaskPlanContext& current_context, std::string_view action, TaskExecutionState& state,
    knowledge::UnifiedKnowledge& sources, const knowledge::GoalConditions& environment,
    const std::set<knowledge::ObjectId>& accepted, const std::map<knowledge::ObjectId, ProblemContext>& source_contexts,
    NativeTaskLimits limits) {
    const auto plan = active_plan(session, expected_revision, current_context);
    auto current = tasks_.active(session);
    const auto completed = check_execution_state(session, expected_revision, plan, state);
    if (current->phase != context::TaskPhase::Reasoning)
        current = tasks_.transition(session, current->revision, context::TaskPhase::Reasoning);
    try {
        auto pending = state;
        NativeTaskOperators operators(sources, journal_, model_);
        auto result = operators.execute(current_context, plan, action, pending, environment, accepted, source_contexts, limits);
        // A new user turn, correction or cancellation cannot commit a stale
        // operation into the live task. Historical private receipts stay intact.
        (void)active_plan(session, current->revision, current_context);
        for (const auto& recorded : completed) check_action_receipt(journal_, current->ordered_task, recorded, plan);
        const auto evaluated = knowledge::evaluate_task_plan(current_context, plan, pending.outcomes, environment, pending.artifacts, accepted);
        const auto& certificates = pending.certificates;
        const auto phase = evaluated.goals.root_satisfied ? context::TaskPhase::Checked
            : !evaluated.ready_actions.empty() ? context::TaskPhase::Unresolved
            : result.status == knowledge::KnowledgeRunStatus::Conflict ? context::TaskPhase::Conflict
            : context::TaskPhase::Unresolved;
        tasks_.transition(session, current->revision, phase, certificates,
            context::CompletedTaskAction{current->revision + 1, std::string(action), result.receipt});
        state = std::move(pending);
        return result;
    } catch (...) {
        const auto latest = tasks_.active(session);
        if (latest && latest->revision == current->revision)
            tasks_.transition(session, current->revision, context::TaskPhase::Failed);
        throw;
    }
}

knowledge::TaskRun TaskSession::execute(std::string_view session, std::uint64_t expected_revision,
    knowledge::NativeRunLimits limits, knowledge::NativeCoKLimits materialization) {
    require(model_.state() == neural::ModelState::Unloaded, ErrorCode::Permission,
            "Unload every neural model before native task execution");
    auto state = tasks_.active(session);
    require(state && state->revision == expected_revision, ErrorCode::Schema, "Session task revision changed");
    require(state->phase == context::TaskPhase::Interpreted || state->phase == context::TaskPhase::Unresolved ||
        state->phase == context::TaskPhase::Reasoning, ErrorCode::Schema, "Session task is not executable");
    const auto message = source(session, state->user_message);
    require(message.content_hash == state->message_hash, ErrorCode::Integrity, "Original task message changed");
    const auto admitted = knowledge::restore_task(journal_, state->ordered_task);
    binds(ProblemSourceCatalog(admitted.request().first.context, admitted.request().first.limits), message);
    // A Reasoning record can outlive a crashed process. The OS-released lock
    // excludes another live controller; checked replay makes restart idempotent.
    if (state->phase != context::TaskPhase::Reasoning) {
        state = tasks_.transition(session, state->revision, context::TaskPhase::Reasoning);
    }
    try {
        auto result = knowledge::execute_task(journal_, admitted, limits, materialization);
        (void)source(session, state->user_message);
        std::vector<knowledge::ObjectId> certificates;
        bool conflict = false;
        for (const auto& slot : result.slots) {
            if (slot.execution.certificate) certificates.push_back(*slot.execution.certificate);
            conflict = conflict || slot.execution.check.conflict;
        }
        const auto phase = conflict ? context::TaskPhase::Conflict
            : result.evaluation.root_satisfied ? context::TaskPhase::Checked : context::TaskPhase::Unresolved;
        tasks_.transition(session, state->revision, phase, certificates);
        return result;
    } catch (...) {
        const auto current = tasks_.active(session);
        if (current && current->revision == state->revision) {
            tasks_.transition(session, state->revision, context::TaskPhase::Failed);
        }
        throw;
    }
}

} // namespace axiom::runtime
