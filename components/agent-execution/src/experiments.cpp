#include "experiments.hpp"
#include "task_plan.hpp"

#include <algorithm>
#include <json-c/json.h>
#include <openssl/rand.h>

namespace axiom::knowledge {
namespace {
using Json = std::unique_ptr<json_object, decltype(&json_object_put)>;
constexpr std::size_t byte_limit = 16 * 1024 * 1024;
constexpr std::size_t receipt_reserve = 16384;
void require(bool value, ErrorCode code, const char* message) {
    if (!value) throw Error(code, message);
}
void put_text(json_object* value, const char* key, std::string_view text) {
    json_object_object_add(value, key, json_object_new_string_len(text.data(), static_cast<int>(text.size())));
}
void put_number(json_object* value, const char* key, std::uint64_t number) {
    json_object_object_add(value, key, json_object_new_uint64(number));
}
json_object* scalar(const GoalValue& value) {
    return std::visit([](const auto& item) -> json_object* {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T, bool>) return json_object_new_boolean(item);
        else if constexpr (std::is_same_v<T, std::int64_t>) return json_object_new_int64(item);
        else return json_object_new_string_len(item.data(), static_cast<int>(item.size()));
    }, value);
}
std::string wire(json_object* value) {
    return json_object_to_json_string_ext(value, JSON_C_TO_STRING_PLAIN);
}
Json parse(std::span<const std::uint8_t> payload) {
    require(!payload.empty() && payload.size() <= maximum_object_bytes, ErrorCode::Budget, "Experiment record exceeds budget");
    std::unique_ptr<json_tokener, decltype(&json_tokener_free)> parser(json_tokener_new_ex(32), json_tokener_free);
    require(bool(parser), ErrorCode::Storage, "Cannot allocate experiment parser");
    json_tokener_set_flags(parser.get(), JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
    Json result(json_tokener_parse_ex(parser.get(), reinterpret_cast<const char*>(payload.data()),
        static_cast<int>(payload.size())), json_object_put);
    require(result && json_tokener_get_error(parser.get()) == json_tokener_success &&
        json_tokener_get_parse_end(parser.get()) == payload.size(), ErrorCode::Integrity, "Invalid experiment record");
    return result;
}
json_object* field(json_object* value, const char* key, json_type type) {
    require(value && json_object_is_type(value, json_type_object), ErrorCode::Integrity, "Expected experiment object");
    auto* result = json_object_object_get(value, key);
    require(result && json_object_is_type(result, type), ErrorCode::Integrity, "Invalid experiment field");
    return result;
}
std::string string(json_object* value, const char* key) {
    auto* result = field(value, key, json_type_string);
    return {json_object_get_string(result), static_cast<std::size_t>(json_object_get_string_len(result))};
}
std::size_t number(json_object* value, const char* key, std::size_t minimum, std::size_t maximum) {
    auto* result = field(value, key, json_type_int);
    const auto amount = json_object_get_int64(result);
    require(amount >= 0 && static_cast<std::uint64_t>(amount) >= minimum &&
        static_cast<std::uint64_t>(amount) <= maximum, ErrorCode::Integrity, "Experiment integer exceeds bounds");
    return static_cast<std::size_t>(amount);
}
Object record(Journal& journal, const ObjectId& id, ObjectKind kind, std::string_view format = {}) {
    const auto assessment = journal.assessment(id);
    require(!assessment || (assessment->state != KnowledgeState::Invalidated && assessment->logical != LogicalValidity::Invalid),
        ErrorCode::Integrity, "Experimental dependency requires revalidation");
    const auto result = journal.get(id);
    require(result.has_value(), ErrorCode::Unavailable, "Experimental dependency is unavailable");
    require(result->kind == kind && (format.empty() || result->format == format),
        ErrorCode::Integrity, "Experiment dependency has an incompatible kind or format");
    return *result;
}
void charge(std::size_t& used, const Object& value, std::size_t maximum = byte_limit) {
    const auto size = encode_object(value).size();
    require(used <= maximum && size <= maximum - used, ErrorCode::Budget, "Experiment aggregate byte budget exceeded");
    used += size;
}
Object object(ObjectKind kind, std::string_view format, std::string_view payload) {
    Object result; result.kind = kind; result.label = std::string(format); result.format = std::string(format);
    result.payload.assign(payload.begin(), payload.end());
    return result;
}
Object protocol_object(const ExperimentProtocol& protocol) {
    for (const auto& value : {protocol.name, protocol.revision, protocol.instrument}) {
        validate_text(value, 128); require(!value.empty(), ErrorCode::Schema, "Empty experiment protocol identity");
    }
    require(protocol.trials > 0 && protocol.trials <= 64 && !protocol.expected.empty() &&
        protocol.expected.size() <= 64, ErrorCode::Budget, "Invalid bounded experiment protocol");
    validate_goal_predicates(protocol.expected);
    Json value(json_object_new_object(), json_object_put);
    put_text(value.get(), "name", protocol.name); put_text(value.get(), "revision", protocol.revision);
    put_text(value.get(), "instrument", protocol.instrument); put_number(value.get(), "trials", protocol.trials);
    auto* expected = json_object_new_array(); json_object_object_add(value.get(), "expected", expected);
    constexpr std::array<std::string_view, 7> comparisons{"", "eq", "ne", "lt", "le", "gt", "ge"};
    for (const auto& predicate : protocol.expected) {
        auto* row = json_object_new_array(); json_object_array_add(expected, row);
        json_object_array_add(row, json_object_new_string(predicate.field.c_str()));
        json_object_array_add(row, json_object_new_string(comparisons.at(static_cast<std::size_t>(predicate.comparison)).data()));
        json_object_array_add(row, scalar(predicate.value));
    }
    return object(ObjectKind::Action, "gammathink.experiment-protocol.v1", wire(value.get()));
}
ExperimentProtocol protocol_value(const Object& stored) {
    const auto fields = parse(stored.payload);
    ExperimentProtocol result;
    result.name = string(fields.get(), "name"); result.revision = string(fields.get(), "revision");
    result.instrument = string(fields.get(), "instrument"); result.trials = number(fields.get(), "trials", 1, 64);
    auto* expected = field(fields.get(), "expected", json_type_array);
    require(json_object_array_length(expected) > 0 && json_object_array_length(expected) <= 64,
        ErrorCode::Integrity, "Invalid experiment criterion count");
    constexpr std::array<std::string_view, 6> comparisons{"eq", "ne", "lt", "le", "gt", "ge"};
    for (std::size_t index = 0; index < json_object_array_length(expected); ++index) {
        auto* row = json_object_array_get_idx(expected, index);
        require(row && json_object_is_type(row, json_type_array) && json_object_array_length(row) == 3,
            ErrorCode::Integrity, "Invalid experimental criterion");
        auto* key = json_object_array_get_idx(row, 0); auto* comparison = json_object_array_get_idx(row, 1);
        auto* value = json_object_array_get_idx(row, 2);
        require(key && comparison && value && json_object_is_type(key, json_type_string) &&
            json_object_is_type(comparison, json_type_string), ErrorCode::Integrity, "Invalid experimental criterion types");
        const auto found = std::find(comparisons.begin(), comparisons.end(), json_object_get_string(comparison));
        require(found != comparisons.end(), ErrorCode::Integrity, "Unknown experimental comparison");
        GoalValue target;
        switch (json_object_get_type(value)) {
            case json_type_boolean: target = bool(json_object_get_boolean(value)); break;
            case json_type_int: target = static_cast<std::int64_t>(json_object_get_int64(value)); break;
            case json_type_string: target = std::string(json_object_get_string(value),
                static_cast<std::size_t>(json_object_get_string_len(value))); break;
            default: throw Error(ErrorCode::Integrity, "Unsupported experimental criterion value");
        }
        result.expected.push_back({std::string(json_object_get_string(key), static_cast<std::size_t>(json_object_get_string_len(key))),
            static_cast<GoalComparison>(1 + (found - comparisons.begin())), std::move(target)});
    }
    require(stored == protocol_object(result), ErrorCode::Integrity, "Experiment protocol changed its canonical scope");
    return result;
}
std::vector<ObjectId> identities(json_object* fields, const char* key, std::size_t minimum = 0) {
    auto* values = field(fields, key, json_type_array);
    require(json_object_array_length(values) >= minimum && json_object_array_length(values) <= 64,
        ErrorCode::Integrity, "Experiment reference count exceeds bounds");
    std::vector<ObjectId> result;
    for (std::size_t index = 0; index < json_object_array_length(values); ++index) {
        auto* value = json_object_array_get_idx(values, index);
        require(value && json_object_is_type(value, json_type_string), ErrorCode::Integrity, "Invalid experiment reference");
        result.push_back(parse_id(json_object_get_string(value)));
    }
    return result;
}
void put_ids(json_object* value, const char* key, std::span<const ObjectId> ids) {
    auto* values = json_object_new_array(); json_object_object_add(value, key, values);
    for (const auto& id : ids) json_object_array_add(values, json_object_new_string(hex(id).c_str()));
}
Object experiment_object(Journal& journal, std::span<const ObjectId> hypotheses, const ObjectId& protocol,
    std::size_t& used, std::size_t maximum = byte_limit) {
    require(!hypotheses.empty() && hypotheses.size() <= 64, ErrorCode::Budget, "Expected a bounded joint hypothesis group");
    Json value(json_object_new_object(), json_object_put);
    put_text(value.get(), "protocol", hex(protocol)); put_ids(value.get(), "jointly_required", hypotheses);
    auto result = object(ObjectKind::Experiment, "gammathink.experiment.v1", wire(value.get()));
    result.links.push_back({Relationship::DependsOn, protocol});
    for (const auto& id : hypotheses) {
        const auto hypothesis = record(journal, id, ObjectKind::Hypothesis);
        charge(used, hypothesis, maximum);
        result.sources.insert(result.sources.end(), hypothesis.sources.begin(), hypothesis.sources.end());
        result.links.push_back({Relationship::Tests, id});
    }
    charge(used, result, maximum);
    return result;
}
struct Plan {
    Object experiment, protocol;
    ExperimentProtocol specification;
    std::size_t bytes = 0;
};
Plan load(Journal& journal, const ObjectId& id, std::size_t maximum = byte_limit) {
    Plan result;
    result.experiment = record(journal, id, ObjectKind::Experiment, "gammathink.experiment.v1");
    const auto fields = parse(result.experiment.payload);
    const auto protocol = parse_id(string(fields.get(), "protocol"));
    result.protocol = record(journal, protocol, ObjectKind::Action, "gammathink.experiment-protocol.v1");
    charge(result.bytes, result.protocol, maximum); result.specification = protocol_value(result.protocol);
    require(result.experiment == experiment_object(journal, identities(fields.get(), "jointly_required", 1),
        protocol, result.bytes, maximum), ErrorCode::Integrity, "Experiment lost its exact ordered hypotheses or protocol");
    return result;
}
std::string values_wire(const GoalConditions& values) {
    require(!values.empty() && values.size() <= 64, ErrorCode::Budget, "Instrument observation exceeds field budget");
    Json result(json_object_new_array(), json_object_put);
    for (const auto& [key, value] : values) {
        validate_text(key, 128);
        if (const auto* text = std::get_if<std::string>(&value)) validate_text(*text, 4096);
        auto* row = json_object_new_array(); json_object_array_add(result.get(), row);
        json_object_array_add(row, json_object_new_string(key.c_str())); json_object_array_add(row, scalar(value));
    }
    const auto encoded = wire(result.get());
    require(parse_task_environment(encoded) == values, ErrorCode::Integrity, "Instrument observation changed typed values");
    return encoded;
}
Object observation(const ObjectId& experiment, const ExperimentProtocol& protocol, const Digest& nonce,
    std::size_t trial, const GoalConditions& values) {
    const auto payload = values_wire(values);
    auto result = object(ObjectKind::Evidence, "gammathink.experiment-observation.v1", payload);
    result.sources = {{protocol.instrument, "1", hex(ObjectId{nonce}) + "/" + std::to_string(trial),
        "gammathink.host-instrument.v1", digest(result.payload), 0, result.payload.size()}};
    result.links = {{Relationship::DependsOn, experiment}};
    return result;
}
int matches(const ExperimentProtocol& protocol, const GoalConditions& values) {
    for (const auto& predicate : protocol.expected) {
        const auto found = values.find(predicate.field);
        if (found == values.end() || found->second.index() != predicate.value.index()) return -1;
    }
    GoalIR criterion; criterion.id = "measurement"; criterion.requirements = protocol.expected;
    const auto evaluation = evaluate_goals(decompose_goals(criterion.id, {criterion}), {}, values);
    return evaluation.states.front() == GoalState::Ready ? 1 : 0;
}
ExperimentMatch match(const std::vector<int>& results, ExperimentStop stop) {
    if (stop != ExperimentStop::Complete || results.empty() || std::find(results.begin(), results.end(), -1) != results.end())
        return ExperimentMatch::Incomplete;
    const auto passed = std::count(results.begin(), results.end(), 1);
    return passed == static_cast<std::ptrdiff_t>(results.size()) ? ExperimentMatch::Matched :
        passed == 0 ? ExperimentMatch::NotMatched : ExperimentMatch::Mixed;
}
Object receipt(const ExperimentReview& review, const Digest& nonce, const ObjectId& protocol) {
    Json value(json_object_new_object(), json_object_put);
    put_text(value.get(), "experiment", hex(review.experiment)); put_text(value.get(), "run_nonce", hex(ObjectId{nonce}));
    put_text(value.get(), "scope", hex(review.scope));
    put_number(value.get(), "basis", static_cast<std::size_t>(review.basis));
    put_number(value.get(), "stop", static_cast<std::size_t>(review.stop));
    put_number(value.get(), "match", static_cast<std::size_t>(review.match));
    put_number(value.get(), "attempted_trials", review.attempted_trials); put_ids(value.get(), "observations", review.observations);
    json_object_object_add(value.get(), "domain_review_required", json_object_new_boolean(true));
    json_object_object_add(value.get(), "hypothesis_promoted", json_object_new_boolean(false));
    json_object_object_add(value.get(), "empirical_truth_verified", json_object_new_boolean(false));
    json_object_object_add(value.get(), "logical_proof", json_object_new_boolean(false));
    auto result = object(ObjectKind::TestResult, "gammathink.experiment-result.v1", wire(value.get()));
    result.links = {{Relationship::DependsOn, review.experiment}, {Relationship::DependsOn, protocol},
        {Relationship::DependsOn, review.scope}};
    for (const auto& id : review.observations) result.links.push_back({Relationship::DependsOn, id});
    return result;
}
Object scope(const ObjectId& experiment, const Digest& nonce, const ExperimentProtocol& protocol,
    MeasurementBasis basis, const ExperimentLimits& limits) {
    Json value(json_object_new_object(), json_object_put);
    put_text(value.get(), "experiment", hex(experiment)); put_text(value.get(), "run_nonce", hex(ObjectId{nonce}));
    put_text(value.get(), "instrument", protocol.instrument);
    put_number(value.get(), "basis", static_cast<std::size_t>(basis));
    put_number(value.get(), "encoded_bytes", limits.encoded_bytes);
    put_number(value.get(), "time_ms", static_cast<std::uint64_t>(limits.time.count()));
    put_number(value.get(), "resident_bytes", limits.resident_bytes);
    auto result = object(ObjectKind::Action, "gammathink.experiment-scope.v1", wire(value.get()));
    result.links = {{Relationship::DependsOn, experiment}};
    return result;
}
void memory_budget(const ExperimentLimits& limits) {
    const auto usage = neural::process_memory();
    require(usage.resident_bytes > 0 && usage.lifetime_peak_bytes > 0 &&
        usage.lifetime_peak_bytes <= limits.resident_bytes, ErrorCode::Budget, "Experiment process memory budget exceeded");
}
}

GoalConditions ProcessMemoryInstrument::sample(std::chrono::steady_clock::time_point deadline, std::size_t maximum_bytes) {
    require(std::chrono::steady_clock::now() < deadline && maximum_bytes >= 256, ErrorCode::Budget, "Measurement budget exhausted");
    const auto memory = neural::process_memory();
    require(memory.resident_bytes > 0 && memory.lifetime_peak_bytes > 0 && memory.lifetime_peak_bytes <= INT64_MAX,
        ErrorCode::Unavailable, "Native process memory observation is unavailable");
    return {{"resident_bytes", static_cast<std::int64_t>(memory.resident_bytes)},
        {"peak_bytes", static_cast<std::int64_t>(memory.lifetime_peak_bytes)}};
}
void ExperimentRunner::current() const {
    require(std::this_thread::get_id() == thread_ && model_.state() == neural::ModelState::Unloaded,
        ErrorCode::Permission, "Experiments require their owning thread and unloaded model");
}
ObjectId ExperimentRunner::propose(std::span<const ObjectId> hypotheses, const ExperimentProtocol& protocol) {
    current();
    const auto control = protocol_object(protocol);
    std::size_t used = 0; charge(used, control);
    const auto proposed = experiment_object(journal_, hypotheses, identify(control), used);
    journal_.put(control);
    return journal_.put(proposed);
}
ExperimentPermit ExperimentRunner::authorize(const ObjectId& experiment,
    std::shared_ptr<ExperimentInstrument> instrument, ExperimentLimits limits) {
    current();
    require(instrument && limits.encoded_bytes > 0 && limits.encoded_bytes <= byte_limit &&
        limits.time.count() > 0 && limits.time <= std::chrono::minutes(5) &&
        limits.resident_bytes > 0 && limits.resident_bytes < 3000000000ULL,
        ErrorCode::Budget, "Invalid host experiment scope");
    const auto plan = load(journal_, experiment, limits.encoded_bytes);
    memory_budget(limits);
    require(instrument->id() == plan.specification.instrument &&
        (instrument->basis() == MeasurementBasis::Direct || instrument->basis() == MeasurementBasis::Simulation),
        ErrorCode::Permission, "Host instrument differs from the fixed experiment protocol");
    ExperimentPermit result;
    result.owner_ = owner_; result.instrument_ = std::move(instrument); result.experiment_ = experiment;
    result.limits_ = limits; result.basis_ = result.instrument_->basis();
    require(RAND_bytes(result.run_nonce_.data(), static_cast<int>(result.run_nonce_.size())) == 1,
        ErrorCode::Storage, "Cannot allocate a distinct physical trial identity");
    const auto fixed = scope(experiment, result.run_nonce_, plan.specification, result.basis_, limits);
    std::size_t used = plan.bytes; charge(used, fixed, limits.encoded_bytes);
    require(receipt_reserve <= limits.encoded_bytes - used, ErrorCode::Budget, "Experiment cannot retain its bounded receipt");
    result.scope_ = journal_.put(fixed);
    return result;
}
ExperimentReview ExperimentRunner::run(ExperimentPermit& permit) {
    current();
    require(permit.owner_ == owner_ && permit.instrument_ && !permit.used_, ErrorCode::Permission, "Foreign or spent experiment permit");
    const auto plan = load(journal_, permit.experiment_, permit.limits_.encoded_bytes);
    const auto fixed = record(journal_, permit.scope_, ObjectKind::Action, "gammathink.experiment-scope.v1");
    require(fixed == scope(permit.experiment_, permit.run_nonce_, plan.specification, permit.basis_, permit.limits_),
        ErrorCode::Integrity, "Experiment lost its fixed host scope");
    require(permit.instrument_->id() == plan.specification.instrument && permit.instrument_->basis() == permit.basis_,
        ErrorCode::Integrity, "Authorized instrument changed before observation");
    permit.used_ = true;
    ExperimentReview result; result.experiment = permit.experiment_; result.scope = permit.scope_; result.basis = permit.basis_;
    const auto deadline = std::chrono::steady_clock::now() + permit.limits_.time;
    std::size_t used = plan.bytes; charge(used, fixed, permit.limits_.encoded_bytes);
    std::vector<Object> observations; std::vector<int> matched;
    for (std::size_t index = 0; index < plan.specification.trials; ++index) {
        try {
            require(std::chrono::steady_clock::now() < deadline, ErrorCode::Budget, "Experiment deadline exhausted");
            memory_budget(permit.limits_);
            ++result.attempted_trials;
            const auto values = permit.instrument_->sample(deadline, permit.limits_.encoded_bytes - used - receipt_reserve);
            auto measured = observation(result.experiment, plan.specification, permit.run_nonce_, index, values);
            charge(used, measured, permit.limits_.encoded_bytes - receipt_reserve);
            matched.push_back(matches(plan.specification, values));
            result.observations.push_back(identify(measured)); observations.push_back(std::move(measured));
            memory_budget(permit.limits_);
            require(std::chrono::steady_clock::now() < deadline, ErrorCode::Budget, "Instrument exceeded its cooperative deadline");
        } catch (const Error& error) {
            if (error.code == ErrorCode::Budget) result.stop = ExperimentStop::ResourceLimited;
            else if (error.code == ErrorCode::Unavailable) result.stop = ExperimentStop::Unavailable;
            else throw;
            break;
        }
    }
    current();
    require(permit.instrument_->id() == plan.specification.instrument && permit.instrument_->basis() == permit.basis_,
        ErrorCode::Integrity, "Instrument identity changed during observation");
    (void)load(journal_, result.experiment, permit.limits_.encoded_bytes);
    result.match = match(matched, result.stop);
    const auto recorded = receipt(result, permit.run_nonce_, identify(plan.protocol));
    charge(used, recorded, permit.limits_.encoded_bytes);
    for (const auto& value : observations) journal_.put(value);
    result.receipt = journal_.put(recorded);
    return result;
}
ExperimentReview ExperimentRunner::review(const ObjectId& experiment, const ObjectId& id) {
    current();
    const auto plan = load(journal_, experiment);
    const auto stored = record(journal_, id, ObjectKind::TestResult, "gammathink.experiment-result.v1");
    const auto fields = parse(stored.payload);
    require(parse_id(string(fields.get(), "experiment")) == experiment, ErrorCode::Integrity, "Result belongs to another experiment");
    const auto nonce = parse_id(string(fields.get(), "run_nonce")).digest;
    ExperimentReview result; result.experiment = experiment; result.receipt = id;
    result.scope = parse_id(string(fields.get(), "scope"));
    result.basis = static_cast<MeasurementBasis>(number(fields.get(), "basis", 1, 2));
    result.stop = static_cast<ExperimentStop>(number(fields.get(), "stop", 1, 3));
    result.attempted_trials = number(fields.get(), "attempted_trials", 0, plan.specification.trials);
    result.observations = identities(fields.get(), "observations");
    require(result.observations.size() <= result.attempted_trials &&
        result.attempted_trials - result.observations.size() <= 1 &&
        (result.stop != ExperimentStop::Complete || result.observations.size() == plan.specification.trials),
        ErrorCode::Integrity, "Result changed its trial coverage");
    const auto fixed = record(journal_, result.scope, ObjectKind::Action, "gammathink.experiment-scope.v1");
    const auto policy = parse(fixed.payload);
    ExperimentLimits limits;
    limits.encoded_bytes = number(policy.get(), "encoded_bytes", 1, byte_limit);
    limits.time = std::chrono::milliseconds(number(policy.get(), "time_ms", 1, 300000));
    limits.resident_bytes = number(policy.get(), "resident_bytes", 1, 2999999999ULL);
    require(fixed == scope(experiment, nonce, plan.specification, result.basis, limits),
        ErrorCode::Integrity, "Experiment result changed its host scope");
    std::size_t used = plan.bytes; charge(used, stored, limits.encoded_bytes); charge(used, fixed, limits.encoded_bytes);
    std::vector<int> matched;
    for (std::size_t index = 0; index < result.observations.size(); ++index) {
        const auto measured = record(journal_, result.observations[index], ObjectKind::Evidence, "gammathink.experiment-observation.v1");
        charge(used, measured, limits.encoded_bytes);
        const auto values = parse_task_environment(std::string(measured.payload.begin(), measured.payload.end()));
        require(measured == observation(experiment, plan.specification, nonce, index, values),
            ErrorCode::Integrity, "Observation changed its exact values, trial identity or provenance");
        matched.push_back(matches(plan.specification, values));
    }
    result.match = match(matched, result.stop);
    require(stored == receipt(result, nonce, identify(plan.protocol)), ErrorCode::Integrity,
        "Result changed its independently checked criterion match or authority");
    return result;
}

} // namespace axiom::knowledge
