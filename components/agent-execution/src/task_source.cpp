#include "task_session.hpp"
#include "archive_dsl/codec.hpp"

#include <algorithm>
#include <json-c/json.h>

namespace axiom::runtime {
namespace {
using namespace knowledge;
using Json = std::unique_ptr<json_object, decltype(&json_object_put)>;
void require(bool condition, ErrorCode code, const char* message) {
    if (!condition) throw Error(code, message);
}
void text(json_object* object, const char* key, std::string_view value) {
    json_object_object_add(object, key, json_object_new_string_len(value.data(), static_cast<int>(value.size())));
}
std::string_view source_policy(SourceTranslationProfile profile) {
    switch (profile) {
    case SourceTranslationProfile::V1: return source_translation_policy;
    case SourceTranslationProfile::QuantifiedV2: return source_translation_policy_v2;
    }
    throw Error(ErrorCode::Schema, "Unknown source translation profile");
}
std::string source_format(SourceTranslationProfile profile, std::string_view kind) {
    return "gammathink.source-translation-" + std::string(kind) + ".v" +
        std::to_string(static_cast<int>(profile));
}
const TaskAction& source_action(const AdmittedTaskPlan& plan, std::string_view id) {
    const auto found = std::find_if(plan.actions().begin(), plan.actions().end(),
        [&](const auto& value) { return value.id == id; });
    require(found != plan.actions().end() && found->kind == TaskActionKind::CompileSource,
        ErrorCode::Schema, "Expected an admitted compile-source action");
    return *found;
}
void ready(const TaskPlanContext& context, const AdmittedTaskPlan& plan, const TaskAction& action,
    const TaskExecutionState& state, const GoalConditions& environment, const std::set<ObjectId>& accepted) {
    const auto evaluation = evaluate_task_plan(context, plan, state.outcomes, environment, state.artifacts, accepted);
    require(std::find(evaluation.ready_actions.begin(), evaluation.ready_actions.end(), action.id) !=
        evaluation.ready_actions.end(), ErrorCode::Permission, "Source compilation prerequisites or policy are not ready");
}
Object current(Journal& journal, const ObjectId& id) {
    const auto assessment = journal.assessment(id);
    require(!assessment || (assessment->state != KnowledgeState::Invalidated &&
        assessment->logical != LogicalValidity::Invalid), ErrorCode::Integrity,
        "Source compilation dependency requires explicit revalidation");
    const auto value = journal.get(id);
    require(value.has_value(), ErrorCode::Missing, "Source compilation dependency is unavailable");
    return *value;
}
ObjectId source_identity(const AdmittedTaskPlan& plan, const TaskAction& action, const TaskExecutionState& state) {
    const auto entity = std::find_if(plan.entities().begin(), plan.entities().end(),
        [&](const auto& value) { return value.id == action.inputs.front(); });
    require(entity != plan.entities().end(), ErrorCode::Integrity, "Source action lost its input entity");
    if (const auto* value = std::get_if<TaskObject>(&entity->value)) return value->id;
    require(std::holds_alternative<TaskArtifact>(entity->value) && state.artifacts.contains(entity->id),
        ErrorCode::Integrity, "Source action has no produced input artifact");
    return state.artifacts.at(entity->id);
}
struct Prepared {
    std::vector<Object> records;
    NativeTaskStep step;
};
Object complete_source(Journal& journal, const ObjectId& source_id, const TaskSourceCompilation& input) {
    require(source_id == input.translation.source, ErrorCode::Integrity, "Translation selected another source");
    require(input.storage.objects >= 2 && input.storage.objects <= 512 &&
        input.storage.encoded_bytes > 0 && input.storage.encoded_bytes <= 64 * 1024 * 1024,
        ErrorCode::Budget, "Invalid complete source action budget");
    require(input.selectors == ProblemSelectorMode::OffsetV1 || input.selectors == ProblemSelectorMode::QuoteV1,
        ErrorCode::Schema, "Unknown source selector profile");
    (void)source_policy(input.profile);
    const auto source = current(journal, source_id);
    require(identify(source) == source_id && source.kind == ObjectKind::Evidence &&
        source.format.starts_with("source.zim.raw.v1:") && source.sources.size() == 1,
        ErrorCode::Integrity, "Translation requires the exact complete raw source record");
    const auto& reference = source.sources.front();
    require(reference.extractor_version == "libzim-raw-entry-v1" && reference.offset == 0 &&
        reference.length == source.payload.size() && reference.source_digest == digest(source.payload),
        ErrorCode::Integrity, "Translation source is not a complete raw entry");
    require(input.context.views.size() == 1, ErrorCode::Schema, "Source action requires one complete source view");
    const auto& view = input.context.views.front();
    require(view.basis == PremiseBasis::Source && view.archive_id == reference.archive_id &&
        view.entry_path == reference.entry_path && view.extractor_version == reference.extractor_version &&
        view.text == std::string(source.payload.begin(), source.payload.end()) &&
        view.digest == evidence_digest(view.text), ErrorCode::Integrity, "Source action changed complete source bytes or provenance");
    const ProblemSourceCatalog catalog(input.context);
    require(catalog.snapshot() == input.translation.context_snapshot, ErrorCode::Integrity,
        "Source translation host snapshot changed");
    require(encode_object(source).size() < input.storage.encoded_bytes, ErrorCode::Budget,
        "Complete source exceeds action byte budget");
    return source;
}
struct TranslationOrigin {
    std::string archive_dsl;
    std::vector<Object> records;
};
TranslationOrigin translation_origin(Journal& journal, const AdmittedTaskPlan& plan, const TaskAction& action,
    const ObjectId& manifest, const ObjectId& source_id, const TaskSourceCompilation& input);
Prepared prepare(Journal& journal, const AdmittedTaskPlan& plan, const TaskAction& action,
    const ObjectId& manifest, const ObjectId& source_id, const TaskSourceCompilation& input) {
    const auto source = complete_source(journal, source_id, input);
    auto origin = translation_origin(journal, plan, action, manifest, source_id, input);
    const auto compilation = compile_source(source, source_id, origin.archive_dsl);
    Prepared result;
    result.records = std::move(origin.records);
    std::optional<ObjectId> context, module;
    std::vector<ObjectId> members;
    const auto raw_bytes = encode_object(source).size();
    std::size_t initial_bytes = raw_bytes;
    for (const auto& value : result.records) {
        const auto bytes = encode_object(value).size();
        require(bytes < input.storage.encoded_bytes - initial_bytes, ErrorCode::Budget, "Translation provenance exceeds source budget");
        initial_bytes += bytes;
    }
    if (compilation.kind == SourceCompilationKind::Candidate) {
        require(input.storage.objects >= 4 + result.records.size(), ErrorCode::Budget, "Source action cannot hold its scoped module");
        const auto compiled = materialize_source(source, *compilation.candidate, input.context,
            {input.storage.objects - 2 - result.records.size(), input.storage.encoded_bytes - initial_bytes}, {}, input.selectors);
        context = identify(compiled.context); module = identify(compiled.module);
        result.records.push_back(compiled.context); result.records.push_back(compiled.module);
        for (const auto& member : compiled.members) {
            members.push_back(identify(member)); result.records.push_back(member);
        }
    }
    Json payload(json_object_new_object(), json_object_put);
    require(bool(payload), ErrorCode::Storage, "Cannot allocate source compilation receipt");
    text(payload.get(), "manifest", hex(manifest)); text(payload.get(), "action", action.id);
    text(payload.get(), "goal", action.goal); text(payload.get(), "task_ir", plan.dsl());
    text(payload.get(), "source", hex(source_id)); text(payload.get(), "host_context", serialize_problem_context(input.context));
    text(payload.get(), "context_snapshot", input.translation.context_snapshot);
    text(payload.get(), "archive_dsl", origin.archive_dsl);
    if (input.candidate) text(payload.get(), "translation_candidate", hex(*input.candidate));
    text(payload.get(), "reason", compilation.reason);
    json_object_object_add(payload.get(), "kind", json_object_new_int(static_cast<int>(compilation.kind)));
    json_object_object_add(payload.get(), "selectors", json_object_new_int(static_cast<int>(input.selectors)));
    json_object_object_add(payload.get(), "object_budget", json_object_new_uint64(input.storage.objects));
    json_object_object_add(payload.get(), "byte_budget", json_object_new_uint64(input.storage.encoded_bytes));
    json_object_object_add(payload.get(), "source_translation_qualified", json_object_new_boolean(false));
    json_object_object_add(payload.get(), "empirical_truth_verified", json_object_new_boolean(false));
    json_object_object_add(payload.get(), "module", module ? json_object_new_string(hex(*module).c_str()) : nullptr);
    json_object_object_add(payload.get(), "context", context ? json_object_new_string(hex(*context).c_str()) : nullptr);
    auto* member_ids = json_object_new_array(); json_object_object_add(payload.get(), "members", member_ids);
    for (const auto& id : members) json_object_array_add(member_ids, json_object_new_string(hex(id).c_str()));
    Object receipt;
    receipt.kind = ObjectKind::TestResult; receipt.label = action.id;
    receipt.format = input.profile == SourceTranslationProfile::QuantifiedV2
        ? "gammathink.task-source-compilation.v2" : "gammathink.task-source-compilation.v1";
    const std::string encoded = json_object_to_json_string_ext(payload.get(), JSON_C_TO_STRING_PLAIN);
    receipt.payload.assign(encoded.begin(), encoded.end()); receipt.sources = source.sources;
    receipt.links = {{Relationship::DependsOn, manifest}, {Relationship::DependsOn, source_id}};
    for (const auto& record : result.records) receipt.links.push_back({Relationship::DependsOn, identify(record)});
    const auto receipt_id = identify(receipt);
    result.records.push_back(std::move(receipt));
    require(result.records.size() + 1 <= input.storage.objects, ErrorCode::Budget, "Source action object budget exceeded");
    std::size_t used = raw_bytes;
    for (const auto& record : result.records) {
        const auto bytes = encode_object(record).size();
        require(bytes <= input.storage.encoded_bytes - used, ErrorCode::Budget, "Source action byte budget exceeded");
        used += bytes;
    }
    auto& step = result.step;
    step.receipt = receipt_id; step.artifact = module.value_or(receipt_id);
    step.status = module ? KnowledgeRunStatus::Checked : KnowledgeRunStatus::Unresolved;
    step.outcome.verified = true; step.outcome.succeeded = module.has_value(); step.outcome.evidence = receipt_id;
    step.outcome.observed = {{"source_compilation_kind", static_cast<std::int64_t>(compilation.kind)},
        {"source_materialized", module.has_value()}, {"source_translation_qualified", false},
        {"empirical_truth_verified", false}, {"native_checked", false}, {"source_raw", hex(source_id)},
        {"source_record_restored", false}, {"source_records", static_cast<std::int64_t>(result.records.size() + 1)},
        {"source_encoded_bytes", static_cast<std::int64_t>(used)}, {"source_reason", compilation.reason}};
    if (input.candidate) step.outcome.observed.emplace("source_model_candidate", hex(*input.candidate));
    if (module) {
        step.outcome.observed.emplace("source_module", hex(*module));
        step.outcome.observed.emplace("source_context", hex(*context));
        step.outcome.observed.emplace("source_members", static_cast<std::int64_t>(members.size()));
    }
    return result;
}
void apply(const AdmittedTaskPlan& plan, const TaskAction& action, const NativeTaskStep& step, TaskExecutionState& state) {
    state.capture = digest({reinterpret_cast<const std::uint8_t*>(plan.dsl().data()), plan.dsl().size()});
    state.outcomes[action.goal] = step.outcome;
    state.artifacts[action.outputs.front()] = step.artifact;
    state.steps[action.id] = step;
}
Json document(const Object& object) {
    require(object.payload.size() <= 16 * 1024 * 1024, ErrorCode::Budget, "Source receipt exceeds byte budget");
    std::unique_ptr<json_tokener, decltype(&json_tokener_free)> parser(json_tokener_new_ex(64), json_tokener_free);
    require(bool(parser), ErrorCode::Storage, "Cannot allocate source receipt parser");
    json_tokener_set_flags(parser.get(), JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
    Json value(json_tokener_parse_ex(parser.get(), reinterpret_cast<const char*>(object.payload.data()),
        static_cast<int>(object.payload.size())), json_object_put);
    require(value && json_tokener_get_error(parser.get()) == json_tokener_success &&
        json_tokener_get_parse_end(parser.get()) == object.payload.size() &&
        json_object_is_type(value.get(), json_type_object), ErrorCode::Integrity, "Invalid source receipt");
    return value;
}
std::string string(json_object* root, const char* key) {
    auto* value = json_object_object_get(root, key);
    require(value && json_object_is_type(value, json_type_string), ErrorCode::Integrity, "Missing source receipt text");
    return std::string(json_object_get_string(value), static_cast<std::size_t>(json_object_get_string_len(value)));
}
std::size_t number(json_object* root, const char* key, std::size_t maximum) {
    auto* value = json_object_object_get(root, key);
    require(value && json_object_is_type(value, json_type_int) && json_object_get_int64(value) >= 0 &&
        json_object_get_uint64(value) <= maximum, ErrorCode::Integrity, "Invalid source receipt budget");
    return static_cast<std::size_t>(json_object_get_uint64(value));
}
std::string source_prompt(const TaskSourceCompilation& input) {
    (void)source_policy(input.profile);
    Json value(json_object_new_object(), json_object_put);
    text(value.get(), "profile", source_format(input.profile, "input"));
    text(value.get(), "source", hex(input.translation.source));
    text(value.get(), "context_snapshot", input.translation.context_snapshot);
    text(value.get(), "source_context", serialize_problem_context(input.context));
    json_object_object_add(value.get(), "selectors", json_object_new_int(static_cast<int>(input.selectors)));
    text(value.get(), "archive_roles", input.profile == SourceTranslationProfile::QuantifiedV2
        ? "ENTITY=entities; QUANTITY=quantities; NUMBER=numbers; BINDING=bindings; NODE=nodes; "
          "EQUATION=equations; PREDICATE=predicates; RULE=rules; QUANTIFIED_EQUATION=quantified_equations. "
          "Exactly these nine theory arrays. Each declaration appends its JSON body in array order. "
          "GOAL is forbidden. CLARIFY or UNSUPPORTED instead has one JSON string body and no declarations."
        :
        "ENTITY=entities; QUANTITY=quantities; NUMBER=numbers; BINDING=bindings; NODE=nodes; "
        "EQUATION=equations; PREDICATE=predicates; RULE=rules. Exactly these eight existing theory arrays. "
        "Each declaration appends its JSON body in array order. GOAL is forbidden. "
        "CLARIFY or UNSUPPORTED instead has one JSON string body and no declarations.");
    std::string tuples =
        "ENTITY is a string. scope=[resolved,sorted qualifiers]. "
        "QUANTITY=[property,entity,scope,unit_index]. "
        "NUMBER=[numeral_span,unit_span_or_null,unit_index,precision,qualifier_span_or_null]. "
        "BINDING=[quantity_index,number_index,[supports]]. "
        "NODE=[\"q\",quantity_index] or [\"n\",number_index] or [\"neg\",earlier_node] or "
        "[\"add\"|\"sub\"|\"mul\"|\"div\",earlier_node,earlier_node]. "
        "EQUATION=[left_node,right_node,scope,guard_or_null,[supports]]. "
        "PREDICATE=[name,arity]. atom=[predicate_index,negative,[terms]]. "
        "term=[\"c\",entity_index] or [\"v\",variable_index]. "
        "RULE=[variable_count,head_atom,[premise_atoms],[supports]], range-restricted. "
        "A zero-premise,zero-variable rule records an explicit source assertion, not an inference. "
        "Copy numeral text into spans; never calculate or normalize numeric answers. "
        "Units, scales, source views and registries come only from source_context.";
    if (input.profile == SourceTranslationProfile::QuantifiedV2)
        tuples += " QUANTIFIED_EQUATION=[variable_count,[quantity_templates],[local_nodes],left_node,right_node,"
            "scope,guard_or_null,[supports]]. Bind 1..4 universal variables. "
            "quantity_template=[property,term,scope,unit_index]. Local nodes use the NODE shapes, "
            "but q indexes quantity_templates and n indexes the shared numbers array. "
            "Every variable term must be bound by this declaration. Preserve guards and source citations. "
            "Emit the universal declaration, not ground instances, answers or inferred facts.";
    text(value.get(), "tuple_contract", tuples);
    text(value.get(), "citation_contract", input.selectors == ProblemSelectorMode::QuoteV1
        ? "quote-v1 spans=[view_index,locator_or_null,exact_quote,enclosing_quote_or_null]. "
          "Copy exact original bytes and case. The quote must uniquely match its view/locator; "
          "an exact unique enclosure may disambiguate. Do not use offsets."
        : "offset-v1 spans=[view_index,UTF8_byte_offset,UTF8_byte_length,locator_or_null]. Cite exact original bytes.");
    return json_object_to_json_string_ext(value.get(), JSON_C_TO_STRING_PLAIN);
}
Object translation_record(const Object& source, const ObjectId& manifest, const ObjectId& source_id,
    std::string_view format, std::string_view payload) {
    Object result; result.kind = ObjectKind::Evidence; result.label = std::string(format); result.format = std::string(format);
    result.payload.assign(payload.begin(), payload.end()); result.sources = source.sources;
    result.links = {{Relationship::DependsOn, manifest}, {Relationship::DependsOn, source_id}};
    return result;
}
Object source_model_record(const Object& source, const ObjectId& manifest, const ObjectId& source_id,
    SourceTranslationProfile profile,
    const neural::ModelSpec& model, const neural::ModelMetrics& metrics, std::size_t context_tokens, std::size_t output_tokens) {
    Json value(json_object_new_object(), json_object_put);
    text(value.get(), "id", model.id); text(value.get(), "path", model.path.string());
    text(value.get(), "sha256", hex(ObjectId{model.expected_sha256})); text(value.get(), "system_policy", source_policy(profile));
    json_object_object_add(value.get(), "role", json_object_new_int(static_cast<int>(model.role)));
    json_object_object_add(value.get(), "enable_thinking", json_object_new_boolean(model.enable_thinking));
    json_object_object_add(value.get(), "file_bytes", json_object_new_uint64(metrics.file_bytes));
    json_object_object_add(value.get(), "retained_parameters", json_object_new_uint64(metrics.retained_parameters));
    json_object_object_add(value.get(), "runtime_parameters", json_object_new_uint64(metrics.runtime_parameters));
    json_object_object_add(value.get(), "sampled_peak_rss", json_object_new_uint64(metrics.sampled_peak_rss));
    json_object_object_add(value.get(), "unloaded_rss", json_object_new_uint64(metrics.unloaded_rss));
    json_object_object_add(value.get(), "context_tokens", json_object_new_uint64(context_tokens));
    json_object_object_add(value.get(), "output_tokens", json_object_new_uint64(output_tokens));
    return translation_record(source, manifest, source_id, source_format(profile, "model"),
        json_object_to_json_string_ext(value.get(), JSON_C_TO_STRING_PLAIN));
}
Object source_candidate_record(const Object& source, const AdmittedTaskPlan& plan, const TaskAction& action,
    const ObjectId& manifest, const TaskSourceCompilation& input, const ObjectId& prompt, const ObjectId& model,
    const neural::Generation& generation) {
    Json value(json_object_new_object(), json_object_put);
    text(value.get(), "manifest", hex(manifest)); text(value.get(), "action", action.id); text(value.get(), "goal", action.goal);
    text(value.get(), "task_snapshot", plan.snapshot()); text(value.get(), "source", hex(input.translation.source));
    text(value.get(), "source_context", serialize_problem_context(input.context));
    text(value.get(), "context_snapshot", input.translation.context_snapshot);
    text(value.get(), "prompt", hex(prompt)); text(value.get(), "model", hex(model));
    text(value.get(), "archive_dsl", generation.text);
    json_object_object_add(value.get(), "selectors", json_object_new_int(static_cast<int>(input.selectors)));
    json_object_object_add(value.get(), "input_tokens", json_object_new_uint64(generation.input_tokens));
    json_object_object_add(value.get(), "output_tokens", json_object_new_uint64(generation.output_tokens));
    json_object_object_add(value.get(), "stop", json_object_new_int(static_cast<int>(generation.stop)));
    json_object_object_add(value.get(), "source_translation_qualified", json_object_new_boolean(false));
    json_object_object_add(value.get(), "empirical_truth_verified", json_object_new_boolean(false));
    auto result = translation_record(source, manifest, input.translation.source, source_format(input.profile, "candidate"),
        json_object_to_json_string_ext(value.get(), JSON_C_TO_STRING_PLAIN));
    result.links.push_back({Relationship::DependsOn, prompt}); result.links.push_back({Relationship::DependsOn, model});
    return result;
}
TranslationOrigin translation_origin(Journal& journal, const AdmittedTaskPlan& plan, const TaskAction& action,
    const ObjectId& manifest, const ObjectId& source_id, const TaskSourceCompilation& input) {
    if (!input.candidate) return {input.translation.archive_dsl, {}};
    const auto source = current(journal, source_id);
    const auto candidate = current(journal, *input.candidate);
    require(candidate.kind == ObjectKind::Evidence && candidate.visibility == Visibility::Private &&
        candidate.format == source_format(input.profile, "candidate"), ErrorCode::Integrity, "Wrong source candidate profile");
    const auto fields = document(candidate);
    const auto prompt = current(journal, parse_id(string(fields.get(), "prompt")));
    const auto model = current(journal, parse_id(string(fields.get(), "model")));
    require(prompt == translation_record(source, manifest, source_id, source_format(input.profile, "prompt"), source_prompt(input)),
        ErrorCode::Integrity, "Source candidate changed its complete prompt or source context");
    require(model.kind == ObjectKind::Evidence && model.visibility == Visibility::Private &&
        model.format == source_format(input.profile, "model"), ErrorCode::Integrity, "Wrong source model record profile");
    const auto metadata = document(model);
    auto* thinking = json_object_object_get(metadata.get(), "enable_thinking");
    require(thinking && json_object_is_type(thinking, json_type_boolean), ErrorCode::Integrity, "Invalid source deliberation flag");
    neural::ModelSpec identity{string(metadata.get(), "id"), string(metadata.get(), "path"),
        parse_id(string(metadata.get(), "sha256")).digest, neural::ModelRole::KnowledgeExtraction, bool(json_object_get_boolean(thinking))};
    neural::ModelMetrics metrics;
    metrics.file_bytes = number(metadata.get(), "file_bytes", 2999999999ULL);
    metrics.retained_parameters = number(metadata.get(), "retained_parameters", INT64_MAX);
    metrics.runtime_parameters = number(metadata.get(), "runtime_parameters", INT64_MAX);
    metrics.sampled_peak_rss = number(metadata.get(), "sampled_peak_rss", 2999999999ULL);
    metrics.unloaded_rss = number(metadata.get(), "unloaded_rss", 2999999999ULL);
    const auto context_tokens = number(metadata.get(), "context_tokens", 32768);
    const auto output_tokens = number(metadata.get(), "output_tokens", 32767);
    require(metrics.file_bytes > 0 && metrics.retained_parameters > 0 &&
        metrics.runtime_parameters == metrics.retained_parameters && metrics.sampled_peak_rss > 0 &&
        metrics.unloaded_rss > 0 && output_tokens > 0 && context_tokens > output_tokens &&
        model == source_model_record(source, manifest, source_id, input.profile, identity, metrics, context_tokens, output_tokens),
        ErrorCode::Integrity, "Source model record changed its verified role, resource counts or policy");
    neural::Generation generation{string(fields.get(), "archive_dsl"),
        static_cast<neural::GenerationStop>(number(fields.get(), "stop", 1)),
        static_cast<std::uint32_t>(number(fields.get(), "input_tokens", context_tokens - output_tokens)),
        static_cast<std::uint32_t>(number(fields.get(), "output_tokens", output_tokens))};
    require(generation.input_tokens > 0 && generation.stop == neural::GenerationStop::EndOfGeneration,
        ErrorCode::Budget, "Incomplete source generation cannot be compiled");
    require(candidate == source_candidate_record(source, plan, action, manifest, input, identify(prompt), identify(model), generation),
        ErrorCode::Integrity, "Source candidate changed its task scope or raw output");
    require(input.translation.archive_dsl.empty() || input.translation.archive_dsl == generation.text,
        ErrorCode::Integrity, "Selected source candidate output was rewritten");
    try {
        if (input.profile == SourceTranslationProfile::QuantifiedV2)
            (void)gammathink::archive_dsl::decode_v2(generation.text);
        else (void)gammathink::archive_dsl::decode(generation.text);
    } catch (const std::runtime_error& error) {
        throw Error(ErrorCode::Schema, error.what());
    }
    return {generation.text, {prompt, model, candidate}};
}
}

std::string source_translation_input(const TaskSourceCompilation& input) { return source_prompt(input); }

SourceTranslationCandidate TaskSession::translate_source_action(std::string_view session, std::uint64_t revision,
    const TaskPlanContext& context, std::string_view action_id, TaskExecutionState& state,
    const GoalConditions& environment, const TaskSourceCompilation& input, const neural::ModelSpec& model,
    const std::set<ObjectId>& accepted, SourceTranslationOptions options) {
    require(model_.state() == neural::ModelState::Unloaded, ErrorCode::Permission, "Source translation requires an unloaded model");
    require(model.role == neural::ModelRole::KnowledgeExtraction, ErrorCode::Schema, "Source translation requires its explicit model role");
    require(!input.candidate && input.translation.archive_dsl.empty(), ErrorCode::Schema, "Source translation does not accept an answer candidate");
    require(options.output_tokens > 0 && options.output_tokens < model_.limits().context_tokens,
        ErrorCode::Budget, "Invalid source translation output budget");
    const auto plan = active_plan(session, revision, context);
    (void)check_execution_state(session, revision, plan, state);
    const auto& action = source_action(plan, action_id); ready(context, plan, action, state, environment, accepted);
    const auto source_id = source_identity(plan, action, state);
    const auto source = complete_source(journal_, source_id, input);
    require(input.storage.objects >= 4, ErrorCode::Budget, "Source translation provenance exceeds object budget");
    const auto manifest = tasks_.active(session)->ordered_task;
    const auto body = source_prompt(input);
    neural::Generation generation;
    try {
        model_.load(model, source_policy(input.profile));
        const auto counted = model_.token_count(body);
        require(counted <= model_.limits().context_tokens - options.output_tokens,
            ErrorCode::Budget, "Complete source and native template exceed the context budget");
        generation = model_.generate(body, options.output_tokens);
        require(generation.input_tokens == counted, ErrorCode::Integrity, "Source translation changed its counted native prompt");
        model_.unload();
    } catch (...) { model_.unload(); throw; }
    const auto& metrics = model_.metrics();
    (void)active_plan(session, revision, context);
    (void)check_execution_state(session, revision, plan, state);
    require(source == complete_source(journal_, source_id, input), ErrorCode::Integrity, "Source changed during translation");
    const auto prompt = translation_record(source, manifest, source_id, source_format(input.profile, "prompt"), body);
    const auto identity = source_model_record(source, manifest, source_id, input.profile, model, metrics, model_.limits().context_tokens, options.output_tokens);
    const auto candidate = source_candidate_record(source, plan, action, manifest, input, identify(prompt), identify(identity), generation);
    std::size_t used = encode_object(source).size();
    for (const auto* value : {&prompt, &identity, &candidate}) {
        const auto bytes = encode_object(*value).size();
        require(bytes <= input.storage.encoded_bytes - used, ErrorCode::Budget, "Source translation provenance exceeds byte budget");
        used += bytes;
    }
    journal_.put(prompt); journal_.put(identity); journal_.put(candidate);
    return {identify(candidate), identify(prompt), identify(identity), generation.text, generation.input_tokens, generation.output_tokens,
        generation.stop == neural::GenerationStop::EndOfGeneration};
}

NativeTaskStep TaskSession::compile_source_action(std::string_view session, std::uint64_t revision,
    const TaskPlanContext& context, std::string_view action_id, TaskExecutionState& state,
    const GoalConditions& environment, const TaskSourceCompilation& input, const std::set<ObjectId>& accepted) {
    require(model_.state() == neural::ModelState::Unloaded, ErrorCode::Permission, "Unload model before source compilation");
    const auto plan = active_plan(session, revision, context);
    const auto& action = source_action(plan, action_id);
    (void)check_execution_state(session, revision, plan, state);
    ready(context, plan, action, state, environment, accepted);
    auto task = tasks_.active(session);
    require(task && task->revision == revision, ErrorCode::Schema, "Session task revision changed");
    const auto prepared = prepare(journal_, plan, action, task->ordered_task, source_identity(plan, action, state), input);
    (void)active_plan(session, revision, context);
    if (task->phase != context::TaskPhase::Reasoning)
        task = tasks_.transition(session, revision, context::TaskPhase::Reasoning);
    for (const auto& record : prepared.records) journal_.put(record);
    auto pending = state; apply(plan, action, prepared.step, pending);
    const auto evaluation = evaluate_task_plan(context, plan, pending.outcomes, environment, pending.artifacts, accepted);
    tasks_.transition(session, task->revision,
        evaluation.goals.root_satisfied ? context::TaskPhase::Checked : context::TaskPhase::Unresolved,
        pending.certificates, context::CompletedTaskAction{task->revision + 1, action.id, prepared.step.receipt});
    state = std::move(pending);
    return prepared.step;
}

NativeTaskStep TaskSession::restore_source_action(const TaskPlanContext& context, const AdmittedTaskPlan& plan,
    const context::SessionTask& task, const context::CompletedTaskAction& marker, TaskExecutionState& state,
    const GoalConditions& environment, const std::set<ObjectId>& accepted) {
    const auto& action = source_action(plan, marker.action);
    ready(context, plan, action, state, environment, accepted);
    const auto receipt = current(journal_, marker.receipt);
    require(receipt.kind == ObjectKind::TestResult && receipt.visibility == Visibility::Private &&
        (receipt.format == "gammathink.task-source-compilation.v1" ||
         receipt.format == "gammathink.task-source-compilation.v2"), ErrorCode::Integrity, "Wrong source receipt profile");
    const auto fields = document(receipt);
    TaskSourceCompilation input;
    input.profile = receipt.format == "gammathink.task-source-compilation.v2"
        ? SourceTranslationProfile::QuantifiedV2 : SourceTranslationProfile::V1;
    input.context = parse_problem_context(string(fields.get(), "host_context"));
    input.translation = {parse_id(string(fields.get(), "source")), string(fields.get(), "context_snapshot"),
        string(fields.get(), "archive_dsl")};
    input.selectors = static_cast<ProblemSelectorMode>(number(fields.get(), "selectors", 2));
    input.storage.objects = number(fields.get(), "object_budget", 512);
    input.storage.encoded_bytes = number(fields.get(), "byte_budget", 64 * 1024 * 1024);
    if (auto* candidate = json_object_object_get(fields.get(), "translation_candidate")) {
        require(json_object_is_type(candidate, json_type_string), ErrorCode::Integrity, "Invalid source candidate receipt identity");
        input.candidate = parse_id(json_object_get_string(candidate));
    }
    auto prepared = prepare(journal_, plan, action, task.ordered_task, source_identity(plan, action, state), input);
    require(prepared.step.receipt == marker.receipt, ErrorCode::Integrity, "Source receipt differs from checked compilation");
    for (const auto& expected : prepared.records) {
        const auto actual = current(journal_, identify(expected));
        require(actual == expected, ErrorCode::Integrity, "Source compilation lost a scoped record");
    }
    prepared.step.outcome.observed["source_record_restored"] = true;
    apply(plan, action, prepared.step, state);
    return prepared.step;
}

} // namespace axiom::runtime
