#include "task_session.hpp"
#include "problem_semantic.hpp"
#include "virtual_drive.hpp"
#include "drive_network.hpp"
#include "experiments.hpp"
#include "hypotheses.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <fstream>
#include <iostream>
#include <json-c/json.h>
#include <mach-o/dyld.h>
#include <sys/resource.h>

namespace {
using namespace axiom;
using namespace axiom::context;
using namespace axiom::knowledge;
using namespace axiom::runtime;
using Json = std::unique_ptr<json_object, decltype(&json_object_put)>;
void require(bool condition, ErrorCode code, const char* message) {
    if (!condition) throw Error(code, message);
}
std::string wire(json_object* value) {
    require(value != nullptr, ErrorCode::Schema, "Missing command value");
    return json_object_to_json_string_ext(value, JSON_C_TO_STRING_PLAIN);
}
Json parse(std::string_view text, std::size_t maximum = 4 * 1024 * 1024) {
    require(!text.empty() && maximum <= 16 * 1024 * 1024 && text.size() <= maximum,
        ErrorCode::Budget, "JSON document exceeds byte budget");
    std::unique_ptr<json_tokener, decltype(&json_tokener_free)> parser(json_tokener_new_ex(64), json_tokener_free);
    require(bool(parser), ErrorCode::Storage, "Cannot allocate command parser");
    json_tokener_set_flags(parser.get(), JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
    Json value(json_tokener_parse_ex(parser.get(), text.data(), static_cast<int>(text.size())), json_object_put);
    require(value && json_tokener_get_error(parser.get()) == json_tokener_success &&
        json_tokener_get_parse_end(parser.get()) == text.size(), ErrorCode::Schema, "Invalid complete command JSON");
    return value;
}
json_object* field(json_object* object, const char* key, bool required = true) {
    json_object* value = nullptr;
    const bool found = json_object_object_get_ex(object, key, &value);
    require(!required || (found && value), ErrorCode::Schema, "Missing command field");
    return value;
}
std::string text(json_object* object, const char* key, std::size_t maximum = 65536, bool empty = false) {
    auto* value = field(object, key);
    require(json_object_is_type(value, json_type_string), ErrorCode::Schema, "Expected command text");
    std::string result(json_object_get_string(value), static_cast<std::size_t>(json_object_get_string_len(value)));
    require((empty || !result.empty()) && result.size() <= maximum, ErrorCode::Budget, "Command text exceeds its budget");
    if (!result.empty()) validate_text(result, maximum);
    return result;
}
std::int64_t number(json_object* object, const char* key, std::int64_t minimum = 0,
    std::int64_t maximum = INT64_MAX) {
    auto* value = field(object, key);
    require(json_object_is_type(value, json_type_int), ErrorCode::Schema, "Expected exact command integer");
    const auto encoded = wire(value);
    std::int64_t result = 0;
    const auto parsed = std::from_chars(encoded.data(), encoded.data() + encoded.size(), result);
    require(parsed.ec == std::errc{} && parsed.ptr == encoded.data() + encoded.size() &&
        result >= minimum && result <= maximum, ErrorCode::Budget, "Command integer exceeds its budget");
    return result;
}
bool boolean(json_object* object, const char* key) {
    auto* value = field(object, key);
    require(json_object_is_type(value, json_type_boolean), ErrorCode::Schema, "Expected command boolean");
    return json_object_get_boolean(value);
}
void keys(json_object* object, std::initializer_list<std::string_view> allowed) {
    require(object && json_object_is_type(object, json_type_object), ErrorCode::Schema, "Expected command object");
    json_object_object_foreach(object, key, value) {
        (void)value;
        require(std::find(allowed.begin(), allowed.end(), key) != allowed.end(), ErrorCode::Schema, "Unknown command field");
    }
}
void string(json_object* object, const char* key, std::string_view value) {
    json_object_object_add(object, key, json_object_new_string_len(value.data(), static_cast<int>(value.size())));
}
json_object* identity_list(const std::vector<ObjectId>& ids) {
    auto* values = json_object_new_array();
    for (const auto& id : ids) json_object_array_add(values, json_object_new_string(hex(id).c_str()));
    return values;
}
void task_result(json_object* output, const SessionTask& task) {
    json_object_object_add(output, "revision", json_object_new_uint64(task.revision));
    json_object_object_add(output, "source_message", json_object_new_uint64(task.user_message));
    json_object_object_add(output, "phase", json_object_new_int(static_cast<int>(task.phase)));
    string(output, "task", hex(task.ordered_task));
    json_object_object_add(output, "native_certificates", identity_list(task.certificates));
}
ObjectKind kind(std::string_view value) {
    for (int i = 1; i <= 12; ++i)
        if (value == name(static_cast<ObjectKind>(i))) return static_cast<ObjectKind>(i);
    throw Error(ErrorCode::Schema, "Unknown advertised knowledge kind");
}
std::map<std::string, TaskObject> objects(json_object* value) {
    std::map<std::string, TaskObject> result;
    if (!value) return result;
    require(json_object_is_type(value, json_type_object), ErrorCode::Schema, "Expected advertised objects");
    json_object_object_foreach(value, key, descriptor) {
        keys(descriptor, {"id", "kind", "format"});
        require(result.size() < 128, ErrorCode::Budget, "Advertised object count exceeded");
        result.emplace(key, TaskObject{parse_id(text(descriptor, "id", 64)),
            kind(text(descriptor, "kind", 64)), text(descriptor, "format", 256)});
    }
    return result;
}
std::vector<GoalPredicate> predicates(json_object* value) {
    std::vector<GoalPredicate> result;
    if (!value) return result;
    require(json_object_is_type(value, json_type_array) && json_object_array_length(value) <= 64,
        ErrorCode::Budget, "Invalid active policy predicates");
    const std::array<std::pair<std::string_view, GoalComparison>, 6> comparisons{{
        {"eq", GoalComparison::Equal}, {"ne", GoalComparison::NotEqual}, {"lt", GoalComparison::Less},
        {"le", GoalComparison::LessEqual}, {"gt", GoalComparison::Greater}, {"ge", GoalComparison::GreaterEqual},
    }};
    for (std::size_t i = 0; i < json_object_array_length(value); ++i) {
        auto* row = json_object_array_get_idx(value, i);
        require(row && json_object_is_type(row, json_type_array) && json_object_array_length(row) == 3 &&
            json_object_is_type(json_object_array_get_idx(row, 1), json_type_string),
            ErrorCode::Schema, "Expected typed policy predicate tuple");
        const std::string comparison = json_object_get_string(json_object_array_get_idx(row, 1));
        const auto found = std::find_if(comparisons.begin(), comparisons.end(),
            [&](const auto& item) { return item.first == comparison; });
        require(found != comparisons.end(), ErrorCode::Schema, "Unknown policy comparison");
        Json pair(json_object_new_array(), json_object_put), environment(json_object_new_array(), json_object_put);
        json_object_array_add(pair.get(), json_object_get(json_object_array_get_idx(row, 0)));
        json_object_array_add(pair.get(), json_object_get(json_object_array_get_idx(row, 2)));
        json_object_array_add(environment.get(), pair.release());
        const auto scalar = parse_task_environment(wire(environment.get()));
        require(scalar.size() == 1, ErrorCode::Schema, "Policy predicate lost its typed scalar");
        result.push_back({scalar.begin()->first, found->second, scalar.begin()->second});
    }
    validate_goal_predicates(result);
    return result;
}
ProblemContext host_context(json_object* command) {
    if (auto* context = field(command, "host_context", false)) return parse_problem_context(wire(context));
    ProblemContext result; result.unit_registry_id = "structured-session"; result.unit_registry_version = "1";
    return result;
}
neural::ModelSpec model_spec(json_object* value, neural::ModelRole role) {
    keys(value, {"id", "path", "sha256", "enable_thinking"});
    return {text(value, "id", 256), text(value, "path", 4096), parse_id(text(value, "sha256", 64)).digest,
        role, field(value, "enable_thinking", false) ? boolean(value, "enable_thinking") : false};
}
neural::ModelLimits model_limits(json_object* value) {
    neural::ModelLimits result;
    if (!value) return result;
    keys(value, {"context_tokens", "threads", "input_bytes", "output_bytes", "file_bytes", "resident_bytes", "phase_seconds"});
    if (field(value, "context_tokens", false)) result.context_tokens = static_cast<std::uint32_t>(number(value, "context_tokens", 32, 32768));
    if (field(value, "threads", false)) result.threads = static_cast<std::uint32_t>(number(value, "threads", 1, 10));
    if (field(value, "input_bytes", false)) result.input_bytes = static_cast<std::size_t>(number(value, "input_bytes", 1, 4 * 1024 * 1024));
    if (field(value, "output_bytes", false)) result.output_bytes = static_cast<std::size_t>(number(value, "output_bytes", 1, 4 * 1024 * 1024));
    if (field(value, "file_bytes", false)) result.file_bytes = static_cast<std::uint64_t>(number(value, "file_bytes", 1, 2999999999LL));
    if (field(value, "resident_bytes", false)) result.resident_bytes = static_cast<std::uint64_t>(number(value, "resident_bytes", 1, 2999999999LL));
    if (field(value, "phase_seconds", false)) result.phase_time = std::chrono::seconds(number(value, "phase_seconds", 1, 1800));
    return result;
}
std::set<ObjectId> accepted(json_object* command) {
    std::set<ObjectId> result;
    if (auto* values = field(command, "accepted_assumptions", false)) {
        require(json_object_is_type(values, json_type_array) && json_object_array_length(values) <= 128,
            ErrorCode::Budget, "Accepted-assumption count exceeded");
        for (std::size_t i = 0; i < json_object_array_length(values); ++i) {
            auto* value = json_object_array_get_idx(values, i);
            require(value && json_object_is_type(value, json_type_string), ErrorCode::Schema, "Expected accepted assumption ID");
            result.insert(parse_id(json_object_get_string(value)));
        }
    }
    return result;
}
std::map<ObjectId, ProblemContext> source_contexts(json_object* command) {
    std::map<ObjectId, ProblemContext> result;
    if (auto* values = field(command, "source_contexts", false)) {
        require(json_object_is_type(values, json_type_object), ErrorCode::Schema, "Expected source context map");
        json_object_object_foreach(values, id, value) {
            require(result.size() < 64, ErrorCode::Budget, "Source context count exceeded");
            result.emplace(parse_id(id), parse_problem_context(wire(value)));
        }
    }
    return result;
}
std::pair<std::vector<execution::Test>, execution::Limits> execution_scope(json_object* value) {
    keys(value, {"tests", "limits"});
    auto* tests = field(value, "tests");
    require(json_object_is_type(tests, json_type_array) && json_object_array_length(tests) > 0 &&
        json_object_array_length(tests) <= 16, ErrorCode::Budget, "Invalid fixed test count");
    std::vector<execution::Test> result;
    for (std::size_t i = 0; i < json_object_array_length(tests); ++i) {
        auto* test = json_object_array_get_idx(tests, i);
        keys(test, {"input", "output"});
        result.push_back({text(test, "input", 65536, true), text(test, "output", 65536, true)});
    }
    execution::Limits limits; limits.attempts = 1;
    if (auto* settings = field(value, "limits", false)) {
        keys(settings, {"attempts", "seconds", "output_bytes", "source_bytes", "resident_bytes"});
        if (field(settings, "attempts", false)) limits.attempts = static_cast<std::uint32_t>(number(settings, "attempts", 1, 8));
        if (field(settings, "seconds", false)) limits.seconds = static_cast<std::uint32_t>(number(settings, "seconds", 1, 60));
        if (field(settings, "output_bytes", false)) limits.output_bytes = static_cast<std::size_t>(number(settings, "output_bytes", 1, 65536));
        if (field(settings, "source_bytes", false)) limits.source_bytes = static_cast<std::size_t>(number(settings, "source_bytes", 1, 65536));
        if (field(settings, "resident_bytes", false)) limits.resident_bytes = static_cast<std::uint64_t>(
            number(settings, "resident_bytes", 16 * 1024 * 1024, 1024 * 1024 * 1024));
    }
    return {std::move(result), limits};
}
TaskSourceCompilation source_scope(json_object* value) {
    keys(value, {"source", "context", "snapshot", "archive_dsl", "candidate", "storage", "selectors", "translation_profile"});
    TaskSourceCompilation result;
    result.context = parse_problem_context(wire(field(value, "context")));
    result.translation = {parse_id(text(value, "source", 64)), text(value, "snapshot", 256),
        field(value, "archive_dsl", false) ? text(value, "archive_dsl", 1024 * 1024) : ""};
    if (field(value, "candidate", false)) result.candidate = parse_id(text(value, "candidate", 64));
    if (field(value, "translation_profile", false)) {
        const auto profile = text(value, "translation_profile", 64);
        require(profile == "gammathink.source-translation-input.v1" ||
            profile == "gammathink.source-translation-input.v2", ErrorCode::Schema, "Unknown source translation profile");
        result.profile = profile == "gammathink.source-translation-input.v2"
            ? SourceTranslationProfile::QuantifiedV2 : SourceTranslationProfile::V1;
    }
    if (auto* storage = field(value, "storage", false)) {
        keys(storage, {"objects", "encoded_bytes"});
        if (field(storage, "objects", false)) result.storage.objects =
            static_cast<std::size_t>(number(storage, "objects", 2, 512));
        if (field(storage, "encoded_bytes", false)) result.storage.encoded_bytes =
            static_cast<std::size_t>(number(storage, "encoded_bytes", 1, 64 * 1024 * 1024));
    }
    if (field(value, "selectors", false)) {
        const auto mode = text(value, "selectors", 32);
        require(mode == "offset-v1" || mode == "quote-v1", ErrorCode::Schema, "Unknown source selector profile");
        result.selectors = mode == "offset-v1" ? ProblemSelectorMode::OffsetV1 : ProblemSelectorMode::QuoteV1;
    }
    return result;
}
InspectionLimits inspection_limits(json_object* value) {
    InspectionLimits result;
    if (!value) return result;
    keys(value, {"objects", "bytes", "queries", "frontier", "depth", "incoming_page"});
    if (field(value, "objects", false)) result.objects = static_cast<std::size_t>(number(value, "objects", 1, 256));
    if (field(value, "bytes", false)) result.bytes = static_cast<std::size_t>(number(value, "bytes", 1, 8 * 1024 * 1024));
    if (field(value, "queries", false)) result.queries = static_cast<std::size_t>(number(value, "queries", 1, 1024));
    if (field(value, "frontier", false)) result.frontier = static_cast<std::size_t>(number(value, "frontier", 1, 4096));
    if (field(value, "depth", false)) result.depth = static_cast<std::size_t>(number(value, "depth", 0, 32));
    if (field(value, "incoming_page", false)) result.incoming_page = static_cast<std::size_t>(number(value, "incoming_page", 1, 64));
    return result;
}
std::string input(const char* path) {
    std::ifstream file;
    std::istream* stream = &std::cin;
    if (std::string_view(path) != "-") {
        file.open(path, std::ios::binary); require(file.good(), ErrorCode::Storage, "Cannot open command input");
        stream = &file;
    }
    std::string result;
    std::array<char, 4096> buffer{};
    while (stream->good()) {
        stream->read(buffer.data(), buffer.size());
        const auto count = stream->gcount();
        require(count >= 0 && static_cast<std::size_t>(count) <= 4 * 1024 * 1024 - result.size(),
            ErrorCode::Budget, "Command input exceeds byte budget");
        result.append(buffer.data(), static_cast<std::size_t>(count));
    }
    require(stream->eof(), ErrorCode::Storage, "Cannot finish reading command");
    return result;
}
std::filesystem::path execution_helper() {
    std::uint32_t size = 0;
    (void)_NSGetExecutablePath(nullptr, &size);
    require(size > 0 && size <= 65536, ErrorCode::Storage, "Cannot size application executable path");
    std::string executable(size, '\0');
    require(_NSGetExecutablePath(executable.data(), &size) == 0,
        ErrorCode::Storage, "Cannot locate application executable");
    return std::filesystem::canonical(
        std::filesystem::path(executable.c_str()).parent_path() / "execution-helper");
}
void mount_result(json_object* output, const MountState& mount) {
    string(output, "alias", mount.alias);
    json_object_object_add(output, "generation", json_object_new_uint64(mount.generation));
    json_object_object_add(output, "mounted", json_object_new_boolean(mount.binding.has_value()));
    if (mount.binding) {
        const auto& binding = *mount.binding;
        string(output, "name", binding.archive.name); string(output, "revision", binding.archive.revision);
        string(output, "root", hex(binding.archive.root)); string(output, "directory", binding.directory.string());
        string(output, "source", binding.source);
        json_object_object_add(output, "backend", json_object_new_int(static_cast<int>(binding.backend)));
    }
}
SnapshotLimits snapshot_limits(json_object* value) {
    SnapshotLimits result;
    if (!value) return result;
    keys(value, {"objects", "input_bytes", "output_bytes", "index_entries", "workspace_bytes"});
    if (field(value, "objects", false)) result.objects = number(value, "objects", 1, result.objects);
    if (field(value, "input_bytes", false)) result.input_bytes = number(value, "input_bytes", 1, result.input_bytes);
    if (field(value, "output_bytes", false)) result.output_bytes = number(value, "output_bytes", 1, result.output_bytes);
    if (field(value, "index_entries", false)) result.index_entries = number(value, "index_entries", 1, result.index_entries);
    if (field(value, "workspace_bytes", false)) result.workspace_bytes = number(value, "workspace_bytes", 1, result.workspace_bytes);
    return result;
}
std::unique_ptr<DriveNetwork> network_access(json_object* value, const std::filesystem::path& directory,
    std::vector<std::string>& source_keys) {
    if (!value) return {};
    keys(value, {"enabled", "sources", "cache_limits", "http_limits", "retrieval_limits"});
    auto* sources = field(value, "sources");
    require(json_object_is_type(sources, json_type_array) && json_object_array_length(sources) > 0 &&
        json_object_array_length(sources) <= 15, ErrorCode::Budget, "Expected 1 to 15 explicit network sources");
    DriveNetworkLimits limits; limits.sources = json_object_array_length(sources);
    const auto lower = [](json_object* object, const char* key, std::uint64_t maximum) {
        return field(object, key, false) ? static_cast<std::uint64_t>(number(object, key, 1,
            static_cast<std::int64_t>(maximum))) : maximum;
    };
    if (auto* cache = field(value, "cache_limits", false)) {
        keys(cache, {"ram_bytes", "ram_entries", "ssd_bytes", "ssd_entries", "lease_references"});
        limits.cache.ram_bytes = lower(cache, "ram_bytes", limits.cache.ram_bytes);
        limits.cache.ram_entries = lower(cache, "ram_entries", limits.cache.ram_entries);
        limits.cache.ssd_bytes = lower(cache, "ssd_bytes", limits.cache.ssd_bytes);
        limits.cache.ssd_entries = lower(cache, "ssd_entries", limits.cache.ssd_entries);
        limits.cache.lease_references = lower(cache, "lease_references", limits.cache.lease_references);
    }
    if (auto* http = field(value, "http_limits", false)) {
        keys(http, {"requests", "total_bytes", "query_bytes"});
        limits.network.requests = lower(http, "requests", limits.network.requests);
        limits.network.total_bytes = lower(http, "total_bytes", limits.network.total_bytes);
        limits.network.query_bytes = lower(http, "query_bytes", limits.network.query_bytes);
    }
    RetrievalLimits retrieval;
    if (auto* budget = field(value, "retrieval_limits", false)) {
        keys(budget, {"reads", "returned_bytes", "upstream_reads", "upstream_bytes", "seconds"});
        retrieval.reads = lower(budget, "reads", retrieval.reads);
        retrieval.returned_bytes = lower(budget, "returned_bytes", retrieval.returned_bytes);
        retrieval.upstream_reads = lower(budget, "upstream_reads", retrieval.upstream_reads);
        retrieval.upstream_bytes = lower(budget, "upstream_bytes", retrieval.upstream_bytes);
        retrieval.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(lower(budget, "seconds", 30));
    }
    auto result = std::make_unique<DriveNetwork>(directory / "block-cache", limits);
    result->begin_retrieval(retrieval);
    for (std::size_t index = 0; index < limits.sources; ++index) {
        auto* source = json_object_array_get_idx(sources, index);
        keys(source, {"key", "origin", "network", "allow_insecure_loopback", "block_bytes", "allow_query_disclosure"});
        GatewayOptions blocks;
        blocks.origin = text(source, "origin", 4096);
        blocks.network = boolean(source, "network");
        if (field(source, "allow_insecure_loopback", false))
            blocks.allow_insecure_loopback = boolean(source, "allow_insecure_loopback");
        if (field(source, "block_bytes", false))
            blocks.block_bytes = static_cast<std::size_t>(number(source, "block_bytes", 1, gks_max_block_bytes));
        const auto key = text(source, "key", 64);
        std::optional<RemoteQueryOptions> queries;
        if (field(source, "allow_query_disclosure", false) && boolean(source, "allow_query_disclosure")) {
            queries.emplace();
            queries->origin = blocks.origin; queries->network = blocks.network;
            queries->allow_insecure_loopback = blocks.allow_insecure_loopback;
            queries->allow_query_disclosure = true;
        }
        result->add(key, blocks, queries);
        source_keys.push_back(key);
    }
    result->set_network(boolean(value, "enabled"));
    return result;
}
void network_result(json_object* output, const DriveNetwork& network, const std::vector<std::string>& keys) {
    auto* value = json_object_new_object(); json_object_object_add(output, "network_usage", value);
    json_object_object_add(value, "enabled", json_object_new_boolean(network.network_enabled()));
    const auto http = network.network_stats();
    json_object_object_add(value, "requests", json_object_new_uint64(http.requests));
    json_object_object_add(value, "accepted_bytes", json_object_new_uint64(http.accepted_bytes));
    json_object_object_add(value, "query_bytes", json_object_new_uint64(http.query_bytes));
    auto* sources = json_object_new_array(); json_object_object_add(value, "sources", sources);
    for (const auto& key : keys) {
        auto* source = json_object_new_object(); json_object_array_add(sources, source); string(source, "key", key);
        const auto cache = network.cache_stats(key); const auto retrieval = network.retrieval_stats(key);
        json_object_object_add(source, "ram_bytes", json_object_new_uint64(cache.ram_bytes));
        json_object_object_add(source, "ssd_bytes", json_object_new_uint64(cache.ssd_bytes));
        json_object_object_add(source, "ssd_hits", json_object_new_uint64(cache.ssd_hits));
        json_object_object_add(source, "source_reads", json_object_new_uint64(cache.source_reads));
        json_object_object_add(source, "reads", json_object_new_uint64(retrieval.reads));
        json_object_object_add(source, "returned_bytes", json_object_new_uint64(retrieval.returned_bytes));
        json_object_object_add(source, "upstream_reads", json_object_new_uint64(retrieval.upstream_reads));
        json_object_object_add(source, "upstream_bytes", json_object_new_uint64(retrieval.upstream_bytes));
    }
}
void experiment_result(json_object* output, const ExperimentReview& review) {
    string(output, "experiment", hex(review.experiment)); string(output, "scope", hex(review.scope));
    string(output, "receipt", hex(review.receipt));
    json_object_object_add(output, "observations", identity_list(review.observations));
    json_object_object_add(output, "measurement_basis", json_object_new_int(static_cast<int>(review.basis)));
    json_object_object_add(output, "experiment_stop", json_object_new_int(static_cast<int>(review.stop)));
    json_object_object_add(output, "criterion_match", json_object_new_int(static_cast<int>(review.match)));
    json_object_object_add(output, "attempted_trials", json_object_new_uint64(review.attempted_trials));
    json_object_object_add(output, "domain_review_required", json_object_new_boolean(review.domain_review_required));
    json_object_object_add(output, "empirical_truth_verified", json_object_new_boolean(false));
    json_object_object_add(output, "hypothesis_promoted", json_object_new_boolean(false));
}
}

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "gammathink-session STATE_DIRECTORY COMMAND_JSON_FILE|-\n";
        return 2;
    }
    try {
        const auto command = parse(input(argv[2]));
        require(json_object_is_type(command.get(), json_type_object) && number(command.get(), "version", 1, 1) == 1,
            ErrorCode::Schema, "Unsupported session command version");
        const auto mode = text(command.get(), "command", 32);
        const auto session = text(command.get(), "session", 256);
        if (mode == "init") keys(command.get(), {"version", "command", "session", "project"});
        else if (mode == "open") keys(command.get(), {"version", "command", "session", "project"});
        else if (mode == "message") keys(command.get(), {"version", "command", "session", "timestamp", "text"});
        else if (mode == "pin") keys(command.get(), {"version", "command", "session", "key", "constraint_revision", "source_message", "active", "text"});
        else if (mode == "capture-structured") keys(command.get(), {"version", "command", "session", "host_context", "objects", "active_constraints", "history_query", "document_query"});
        else if (mode == "import-context") keys(command.get(), {"version", "command", "session", "key", "uri",
            "timestamp", "text", "kind", "attribution", "document_revision", "shared", "active"});
        else if (mode == "search-context") keys(command.get(), {"version", "command", "session", "query", "limit"});
        else if (mode == "read-source") keys(command.get(), {"version", "command", "session", "archive", "entry", "host_context"});
        else if (mode == "read-artifact") keys(command.get(), {"version", "command", "session", "id"});
        else if (mode == "snapshot-journal") keys(command.get(), {"version", "command", "session", "destination", "name", "revision", "limits"});
        else if (mode == "mount") keys(command.get(), {"version", "command", "session", "alias", "directory", "root", "generation"});
        else if (mode == "mount-remote") keys(command.get(), {"version", "command", "session", "alias", "root", "generation",
            "source", "backend", "network_access"});
        else if (mode == "unmount") keys(command.get(), {"version", "command", "session", "alias", "generation"});
        else if (mode == "mounts") keys(command.get(), {"version", "command", "session"});
        else if (mode == "resources") keys(command.get(), {"version", "command", "session"});
        else if (mode == "hypothesize") keys(command.get(), {"version", "command", "session", "goal"});
        else if (mode == "propose-experiment") keys(command.get(), {"version", "command", "session", "hypotheses", "protocol"});
        else if (mode == "measure-experiment") keys(command.get(), {"version", "command", "session", "experiment", "observation_scope"});
        else if (mode == "review-experiment") keys(command.get(), {"version", "command", "session", "experiment", "receipt"});
        else if (mode == "admit") keys(command.get(), {"version", "command", "session", "revision", "source_message", "context", "snapshot", "proposal", "network_access"});
        else if (mode == "refine") keys(command.get(), {"version", "command", "session", "revision", "context", "snapshot", "proposal", "network_access"});
        else if (mode == "interpret") keys(command.get(), {"version", "command", "session", "revision", "host_context", "objects",
            "active_constraints", "history_query", "document_query", "model", "model_limits", "output_tokens", "network_access"});
        else if (mode == "step") keys(command.get(), {"version", "command", "session", "revision", "context", "environment", "action",
            "accepted_assumptions", "source_contexts", "model", "model_limits", "output_tokens", "execution_scope", "source_scope", "inspection_limits", "network_access"});
        else if (mode == "translate-source") keys(command.get(), {"version", "command", "session", "revision", "context", "environment", "action",
            "accepted_assumptions", "source_contexts", "source_scope", "model", "model_limits", "output_tokens", "network_access"});
        else if (mode == "deliver") keys(command.get(), {"version", "command", "session", "revision", "context", "environment", "action",
            "accepted_assumptions", "source_contexts", "timestamp", "network_access"});
        else if (mode == "review-source") keys(command.get(), {"version", "command", "session", "revision", "context", "environment", "action",
            "accepted_assumptions", "source_contexts", "network_access"});
        else if (mode == "history") keys(command.get(), {"version", "command", "session", "after", "limit"});
        else if (mode == "recall") keys(command.get(), {"version", "command", "session", "query", "after", "limit"});
        else throw Error(ErrorCode::Schema, "Unknown session command");
        const std::filesystem::path directory(argv[1]);
        if (mode == "init") require(std::filesystem::create_directory(directory),
            ErrorCode::Schema, "Session initialization requires a new private state directory");
        else for (const auto* file : {"conversation.db", "tasks.db", "journal.db"})
            require(std::filesystem::is_regular_file(directory / file), ErrorCode::Storage, "Existing session state is incomplete");
        ConversationMemory memory(directory / "conversation.db");
        std::unique_ptr<ContextDocuments> documents;
        if (mode == "import-context" || mode == "search-context" ||
            ((mode == "interpret" || mode == "capture-structured") &&
                (field(command.get(), "document_query", false) ||
                 std::filesystem::exists(directory / "context-documents.db"))))
            documents = std::make_unique<ContextDocuments>(directory / "context-documents.db");
        SessionTasks tasks(directory / "tasks.db");
        Journal journal(directory / "journal.db");
        ArchiveRegistry registry(directory / "archives.db", {.active_mounts = 15});
        std::vector<std::string> network_keys;
        const auto network = network_access(field(command.get(), "network_access", false), directory, network_keys);
        Json output(json_object_new_object(), json_object_put);
        string(output.get(), "profile", "gammathink.session-result.v1"); string(output.get(), "command", mode);
        if (mode == "init" || mode == "open") {
            memory.open_session(session, text(command.get(), "project", 256));
            string(output.get(), "status", "opened");
        } else if (mode == "message") {
            const auto message = memory.append(session, Role::User, text(command.get(), "timestamp", 256),
                text(command.get(), "text"));
            json_object_object_add(output.get(), "message", json_object_new_uint64(message.id));
            string(output.get(), "content_hash", hex(ObjectId{message.content_hash}));
        } else if (mode == "import-context") {
            ContextDocument document;
            document.project = memory.project(session);
            document.session = boolean(command.get(), "shared") ? "" : session;
            document.key = text(command.get(), "key", 256);
            document.uri = text(command.get(), "uri", 4096);
            document.timestamp = text(command.get(), "timestamp", 256);
            document.content = text(command.get(), "text");
            document.kind = static_cast<DocumentKind>(number(command.get(), "kind", 1, 4));
            document.attribution = static_cast<DocumentAttribution>(number(command.get(), "attribution", 1, 2));
            document.active = boolean(command.get(), "active");
            const auto saved = documents->save(std::move(document),
                static_cast<std::uint64_t>(number(command.get(), "document_revision")));
            json_object_object_add(output.get(), "document_revision", json_object_new_uint64(saved.revision));
            string(output.get(), "content_hash", hex(ObjectId{saved.content_hash}));
            json_object_object_add(output.get(), "knowledge_accepted", json_object_new_boolean(false));
        } else if (mode == "search-context") {
            const auto page = documents->search(memory.project(session), session, text(command.get(), "query", 1024),
                field(command.get(), "limit", false) ? static_cast<std::size_t>(number(command.get(), "limit", 1, 256)) : 16);
            auto* excerpts = json_object_new_array(); json_object_object_add(output.get(), "excerpts", excerpts);
            for (const auto& excerpt : page.excerpts) {
                const auto& source = excerpt.source;
                auto* row = json_object_new_object(); json_object_array_add(excerpts, row);
                string(row, "project", source.project); string(row, "session", source.session);
                string(row, "key", source.key); string(row, "uri", source.uri);
                string(row, "timestamp", source.timestamp); string(row, "text", excerpt.content);
                string(row, "source_hash", hex(ObjectId{source.content_hash}));
                string(row, "content_hash", hex(ObjectId{excerpt.content_hash}));
                json_object_object_add(row, "revision", json_object_new_uint64(source.revision));
                json_object_object_add(row, "kind", json_object_new_int(static_cast<int>(source.kind)));
                json_object_object_add(row, "attribution", json_object_new_int(static_cast<int>(source.attribution)));
                json_object_object_add(row, "offset", json_object_new_uint64(excerpt.offset));
                json_object_object_add(row, "source_bytes", json_object_new_uint64(excerpt.source_bytes));
            }
            json_object_object_add(output.get(), "complete", json_object_new_boolean(page.complete));
        } else if (mode == "resources") {
            (void)memory.recent(session, 1);
            const auto disk = std::filesystem::space(directory);
            const auto process = neural::process_memory();
            rusage usage{};
            require(getrusage(RUSAGE_SELF, &usage) == 0, ErrorCode::Storage, "Cannot observe process resources");
            auto* resources = json_object_new_object(); json_object_object_add(output.get(), "resources", resources);
            string(resources, "profile", "gammathink.application-resource-snapshot.v1");
            string(resources, "scope", "current-command-process");
            json_object_object_add(resources, "process_resident_bytes", json_object_new_uint64(process.resident_bytes));
            json_object_object_add(resources, "process_peak_rss_bytes", json_object_new_uint64(process.lifetime_peak_bytes));
            json_object_object_add(resources, "process_user_cpu_microseconds", json_object_new_uint64(
                static_cast<std::uint64_t>(usage.ru_utime.tv_sec) * 1000000 + usage.ru_utime.tv_usec));
            json_object_object_add(resources, "process_system_cpu_microseconds", json_object_new_uint64(
                static_cast<std::uint64_t>(usage.ru_stime.tv_sec) * 1000000 + usage.ru_stime.tv_usec));
            json_object_object_add(resources, "disk_capacity_bytes", json_object_new_uint64(disk.capacity));
            json_object_object_add(resources, "disk_free_bytes", json_object_new_uint64(disk.free));
            json_object_object_add(resources, "disk_available_bytes", json_object_new_uint64(disk.available));
            json_object_object_add(resources, "other_processes_included", json_object_new_boolean(false));
            json_object_object_add(resources, "aggregate_memory_qualified", json_object_new_boolean(false));
            json_object_object_add(output.get(), "model_loaded", json_object_new_boolean(false));
            json_object_object_add(output.get(), "model_generations", json_object_new_int(0));
            json_object_object_add(output.get(), "authority_changed", json_object_new_boolean(false));
        } else if (mode == "pin") {
            const auto pin = memory.revise_constraint(session, {text(command.get(), "key", 256), text(command.get(), "text"),
                0, static_cast<std::uint64_t>(number(command.get(), "source_message", 1)), boolean(command.get(), "active")},
                static_cast<std::uint64_t>(number(command.get(), "constraint_revision")));
            json_object_object_add(output.get(), "constraint_revision", json_object_new_uint64(pin.revision));
        } else if (mode == "snapshot-journal") {
            (void)memory.recent(session, 1);
            const auto snapshot = compile_snapshot(journal, text(command.get(), "destination", 4096),
                text(command.get(), "name", 128), text(command.get(), "revision", 128),
                snapshot_limits(field(command.get(), "limits", false)));
            string(output.get(), "root", hex(snapshot.root));
            json_object_object_add(output.get(), "objects", json_object_new_uint64(snapshot.objects));
            json_object_object_add(output.get(), "input_bytes", json_object_new_uint64(snapshot.input_bytes));
            json_object_object_add(output.get(), "output_bytes", json_object_new_uint64(snapshot.output_bytes));
            json_object_object_add(output.get(), "published", json_object_new_boolean(false));
        } else if (mode == "mount") {
            (void)memory.recent(session, 1);
            const auto alias = text(command.get(), "alias", 128);
            const auto generation = static_cast<std::uint64_t>(number(command.get(), "generation"));
            require(registry.resolve(alias).generation == generation, ErrorCode::Unavailable, "Mount generation changed");
            const auto path = std::filesystem::canonical(text(command.get(), "directory", 4096));
            const auto version = registry.register_snapshot(path, parse_block_id(text(command.get(), "root", 64)));
            mount_result(output.get(), registry.mount(alias, version.name, version.revision, path, generation));
        } else if (mode == "mount-remote") {
            (void)memory.recent(session, 1);
            require(bool(network), ErrorCode::Permission, "Remote mount requires explicit host source capabilities");
            const auto alias = text(command.get(), "alias", 128);
            const auto generation = static_cast<std::uint64_t>(number(command.get(), "generation"));
            require(registry.resolve(alias).generation == generation, ErrorCode::Unavailable, "Mount generation changed");
            const auto backend = text(command.get(), "backend", 32);
            require(backend == "blocks" || backend == "query", ErrorCode::Schema, "Unknown remote read backend");
            const auto key = text(command.get(), "source", 64);
            const auto access = network->access();
            if (backend == "query") (void)access.queries(key);
            const auto blocks = access.blocks(key);
            const auto version = registry.register_snapshot(blocks, parse_block_id(text(command.get(), "root", 64)));
            mount_result(output.get(), registry.mount_remote(alias, version.name, version.revision,
                backend == "blocks" ? MountBackend::RemoteBlocks : MountBackend::RemoteQuery, key, blocks, generation));
        } else if (mode == "unmount") {
            (void)memory.recent(session, 1);
            mount_result(output.get(), registry.unmount(text(command.get(), "alias", 128),
                static_cast<std::uint64_t>(number(command.get(), "generation"))));
        } else if (mode == "mounts") {
            (void)memory.recent(session, 1);
            auto* mounts = json_object_new_array(); json_object_object_add(output.get(), "mounts", mounts);
            for (const auto& mount : registry.mounts()) {
                auto* value = json_object_new_object(); mount_result(value, mount); json_object_array_add(mounts, value);
            }
            json_object_object_add(output.get(), "network_authorized", json_object_new_boolean(false));
        } else if (mode == "hypothesize" || mode == "propose-experiment" || mode == "measure-experiment" || mode == "review-experiment") {
            (void)memory.recent(session, 1);
            neural::NativeModelManager model;
            TaskSession controller(memory, tasks, journal, model);
            ExperimentRunner experiments(journal, model);
            if (mode == "hypothesize") {
                const auto goal = parse_id(text(command.get(), "goal", 64));
                const auto assessment = journal.assessment(goal);
                require(!assessment || (assessment->state != KnowledgeState::Invalidated &&
                    assessment->logical != LogicalValidity::Invalid), ErrorCode::Integrity, "Hypothesis source requires revalidation");
                const auto input = journal.get(goal);
                require(input.has_value(), ErrorCode::Unavailable, "Formal hypothesis source is unavailable");
                const auto report = propose_hypotheses(*input);
                std::vector<ObjectId> hypotheses;
                for (const auto& value : report.hypotheses) hypotheses.push_back(journal.put(value));
                json_object_object_add(output.get(), "hypotheses", identity_list(hypotheses));
                auto* alternatives = json_object_new_array(); json_object_object_add(output.get(), "alternatives", alternatives);
                for (const auto& plan : report.alternatives) {
                    auto* value = json_object_new_object(); json_object_array_add(alternatives, value);
                    json_object_object_add(value, "jointly_required", identity_list(plan.jointly_required));
                    auto* rules = json_object_new_array(); json_object_object_add(value, "source_rules", rules);
                    for (const auto rule : plan.source_rules) json_object_array_add(rules, json_object_new_uint64(rule));
                    json_object_object_add(value, "contradicts_known", json_object_new_boolean(plan.contradicts_known));
                }
                json_object_object_add(output.get(), "formally_supported", json_object_new_boolean(report.formally_supported));
                json_object_object_add(output.get(), "existing_contradiction", json_object_new_boolean(report.existing_contradiction));
                json_object_object_add(output.get(), "limited", json_object_new_boolean(report.limited));
                json_object_object_add(output.get(), "work", json_object_new_uint64(report.work));
                json_object_object_add(output.get(), "empirical_truth_verified", json_object_new_boolean(false));
            } else if (mode == "propose-experiment") {
                auto* selected = field(command.get(), "hypotheses");
                require(json_object_is_type(selected, json_type_array) && json_object_array_length(selected) > 0 &&
                    json_object_array_length(selected) <= 64, ErrorCode::Budget, "Expected bounded joint hypothesis references");
                std::vector<ObjectId> hypotheses;
                for (std::size_t index = 0; index < json_object_array_length(selected); ++index) {
                    auto* value = json_object_array_get_idx(selected, index);
                    require(value && json_object_is_type(value, json_type_string), ErrorCode::Schema, "Expected hypothesis identity");
                    hypotheses.push_back(parse_id(json_object_get_string(value)));
                }
                auto* protocol = field(command.get(), "protocol");
                keys(protocol, {"name", "revision", "instrument", "expected", "trials"});
                ExperimentProtocol input{text(protocol, "name", 128), text(protocol, "revision", 128),
                    text(protocol, "instrument", 128), predicates(field(protocol, "expected")),
                    field(protocol, "trials", false) ? static_cast<std::size_t>(number(protocol, "trials", 1, 64)) : 1};
                string(output.get(), "experiment", hex(experiments.propose(hypotheses, input)));
                json_object_object_add(output.get(), "observation_authorized", json_object_new_boolean(false));
            } else {
                const auto experiment = parse_id(text(command.get(), "experiment", 64));
                ExperimentReview result;
                if (mode == "measure-experiment") {
                    auto* scope = field(command.get(), "observation_scope");
                    keys(scope, {"instrument", "limits"});
                    require(text(scope, "instrument", 128) == "process-memory-v1", ErrorCode::Permission,
                        "Only the explicit native process-memory instrument is available");
                    ExperimentLimits limits;
                    if (auto* budget = field(scope, "limits", false)) {
                        keys(budget, {"encoded_bytes", "seconds", "resident_bytes"});
                        if (field(budget, "encoded_bytes", false)) limits.encoded_bytes =
                            static_cast<std::size_t>(number(budget, "encoded_bytes", 1, 16 * 1024 * 1024));
                        if (field(budget, "seconds", false)) limits.time =
                            std::chrono::seconds(number(budget, "seconds", 1, 300));
                        if (field(budget, "resident_bytes", false)) limits.resident_bytes =
                            static_cast<std::uint64_t>(number(budget, "resident_bytes", 1, 2999999999ULL));
                    }
                    auto permit = experiments.authorize(experiment, std::make_shared<ProcessMemoryInstrument>(), limits);
                    result = experiments.run(permit);
                } else result = experiments.review(experiment, parse_id(text(command.get(), "receipt", 64)));
                experiment_result(output.get(), result);
                if (mode == "review-experiment") {
                    auto* observations = json_object_new_array(); json_object_object_add(output.get(), "recorded_observations", observations);
                    for (const auto& id : result.observations) {
                        const auto record = journal.get(id).value();
                        auto value = parse(std::string(record.payload.begin(), record.payload.end()));
                        auto* item = json_object_new_object(); json_object_array_add(observations, item);
                        string(item, "id", hex(id)); json_object_object_add(item, "values", value.release());
                        string(item, "instrument", record.sources.front().archive_id);
                        string(item, "trial", record.sources.front().entry_path);
                        string(item, "digest", hex(ObjectId{record.sources.front().source_digest}));
                    }
                    json_object_object_add(output.get(), "historical_observation", json_object_new_boolean(true));
                    json_object_object_add(output.get(), "authority_changed", json_object_new_boolean(false));
                }
            }
            json_object_object_add(output.get(), "model_loaded", json_object_new_boolean(model.state() != neural::ModelState::Unloaded));
            json_object_object_add(output.get(), "logical_certificates", json_object_new_array());
        } else if (mode == "read-artifact") {
            (void)memory.recent(session, 1);
            const auto id = parse_id(text(command.get(), "id", 64));
            const auto artifact = journal.get(id);
            require(artifact && artifact->visibility == Visibility::Private && artifact->kind == ObjectKind::Evidence &&
                (artifact->format == "gammathink.task-inspection-view.v1" ||
                    artifact->format == "gammathink.publication-request.v1" ||
                    artifact->format == "gammathink.source-translation-candidate.v1" ||
                    artifact->format == "gammathink.source-translation-prompt.v1" ||
                    artifact->format == "gammathink.source-translation-model.v1" ||
                    artifact->format == "gammathink.source-translation-candidate.v2" ||
                    artifact->format == "gammathink.source-translation-prompt.v2" ||
                    artifact->format == "gammathink.source-translation-model.v2"),
                ErrorCode::Schema, "Expected a private review or translation artifact");
            auto view = parse(std::string(artifact->payload.begin(), artifact->payload.end()), 16 * 1024 * 1024);
            string(output.get(), "artifact", hex(id)); string(output.get(), "artifact_format", artifact->format);
            json_object_object_add(output.get(), "historical_observation", json_object_new_boolean(true));
            json_object_object_add(output.get(), "publication_approved", json_object_new_boolean(false));
            json_object_object_add(output.get(), "payload", view.release());
        } else if (mode == "read-source") {
            // Archive paths are host command fields, never interpreted from raw
            // library text or selected by the model's Task IR.
            (void)memory.recent(session, 1);
            const ZimSourceArchive archive(text(command.get(), "archive", 4096));
            const auto source = archive.read(text(command.get(), "entry", 4096));
            auto context = host_context(command.get());
            require(context.views.empty(), ErrorCode::Schema, "Source import requires a host registry without views");
            const auto& reference = source.sources.front();
            context.views.push_back(make_evidence_view("source", PremiseBasis::Source,
                std::string(source.payload.begin(), source.payload.end()), reference.archive_id,
                reference.entry_path, reference.extractor_version));
            const ProblemSourceCatalog catalog(context);
            auto captured = parse(serialize_problem_context(context));
            const auto id = journal.put(source);
            auto* descriptor = json_object_new_object();
            string(descriptor, "id", hex(id)); string(descriptor, "kind", name(source.kind));
            string(descriptor, "format", source.format);
            json_object_object_add(output.get(), "source", descriptor);
            json_object_object_add(output.get(), "source_context", captured.release());
            string(output.get(), "context_snapshot", catalog.snapshot());
        } else if (mode == "capture-structured") {
            const auto history = field(command.get(), "history_query", false)
                ? text(command.get(), "history_query", 4096) : "";
            const auto sources = documents ? retrieve_document_context(memory, *documents, session, {}, history,
                field(command.get(), "document_query", false) ? text(command.get(), "document_query", 1024) : "")
                : retrieve_context(memory, session, {}, history);
            const auto captured = prepare_task_prompt(sources, host_context(command.get()),
                [](std::string_view value) { return value.size(); }, {1048576, 262144, 256},
                objects(field(command.get(), "objects", false)), predicates(field(command.get(), "active_constraints", false)));
            string(output.get(), "counter", "utf8-bytes-structured-only");
            json_object_object_add(output.get(), "model_calls", json_object_new_int(0));
            json_object_object_add(output.get(), "source_message", json_object_new_uint64(sources.current.id));
            string(output.get(), "snapshot", captured.context.snapshot());
            auto context = parse(captured.context.wire());
            auto prompt = parse(captured.input);
            json_object_object_add(output.get(), "context", context.release());
            json_object_object_add(output.get(), "translation_input", prompt.release());
        } else if (mode == "history") {
            const auto page = tasks.history(session, field(command.get(), "after", false)
                ? static_cast<std::uint64_t>(number(command.get(), "after")) : 0,
                field(command.get(), "limit", false) ? static_cast<std::size_t>(number(command.get(), "limit", 1, 256)) : 64);
            auto* events = json_object_new_array(); json_object_object_add(output.get(), "events", events);
            for (const auto& task : page.events) {
                auto* event = json_object_new_object(); task_result(event, task); json_object_array_add(events, event);
            }
            json_object_object_add(output.get(), "complete", json_object_new_boolean(page.complete));
            json_object_object_add(output.get(), "next", json_object_new_uint64(page.next));
        } else if (mode == "recall") {
            const auto after = field(command.get(), "after", false)
                ? static_cast<std::uint64_t>(number(command.get(), "after")) : 0;
            const auto page = memory.search(session, text(command.get(), "query", 4096), after, field(command.get(), "limit", false)
                ? static_cast<std::size_t>(number(command.get(), "limit", 1, 256)) : 16);
            auto* messages = json_object_new_array(); json_object_object_add(output.get(), "messages", messages);
            for (const auto& message : page.messages) {
                auto* row = json_object_new_object(); json_object_array_add(messages, row);
                json_object_object_add(row, "id", json_object_new_uint64(message.id));
                json_object_object_add(row, "role", json_object_new_int(static_cast<int>(message.role)));
                string(row, "text", message.content);
            }
            json_object_object_add(output.get(), "complete", json_object_new_boolean(page.complete));
            json_object_object_add(output.get(), "next",
                json_object_new_uint64(page.messages.empty() ? after : page.messages.back().id));
        } else {
            neural::NativeModelManager model(model_limits(field(command.get(), "model_limits", false)));
            TaskSession controller(memory, tasks, journal, model);
            const auto revision = static_cast<std::uint64_t>(number(command.get(), "revision"));
            if (mode == "admit" || mode == "interpret" || mode == "refine") {
                PlanSessionAcceptance result = [&] {
                    if (mode == "refine") return controller.refine_plan(session, revision,
                        restore_task_plan_context(wire(field(command.get(), "context"))),
                        text(command.get(), "snapshot", 256), text(command.get(), "proposal", 262144));
                    if (mode == "admit") {
                        const auto context = restore_task_plan_context(wire(field(command.get(), "context")));
                        return controller.accept_plan(session, static_cast<std::uint64_t>(number(command.get(), "source_message", 1)),
                            context, text(command.get(), "snapshot", 256), text(command.get(), "proposal", 262144), revision);
                    }
                    const auto latest = memory.recent(session, 1);
                    require(latest.messages.size() == 1 && latest.messages[0].role == Role::User,
                        ErrorCode::Schema, "Interpretation requires the current original user message");
                    TaskInterpretationOptions options;
                    options.objects = objects(field(command.get(), "objects", false));
                    options.active_constraints = predicates(field(command.get(), "active_constraints", false));
                    options.documents = documents.get();
                    if (field(command.get(), "document_query", false))
                        options.document_phrase = text(command.get(), "document_query", 1024);
                    if (field(command.get(), "history_query", false)) options.history_phrase = text(command.get(), "history_query", 4096);
                    if (field(command.get(), "output_tokens", false)) options.output_tokens =
                        static_cast<std::uint32_t>(number(command.get(), "output_tokens", 1, 32767));
                    return controller.interpret_plan(session,
                        model_spec(field(command.get(), "model"), neural::ModelRole::Interpreter),
                        host_context(command.get()), revision, options);
                }();
                if (const auto* task = std::get_if<SessionTask>(&result)) {
                    task_result(output.get(), *task);
                    const auto plan = restore_task_plan(journal, task->ordered_task);
                    auto context = parse(plan.context_wire()); json_object_object_add(output.get(), "context", context.release());
                } else {
                    const auto& decline = std::get<TaskPlanDecline>(result);
                    string(output.get(), "status", "declined"); string(output.get(), "reason", decline.reason);
                    json_object_object_add(output.get(), "decline_kind", json_object_new_int(static_cast<int>(decline.kind)));
                    string(output.get(), "snapshot", decline.snapshot);
                }
            } else {
                const auto context = restore_task_plan_context(wire(field(command.get(), "context")));
                auto environment = field(command.get(), "environment", false)
                    ? parse_task_environment(wire(field(command.get(), "environment"))) : GoalConditions{};
                const bool enabled = network && network->network_enabled();
                if (const auto declared = environment.find("network"); declared != environment.end())
                    require(std::get_if<bool>(&declared->second) && std::get<bool>(declared->second) == enabled,
                        ErrorCode::Permission, "Declared network observation differs from actual access");
                environment.insert_or_assign("network", enabled);
                const auto action_id = text(command.get(), "action", 64);
                const auto assumptions = accepted(command.get());
                const auto contexts = source_contexts(command.get());
                const auto dispatch_plan = mode == "deliver" ? controller.review_plan(session, revision, context)
                    : controller.active_plan(session, revision, context);
                require(std::any_of(dispatch_plan.actions().begin(), dispatch_plan.actions().end(),
                    [&](const auto& action) { return action.id == action_id; }),
                    ErrorCode::Schema, "Unknown admitted action");
                const auto task = tasks.active(session).value();
                const auto recorded = tasks.completed_actions(session, task.ordered_task, revision);
                const auto gate = evaluate_task_plan(context, dispatch_plan, {}, environment, {}, assumptions);
                for (const auto& action : dispatch_plan.actions()) {
                    if (action.id != action_id && std::none_of(recorded.begin(), recorded.end(),
                        [&](const auto& marker) { return marker.action == action.id; })) continue;
                    const auto goal = std::find_if(dispatch_plan.graph().nodes.begin(), dispatch_plan.graph().nodes.end(),
                        [&](const auto& node) { return node.id == action.goal; });
                    const auto state = gate.goals.states.at(static_cast<std::size_t>(goal - dispatch_plan.graph().nodes.begin()));
                    require(state != GoalState::ConstraintBlocked && state != GoalState::AssumptionBlocked,
                        ErrorCode::Permission, "Task policy blocks external provider access");
                }
                // Admission and interpretation do not read mounted archives.
                // Execution/recovery validate live task policy before even a
                // remote manifest read; environment text cannot hide egress.
                VirtualDrive drive(journal, registry, {}, network ? network->access() : DriveAccess{});
                const auto view = drive.open_view();
                auto& knowledge = view->knowledge();
                if (mode == "deliver") {
                    const auto reply = controller.deliver_native_response(session, revision, context, action_id, knowledge,
                        environment, text(command.get(), "timestamp", 256), assumptions, contexts);
                    json_object_object_add(output.get(), "message", json_object_new_uint64(reply.id));
                    auto body = parse(reply.content); json_object_object_add(output.get(), "response", body.release());
                } else if (mode == "translate-source") {
                    auto state = controller.restore_native_execution(session, revision, context, knowledge, environment, assumptions, contexts);
                    SourceTranslationOptions options;
                    if (field(command.get(), "output_tokens", false)) options.output_tokens =
                        static_cast<std::uint32_t>(number(command.get(), "output_tokens", 1, 32767));
                    const auto candidate = controller.translate_source_action(session, revision, context, action_id, state, environment,
                        source_scope(field(command.get(), "source_scope")),
                        model_spec(field(command.get(), "model"), neural::ModelRole::KnowledgeExtraction), assumptions, options);
                    task_result(output.get(), tasks.active(session).value());
                    string(output.get(), "candidate", hex(candidate.candidate)); string(output.get(), "prompt", hex(candidate.prompt));
                    string(output.get(), "model", hex(candidate.model)); string(output.get(), "archive_dsl", candidate.archive_dsl);
                    json_object_object_add(output.get(), "generation_complete", json_object_new_boolean(candidate.complete));
                    json_object_object_add(output.get(), "input_tokens", json_object_new_uint64(candidate.input_tokens));
                    json_object_object_add(output.get(), "output_tokens", json_object_new_uint64(candidate.output_tokens));
                    json_object_object_add(output.get(), "model_generations", json_object_new_int(1));
                    json_object_object_add(output.get(), "action_completed", json_object_new_boolean(false));
                    json_object_object_add(output.get(), "source_translation_qualified", json_object_new_boolean(false));
                    json_object_object_add(output.get(), "model_loaded", json_object_new_boolean(model.state() != neural::ModelState::Unloaded));
                } else if (mode == "review-source") {
                    const auto plan = controller.active_plan(session, revision, context);
                    const auto action = std::find_if(plan.actions().begin(), plan.actions().end(),
                        [&](const auto& value) { return value.id == action_id; });
                    require(action != plan.actions().end() && action->kind == TaskActionKind::CompileSource,
                        ErrorCode::Schema, "Source review requires an admitted compilation action");
                    const auto state = controller.restore_native_execution(session, revision, context, knowledge,
                        environment, assumptions, contexts);
                    require(state.steps.contains(action_id), ErrorCode::Permission, "Source compilation has not completed");
                    const auto& step = state.steps.at(action_id);
                    const auto receipt = journal.get(step.receipt);
                    require(receipt.has_value(), ErrorCode::Missing, "Source review receipt is unavailable");
                    auto captured = parse(std::string(receipt->payload.begin(), receipt->payload.end()));
                    const auto source_id = parse_id(text(captured.get(), "source", 64));
                    const auto source = journal.get(source_id);
                    require(source.has_value(), ErrorCode::Missing, "Complete source is unavailable for review");
                    const auto dsl = text(captured.get(), "archive_dsl", 1024 * 1024);
                    const auto translated = compile_source(*source, source_id, dsl);
                    auto* review = json_object_new_object();
                    json_object_object_add(output.get(), "review", review);
                    string(review, "source", hex(source_id)); string(review, "format", source->format);
                    string(review, "complete_source", std::string(source->payload.begin(), source->payload.end()));
                    string(review, "archive_dsl", dsl); string(review, "reason", translated.reason);
                    string(review, "receipt", hex(step.receipt));
                    string(review, "receipt_format", receipt->format);
                    json_object_object_add(review, "kind", json_object_new_int(static_cast<int>(translated.kind)));
                    json_object_object_add(review, "source_translation_qualified", json_object_new_boolean(false));
                    json_object_object_add(review, "empirical_truth_verified", json_object_new_boolean(false));
                    json_object_object_add(review, "authority_changed", json_object_new_boolean(false));
                    auto host = parse(text(captured.get(), "host_context", 4 * 1024 * 1024));
                    json_object_object_add(review, "host_context", host.release());
                    if (!translated.parsed_json.empty()) {
                        auto parsed = parse(translated.parsed_json);
                        json_object_object_add(review, "parsed_json", parsed.release());
                    }
                    if (step.outcome.succeeded) string(review, "module", hex(step.artifact));
                    auto* provenance = json_object_new_array(); json_object_object_add(review, "provenance", provenance);
                    for (const auto& reference : source->sources) {
                        auto* row = json_object_new_object(); json_object_array_add(provenance, row);
                        string(row, "archive", reference.archive_id); string(row, "version", reference.archive_version);
                        string(row, "entry", reference.entry_path); string(row, "extractor", reference.extractor_version);
                        string(row, "digest", hex(ObjectId{reference.source_digest}));
                        json_object_object_add(row, "offset", json_object_new_uint64(reference.offset));
                        json_object_object_add(row, "length", json_object_new_uint64(reference.length));
                    }
                } else {
                    const auto plan = controller.active_plan(session, revision, context);
                    const auto action = std::find_if(plan.actions().begin(), plan.actions().end(),
                        [&](const auto& value) { return value.id == action_id; });
                    require(action != plan.actions().end(), ErrorCode::Schema, "Unknown admitted action");
                    require(action->kind == TaskActionKind::Visualize || !field(command.get(), "inspection_limits", false),
                        ErrorCode::Schema, "This action does not consume inspection bounds");
                    auto state = controller.restore_native_execution(session, revision, context, knowledge, environment,
                        assumptions, contexts);
                    NativeTaskStep result;
                    if (action->kind == TaskActionKind::Visualize || action->kind == TaskActionKind::RequestPublication) {
                        require(!field(command.get(), "execution_scope", false) && !field(command.get(), "source_scope", false) &&
                            !field(command.get(), "model", false) && !field(command.get(), "model_limits", false) &&
                            !field(command.get(), "output_tokens", false), ErrorCode::Schema,
                            "Private review does not consume model, compilation or execution capabilities");
                        result = controller.review_action(session, revision, context, action_id, state, knowledge, environment,
                            assumptions, inspection_limits(field(command.get(), "inspection_limits", false)));
                    } else if (action->kind == TaskActionKind::CompileSource) {
                        require(!field(command.get(), "execution_scope", false) && !field(command.get(), "model", false) &&
                            !field(command.get(), "model_limits", false) && !field(command.get(), "output_tokens", false),
                            ErrorCode::Schema, "Source materialization does not consume model or execution capabilities");
                        result = controller.compile_source_action(session, revision, context, action_id, state, environment,
                            source_scope(field(command.get(), "source_scope")), assumptions);
                    } else if (action->kind == TaskActionKind::GenerateCode) {
                        require(!field(command.get(), "execution_scope", false) && !field(command.get(), "source_scope", false),
                            ErrorCode::Schema, "Code generation does not consume execution or source compilation capabilities");
                        CodeGenerationOptions options;
                        if (field(command.get(), "output_tokens", false)) options.output_tokens =
                            static_cast<std::uint32_t>(number(command.get(), "output_tokens", 1, 32767));
                        result = controller.generate_code_action(session, revision, context, action_id, state, environment,
                            model_spec(field(command.get(), "model"), neural::ModelRole::CodeGeneration), assumptions, options);
                    } else if (action->kind == TaskActionKind::ExecuteCode) {
                        require(!field(command.get(), "source_scope", false), ErrorCode::Schema,
                            "Code execution does not consume source compilation inputs");
                        // This separate host command field is never read from Task IR.
                        auto [tests, limits] = execution_scope(field(command.get(), "execution_scope"));
                        std::optional<CodeRepairOptions> repair;
                        if (field(command.get(), "model", false)) {
                            require(action->inputs.size() == 2, ErrorCode::Schema,
                                "Exact-source execution does not consume a repair model");
                            repair.emplace(); repair->model =
                                model_spec(field(command.get(), "model"), neural::ModelRole::CodeGeneration);
                            if (field(command.get(), "output_tokens", false)) repair->generation.output_tokens =
                                static_cast<std::uint32_t>(number(command.get(), "output_tokens", 1, 32767));
                        }
                        require(action->inputs.size() != 2 || limits.attempts == 1 || repair.has_value(),
                            ErrorCode::Schema, "Multiple repair attempts require an explicit host model");
                        require(repair || !field(command.get(), "output_tokens", false), ErrorCode::Schema,
                            "Execution output reservation requires a repair model");
                        controller.configure_execution({"/Library/Developer/CommandLineTools/usr/bin/clang++",
                            "/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk", execution_helper()});
                        auto permit = controller.permit_code_action(session, revision, context, action_id, state,
                            environment, std::move(tests), assumptions, limits);
                        result = controller.execute_code_action(session, permit.revision(), context, action_id, state,
                            permit, knowledge, environment, assumptions, repair);
                    } else {
                        require(!field(command.get(), "execution_scope", false) && !field(command.get(), "model", false) &&
                            !field(command.get(), "model_limits", false) && !field(command.get(), "output_tokens", false) &&
                            !field(command.get(), "source_scope", false),
                            ErrorCode::Schema, "Native action does not consume code or model capabilities");
                        result = controller.execute_native_action(session, revision, context, action_id, state, knowledge,
                            environment, assumptions, contexts);
                    }
                    task_result(output.get(), tasks.active(session).value());
                    string(output.get(), "receipt", hex(result.receipt)); string(output.get(), "artifact", hex(result.artifact));
                    json_object_object_add(output.get(), "operation_status", json_object_new_int(static_cast<int>(result.status)));
                    json_object_object_add(output.get(), "succeeded", json_object_new_boolean(result.outcome.succeeded));
                    json_object_object_add(output.get(), "native_executions", json_object_new_uint64(result.native_executions));
                }
                json_object_object_add(output.get(), "model_loaded",
                    json_object_new_boolean(model.state() != neural::ModelState::Unloaded));
            }
        }
        if (network) network_result(output.get(), *network, network_keys);
        std::cout << json_object_to_json_string_ext(output.get(), JSON_C_TO_STRING_PLAIN) << '\n';
        return 0;
    } catch (const std::exception& error) {
        Json output(json_object_new_object(), json_object_put);
        string(output.get(), "profile", "gammathink.session-result.v1");
        string(output.get(), "status", "error"); string(output.get(), "error", error.what());
        std::cout << json_object_to_json_string_ext(output.get(), JSON_C_TO_STRING_PLAIN) << '\n';
        return 1;
    }
}
