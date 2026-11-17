#include "task_ir.hpp"
#include "problem_semantic.hpp"
#include "problem_wire.hpp"
#include "binary_dispatch.hpp"
#include "knowledge_reasoner.hpp"
#include "virtual_drive.hpp"
#include "source_ingestion.hpp"
#include "publisher_catalogue.hpp"
#include "source_reasoner.hpp"
#include "bound_sources.hpp"
#include "parallel_reasoning.hpp"

#include <fstream>
#include <iostream>
#include <json-c/json.h>
#include <memory>

namespace {
using namespace axiom;
using namespace axiom::knowledge;
using Json = std::unique_ptr<json_object, decltype(&json_object_put)>;
std::string read_input(const char* path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot open runtime input");
    std::string result(1048576, '\0');
    input.read(result.data(), static_cast<std::streamsize>(result.size()));
    const auto count = input.gcount();
    if (input.bad() || (!input.eof() && input.peek() != std::char_traits<char>::eof())) {
        throw std::runtime_error("Runtime input exceeds complete-input byte budget");
    }
    result.resize(static_cast<std::size_t>(count));
    return result;
}
void text(json_object* object, const char* key, std::string_view value) {
    json_object_object_add(object, key, json_object_new_string_len(value.data(), static_cast<int>(value.size())));
}
Json json_input(std::string_view input, json_type expected = json_type_object) {
    const auto parser = std::unique_ptr<json_tokener, decltype(&json_tokener_free)>(
        json_tokener_new_ex(64), json_tokener_free);
    if (!parser) throw Error(ErrorCode::Budget, "Cannot allocate source capture parser");
    json_tokener_set_flags(parser.get(), JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
    Json result(json_tokener_parse_ex(parser.get(), input.data(), static_cast<int>(input.size())), json_object_put);
    if (!result || json_tokener_get_error(parser.get()) != json_tokener_success ||
        json_object_get_type(result.get()) != expected) {
        throw Error(ErrorCode::Schema, "Expected a complete runtime JSON value of the requested type");
    }
    for (auto position = json_tokener_get_parse_end(parser.get()); position < input.size(); ++position) {
        const auto value = input[position];
        if (value != ' ' && value != '\n' && value != '\r' && value != '\t') {
            throw Error(ErrorCode::Schema, "Trailing source capture data");
        }
    }
    return result;
}
Atom source_atom(json_object* value) {
    const auto array = [](json_object* input, std::size_t count) {
        if (!input || !json_object_is_type(input, json_type_array) || json_object_array_length(input) != count)
            throw Error(ErrorCode::Schema, "Invalid source query tuple");
    };
    const auto tag = [](json_object* input, std::string_view expected) {
        if (!input || !json_object_is_type(input, json_type_string)
            || std::string_view(json_object_get_string(input), json_object_get_string_len(input)) != expected)
            throw Error(ErrorCode::Schema, "Invalid source query tag");
    };
    const auto integer = [](json_object* input) {
        if (!input || !json_object_is_type(input, json_type_int) || json_object_get_int64(input) < 0
            || json_object_get_uint64(input) > UINT32_MAX)
            throw Error(ErrorCode::Schema, "Invalid source query index");
        return static_cast<std::uint32_t>(json_object_get_uint64(input));
    };
    array(value, 2); tag(json_object_array_get_idx(value, 0), "atom");
    auto* atom = json_object_array_get_idx(value, 1); array(atom, 3);
    auto* sign = json_object_array_get_idx(atom, 1);
    auto* terms = json_object_array_get_idx(atom, 2);
    if (!sign || !json_object_is_type(sign, json_type_boolean) || !terms
        || !json_object_is_type(terms, json_type_array) || json_object_array_length(terms) > 4)
        throw Error(ErrorCode::Schema, "Invalid ground source query atom");
    Atom result{integer(json_object_array_get_idx(atom, 0)), json_object_get_boolean(sign) != 0, {}};
    for (std::size_t i = 0; i < json_object_array_length(terms); ++i) {
        auto* term = json_object_array_get_idx(terms, i); array(term, 2);
        tag(json_object_array_get_idx(term, 0), "c");
        result.terms.push_back({TermKind::Constant, integer(json_object_array_get_idx(term, 1))});
    }
    return result;
}
ProblemGoal source_goal(json_object* value) {
    auto* tag = json_object_array_get_idx(value, 0);
    if (!tag || !json_object_is_type(tag, json_type_string) ||
        std::string_view(json_object_get_string(tag), json_object_get_string_len(tag)) != "q")
        return ProblemAtomGoal{source_atom(value)};
    if (json_object_array_length(value) != 3) throw Error(ErrorCode::Schema, "Invalid source quantity goal tuple");
    const auto index = [&](std::size_t position) {
        auto* item = json_object_array_get_idx(value, position);
        if (!item || !json_object_is_type(item, json_type_int) ||
            json_object_get_int64(item) < 0 || json_object_get_uint64(item) > UINT32_MAX)
            throw Error(ErrorCode::Schema, "Invalid source quantity goal index");
        return static_cast<std::uint32_t>(json_object_get_uint64(item));
    };
    return ProblemQuantityGoal{index(1), index(2)};
}
std::string field(json_object* object, const char* key) {
    const auto value = json_object_object_get(object, key);
    if (!value || json_object_get_type(value) != json_type_string) {
        throw Error(ErrorCode::Schema, "Source capture is missing a text field");
    }
    return {json_object_get_string(value), static_cast<std::size_t>(json_object_get_string_len(value))};
}
std::string_view status(KnowledgeRunStatus value) {
    switch (value) {
    case KnowledgeRunStatus::Checked: return "checked";
    case KnowledgeRunStatus::Unresolved: return "unresolved";
    case KnowledgeRunStatus::Conflict: return "conflict";
    case KnowledgeRunStatus::DependenciesUnavailable: return "dependencies_unavailable";
    case KnowledgeRunStatus::RevalidationRequired: return "revalidation_required";
    case KnowledgeRunStatus::ResourceLimited: return "resource_limited";
    }
    throw std::runtime_error("Unknown knowledge runtime status");
}
}

int main(int argc, char** argv) {
    const auto mode = argc > 1 ? std::string_view(argv[1]) : std::string_view{};
    if (!((argc == 5 && (mode == "solve" || mode == "source" || mode == "resume-catalogue")) ||
        (argc == 6 && (mode == "resume" || mode == "snapshot")) || (argc == 7 && mode == "ingest")
        || (argc == 8 && mode == "query-source") || ((argc == 9 || argc == 11) && mode == "query-bound")
        || (argc == 3 && mode == "prove-batch"))) {
        std::cerr << "gammathink solve CONTEXT_JSON INTERPRETATION_DSL JOURNAL_DB\n"
            "gammathink snapshot JOURNAL_DB NEW_DIRECTORY NAME REVISION\n"
            "gammathink resume NATIVE_GOAL_ID JOURNAL_DB SNAPSHOT_DIRECTORY ROOT_HASH\n"
            "gammathink resume-catalogue NATIVE_GOAL_ID JOURNAL_DB ACCEPTED_CATALOGUE_DB\n"
            "gammathink query-source MODULE_ID HOST_CONTEXT_JSON ATOM_JSON JOURNAL_DB SNAPSHOT_DIRECTORY ROOT_HASH\n"
            "gammathink query-bound BINDING_ID HOST_CONTEXTS_JSON GOAL_MODULE_ID GOAL_JSON JOURNAL_DB SNAPSHOT_DIRECTORY ROOT_HASH --accept-binding BINDING_ID\n"
            "gammathink source ZIM ENTRY_PATH UNIT_CONTEXT_JSON\n"
            "gammathink ingest ZIM ENTRY_PATH SOURCE_RECORD_JSON ARCHIVE_DSL JOURNAL_DB\n"
            "gammathink prove-batch HOST_MANIFEST_JSON\n";
        return 2;
    }
    try {
        Json response(json_object_new_object(), json_object_put);
        json_object_object_add(response.get(), "english_meaning_certified", json_object_new_boolean(false));
        json_object_object_add(response.get(), "empirical_truth_certified", json_object_new_boolean(false));
        json_object_object_add(response.get(), "neural_calls", json_object_new_int(0));
        if (mode == "prove-batch") {
            const auto manifest = json_input(read_input(argv[2]));
            if (!json_object_is_type(manifest.get(), json_type_object))
                throw Error(ErrorCode::Schema, "Expected a host proof batch manifest");
            json_object_object_foreach(manifest.get(), name, value) {
                (void)value;
                if (std::string_view(name) != "version" && std::string_view(name) != "workers" &&
                    std::string_view(name) != "requests")
                    throw Error(ErrorCode::Schema, "Unknown proof batch manifest field");
            }
            const auto* version = json_object_object_get(manifest.get(), "version");
            if (!version || !json_object_is_type(version, json_type_int) ||
                json_object_get_int64(version) != 1)
                throw Error(ErrorCode::Schema, "Unsupported proof batch manifest version");
            ProofBatchLimits limits;
            if (const auto* workers = json_object_object_get(manifest.get(), "workers")) {
                if (!json_object_is_type(workers, json_type_int) ||
                    json_object_get_int64(workers) < 1 || json_object_get_int64(workers) > 4)
                    throw Error(ErrorCode::Budget, "Proof batch requires one to four workers");
                limits.workers = static_cast<std::size_t>(json_object_get_int64(workers));
            }
            const auto* paths = json_object_object_get(manifest.get(), "requests");
            if (!paths || !json_object_is_type(paths, json_type_array) ||
                json_object_array_length(paths) == 0 || json_object_array_length(paths) > limits.requests)
                throw Error(ErrorCode::Budget, "Invalid proof batch request count");
            std::vector<Bytes> requests;
            std::size_t input_bytes = 0;
            for (std::size_t i = 0; i < json_object_array_length(paths); ++i) {
                auto* path = json_object_array_get_idx(paths, i);
                if (!json_object_is_type(path, json_type_string))
                    throw Error(ErrorCode::Schema, "Expected a native proof request path");
                const std::string location(json_object_get_string(path),
                    static_cast<std::size_t>(json_object_get_string_len(path)));
                validate_text(location, 4096);
                if (location.empty()) throw Error(ErrorCode::Schema, "Empty native proof request path");
                std::ifstream input(location, std::ios::binary);
                if (!input) throw Error(ErrorCode::Storage, "Cannot open native proof request");
                auto request = read_binary(input);
                if (request.size() > limits.input_bytes - input_bytes)
                    throw Error(ErrorCode::Budget, "Proof batch input byte budget exceeded");
                input_bytes += request.size();
                requests.push_back(std::move(request));
            }
            const auto batch = run_proof_batch(requests, limits);
            text(response.get(), "profile", "gammathink.native-proof-batch.v1");
            json_object_object_add(response.get(), "executions", json_object_new_uint64(batch.executions));
            json_object_object_add(response.get(), "memo_hits", json_object_new_uint64(batch.memo_hits));
            json_object_object_add(response.get(), "workers_started", json_object_new_uint64(batch.workers_started));
            json_object_object_add(response.get(), "peak_active_workers", json_object_new_uint64(batch.peak_active_workers));
            json_object_object_add(response.get(), "input_bytes", json_object_new_uint64(batch.input_bytes));
            json_object_object_add(response.get(), "output_bytes", json_object_new_uint64(batch.output_bytes));
            auto* replies = json_object_new_array();
            json_object_object_add(response.get(), "replies", replies);
            for (const auto& reply : batch.replies) {
                auto* row = json_object_new_object(); json_object_array_add(replies, row);
                std::string wire;
                wire.reserve(reply.reply.size() * 2);
                constexpr std::string_view digits = "0123456789abcdef";
                for (const auto byte : reply.reply) {
                    wire += digits[byte >> 4U]; wire += digits[byte & 15U];
                }
                text(row, "native_reply_hex", wire);
                json_object_object_add(row, "producer", json_object_new_uint64(reply.producer));
                json_object_object_add(row, "valid", json_object_new_boolean(reply.check.valid));
                json_object_object_add(row, "conclusive", json_object_new_boolean(reply.check.conclusive));
                json_object_object_add(row, "conflict", json_object_new_boolean(reply.check.conflict));
            }
            std::cout << json_object_to_json_string_ext(response.get(), JSON_C_TO_STRING_PLAIN) << '\n';
            return 0;
        }
        if (mode == "source") {
            ZimSourceArchive archive(argv[2]);
            const auto source = archive.read(argv[3]);
            const auto& reference = source.sources.front();
            auto context = parse_problem_context(read_input(argv[4]));
            // This command imports only the host unit registry. The complete
            // source view is constructed from the actual ZIM item, not a preview.
            context.views.clear();
            context.views.push_back(make_evidence_view("source", PremiseBasis::Source,
                std::string(source.payload.begin(), source.payload.end()),
                reference.archive_id, reference.entry_path, reference.extractor_version));
            const ProblemSourceCatalog catalog(context);
            text(response.get(), "status", "source_prepared");
            text(response.get(), "source_id", hex(identify(source)));
            text(response.get(), "context_snapshot", catalog.snapshot());
            auto serialized = json_input(serialize_problem_context(context));
            json_object_object_add(response.get(), "context", serialized.release());
            const std::string output = json_object_to_json_string_ext(response.get(), JSON_C_TO_STRING_PLAIN);
            if (output.size() > 262144) throw Error(ErrorCode::Budget, "Complete source record exceeds CLI byte budget");
            std::cout << output << '\n';
            return 0;
        }
        if (mode == "ingest") {
            const auto capture = json_input(read_input(argv[4]));
            const auto context = json_object_object_get(capture.get(), "context");
            if (!context || json_object_get_type(context) != json_type_object) {
                throw Error(ErrorCode::Schema, "Source capture is missing its host context");
            }
            const auto host = parse_problem_context(json_object_to_json_string_ext(context, JSON_C_TO_STRING_PLAIN));
            const SourceTranslation translation{parse_id(field(capture.get(), "source_id")),
                field(capture.get(), "context_snapshot"), read_input(argv[5])};
            ZimSourceArchive archive(argv[2]);
            Journal journal(argv[6]);
            const auto result = ingest_zim_entry(archive, argv[3], journal, host, translation);
            text(response.get(), "status", result.kind == SourceCompilationKind::Candidate
                ? "source_candidate_stored" : result.kind == SourceCompilationKind::Clarify ? "clarify" : "unsupported");
            text(response.get(), "source_id", hex(result.source));
            text(response.get(), "reason", result.reason);
            if (result.module) text(response.get(), "module", hex(*result.module));
            if (result.context) text(response.get(), "context", hex(*result.context));
            auto* members = json_object_new_array();
            for (const auto& id : result.members) {
                const auto identity = hex(id);
                json_object_array_add(members, json_object_new_string(identity.c_str()));
            }
            json_object_object_add(response.get(), "members", members);
            json_object_object_add(response.get(), "new_objects", json_object_new_uint64(result.new_objects));
            json_object_object_add(response.get(), "encoded_bytes", json_object_new_uint64(result.encoded_bytes));
            json_object_object_add(response.get(), "published", json_object_new_boolean(false));
            std::cout << json_object_to_json_string_ext(response.get(), JSON_C_TO_STRING_PLAIN) << '\n';
            return 0;
        }
        if (mode == "snapshot") {
            Journal journal(argv[2]);
            const auto snapshot = compile_snapshot(journal, argv[3], argv[4], argv[5]);
            text(response.get(), "status", "compiled_private_snapshot");
            text(response.get(), "root", hex(snapshot.root));
            json_object_object_add(response.get(), "objects", json_object_new_uint64(snapshot.objects));
            json_object_object_add(response.get(), "stored_bytes", json_object_new_uint64(snapshot.output_bytes));
            json_object_object_add(response.get(), "published", json_object_new_boolean(false));
            std::cout << json_object_to_json_string_ext(response.get(), JSON_C_TO_STRING_PLAIN) << '\n';
            return 0;
        }
        if (mode == "resume" || mode == "resume-catalogue" || mode == "query-source" || mode == "query-bound") {
            if (mode == "query-bound" && (argc != 11 || std::string_view(argv[9]) != "--accept-binding" ||
                                        parse_id(argv[10]) != parse_id(argv[2])))
                throw Error(ErrorCode::Permission, "Explicit acceptance of the exact binding assumption is required");
            Journal journal(mode == "query-bound" ? argv[6] : mode == "query-source" ? argv[5] : argv[3]);
            const auto result = [&] {
                if (mode == "query-bound") {
                    const auto contexts = json_input(read_input(argv[3]), json_type_array);
                    if (json_object_array_length(contexts.get()) == 0 || json_object_array_length(contexts.get()) > 16)
                        throw Error(ErrorCode::Budget, "Bound source host context count exceeds its limit");
                    std::map<ObjectId, ProblemContext> hosts;
                    for (std::size_t i = 0; i < json_object_array_length(contexts.get()); ++i) {
                        auto* row = json_object_array_get_idx(contexts.get(), i);
                        if (!row || !json_object_is_type(row, json_type_array) || json_object_array_length(row) != 2)
                            throw Error(ErrorCode::Schema, "Invalid bound source host context tuple");
                        auto* module = json_object_array_get_idx(row, 0);
                        if (!module || !json_object_is_type(module, json_type_string))
                            throw Error(ErrorCode::Schema, "Invalid bound source host module identity");
                        const auto id = parse_id(std::string_view(json_object_get_string(module), json_object_get_string_len(module)));
                        const auto host = parse_problem_context(json_object_to_json_string_ext(
                            json_object_array_get_idx(row, 1), JSON_C_TO_STRING_PLAIN));
                        if (!hosts.emplace(id, host).second) throw Error(ErrorCode::Schema, "Duplicate bound source host module");
                    }
                    const auto query = json_input(read_input(argv[5]), json_type_array);
                    const auto binding = parse_id(argv[2]);
                    JournalArchive local(journal);
                    GksArchive archive(argv[7], parse_block_id(argv[8]));
                    UnifiedKnowledge knowledge({&local, &archive});
                    auto run = run_bound_sources(knowledge, journal, binding, hosts, {binding},
                        {parse_id(argv[4]), source_goal(query.get())});
                    text(response.get(), "source_profile", "explicit-bound-sources-v1");
                    text(response.get(), "binding_assumption", hex(binding));
                    json_object_object_add(response.get(), "conditional", json_object_new_boolean(true));
                    json_object_object_add(response.get(), "source_scopes", json_object_new_uint64(run.scopes));
                    json_object_object_add(response.get(), "source_member_queries", json_object_new_uint64(run.members));
                    if (run.request) text(response.get(), "request", hex(*run.request));
                    return std::move(run.outcome);
                }
                if (mode == "query-source") {
                    const auto query = json_input(read_input(argv[4]), json_type_array);
                    const auto goal = source_goal(query.get());
                    const auto host = parse_problem_context(read_input(argv[3]));
                    JournalArchive local(journal);
                    GksArchive archive(argv[6], parse_block_id(argv[7]));
                    UnifiedKnowledge knowledge({&local, &archive});
                    if (const auto* quantity = std::get_if<ProblemQuantityGoal>(&goal)) {
                        auto run = run_source_quantity(knowledge, journal, parse_id(argv[2]), host, *quantity);
                        if (run.request) text(response.get(), "request", hex(*run.request));
                        text(response.get(), "source_profile", "bounded-module-quantity-v1");
                        json_object_object_add(response.get(), "source_member_queries", json_object_new_uint64(run.member_queries));
                        return std::move(run.outcome);
                    }
                    auto run = run_source_logic(knowledge, journal, parse_id(argv[2]), host, std::get<ProblemAtomGoal>(goal).atom);
                    if (run.request) text(response.get(), "request", hex(*run.request));
                    json_object_object_add(response.get(), "source_rule_queries", json_object_new_uint64(run.rule_queries));
                    json_object_object_add(response.get(), "source_work", json_object_new_uint64(run.work));
                    return std::move(run.outcome);
                }
                if (mode == "resume") {
                    JournalArchive local(journal);
                    GksArchive archive(argv[4], parse_block_id(argv[5]));
                    UnifiedKnowledge knowledge({&local, &archive});
                    return run_knowledge_goal(knowledge, journal, parse_id(argv[2]));
                }
                if (!std::filesystem::is_regular_file(argv[4]))
                    throw Error(ErrorCode::Missing, "Accepted publisher catalogue is absent");
                PublisherCatalogue catalogue(argv[4]);
                // Only prior explicit local acceptance can provide these mounts.
                // This offline CLI cannot mint remote network capabilities.
                DriveView view(journal, catalogue.mounts());
                return run_knowledge_goal(view.knowledge(), journal, parse_id(argv[2]));
            }();
            text(response.get(), "status", status(result.status));
            text(response.get(), "reason", result.reason);
            json_object_object_add(response.get(), "object_lookups", json_object_new_uint64(result.object_lookups));
            json_object_object_add(response.get(), "imported_objects", json_object_new_uint64(result.imported_objects));
            json_object_object_add(response.get(), "reused", json_object_new_boolean(result.execution.reused));
            json_object_object_add(response.get(), "native_executions", json_object_new_uint64(
                !result.execution.reply.empty() && !result.execution.reused ? 1 : 0));
            if (result.execution.certificate) text(response.get(), "certificate", hex(*result.execution.certificate));
            if (!result.execution.reply.empty()) {
                text(response.get(), "result", render_binary_result(result.execution.reply));
            }
            auto* missing = json_object_new_array();
            for (const auto& id : result.unavailable) {
                const auto identity = hex(id);
                json_object_array_add(missing, json_object_new_string(identity.c_str()));
            }
            json_object_object_add(response.get(), "unavailable", missing);
            auto* revalidation = json_object_new_array();
            for (const auto& id : result.revalidation_required)
                json_object_array_add(revalidation, json_object_new_string(hex(id).c_str()));
            json_object_object_add(response.get(), "revalidation_required", revalidation);
            std::cout << json_object_to_json_string_ext(response.get(), JSON_C_TO_STRING_PLAIN) << '\n';
            return 0;
        }
        const ProblemSourceCatalog catalog(parse_problem_context(read_input(argv[2])));
        const auto interpretation = admit_task_dsl(catalog, catalog.snapshot(), read_input(argv[3]));
        if (const auto* decline = std::get_if<TaskDecline>(&interpretation)) {
            text(response.get(), "status", decline->kind == TaskDeclineKind::Clarify ? "clarify" : "unsupported");
            text(response.get(), "reason", decline->reason);
            json_object_object_add(response.get(), "native_executions", json_object_new_int(0));
        } else {
            Journal journal(argv[4]);
            const auto result = execute_task(journal, std::get<AdmittedTask>(interpretation));
            text(response.get(), "status", result.evaluation.root_satisfied ? "checked" : "unresolved_or_conflicting");
            text(response.get(), "task", hex(result.task.ordered_request));
            text(response.get(), "interpretation", hex(result.task.interpretation));
            auto* slots = json_object_new_array();
            std::size_t executions = 0;
            for (const auto& slot : result.slots) {
                auto* value = json_object_new_object();
                json_object_object_add(value, "ordinal", json_object_new_uint64(slot.ordinal));
                text(value, "request", hex(result.task.requests[slot.ordinal]));
                json_object_object_add(value, "formally_checked", json_object_new_boolean(slot.execution.check.valid));
                json_object_object_add(value, "conclusive", json_object_new_boolean(slot.execution.check.conclusive));
                json_object_object_add(value, "conflict", json_object_new_boolean(slot.execution.check.conflict));
                json_object_object_add(value, "reused", json_object_new_boolean(slot.execution.reused));
                if (slot.execution.certificate) text(value, "certificate", hex(*slot.execution.certificate));
                if (!slot.execution.reply.empty()) {
                    text(value, "result", render_problem(decode_problem_result(slot.execution.reply)));
                    if (!slot.execution.reused) ++executions;
                }
                auto* missing = json_object_new_array();
                for (const auto& id : slot.execution.unavailable) {
                    const auto identity = hex(id);
                    json_object_array_add(missing, json_object_new_string(identity.c_str()));
                }
                json_object_object_add(value, "unavailable", missing);
                json_object_array_add(slots, value);
            }
            json_object_object_add(response.get(), "slots", slots);
            json_object_object_add(response.get(), "native_executions", json_object_new_uint64(executions));
        }
        std::cout << json_object_to_json_string_ext(response.get(), JSON_C_TO_STRING_PLAIN) << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
