#include "block_cache.hpp"
#include "retention.hpp"
#include "virtual_drive.hpp"

#include <json-c/json.h>

#include <iostream>

namespace {
using namespace axiom::knowledge;
struct JsonDelete { void operator()(json_object* value) const { json_object_put(value); } };
using Json = std::unique_ptr<json_object, JsonDelete>;
json_object* ids(std::span<const ObjectId> values) {
    auto* result = json_object_new_array();
    for (const auto& id : values) json_object_array_add(result, json_object_new_string(hex(id).c_str()));
    return result;
}
json_object* blocks(std::span<const BlockId> values) {
    auto* result = json_object_new_array();
    for (const auto& id : values) json_object_array_add(result, json_object_new_string(hex(id).c_str()));
    return result;
}
void add(json_object* result, const char* key, std::uint64_t value) {
    json_object_object_add(result, key, json_object_new_uint64(value));
}
}

int main(int argc, char** argv) {
    using namespace axiom::knowledge;
    try {
        Json output(json_object_new_object());
        if (argc >= 4 && std::string_view(argv[1]) == "plan") {
            const auto root = parse_id(argv[3]);
            std::unique_ptr<GksArchive> archive;
            if (argc >= 7 && std::string_view(argv[argc - 3]) == "--snapshot") {
                archive = std::make_unique<GksArchive>(argv[argc - 2], BlockId{parse_id(argv[argc - 1]).digest});
                argc -= 3;
            }
            if (!std::filesystem::is_regular_file(argv[2]))
                throw Error(ErrorCode::Missing, "Maintenance requires an existing journal");
            if (argc - 4 > static_cast<int>(maximum_items))
                throw Error(ErrorCode::Budget, "Too many optional working-set candidates");
            std::vector<ObjectId> candidates;
            for (int i = 4; i < argc; ++i) candidates.push_back(parse_id(argv[i]));
            Journal journal(argv[2]);
            JournalArchive local(journal);
            std::vector<KnowledgeArchive*> sources{&local};
            if (archive) sources.push_back(archive.get());
            UnifiedKnowledge knowledge(std::move(sources));
            const auto plan = plan_retention(knowledge, journal, std::array{root}, candidates);
            const auto* status = plan.status == RetentionStatus::Complete ? "complete" :
                plan.status == RetentionStatus::ResourceLimited ? "resource_limited" : "dependencies_unavailable";
            json_object_object_add(output.get(), "status", json_object_new_string(status));
            json_object_object_add(output.get(), "roots", ids(plan.roots));
            json_object_object_add(output.get(), "unavailable", ids(plan.unavailable));
            json_object_object_add(output.get(), "optional_omissions", ids(plan.optional_omissions));
            auto* records = json_object_new_array();
            for (const auto& entry : plan.retained) {
                auto* record = json_object_new_object();
                json_object_object_add(record, "id", json_object_new_string(hex(entry.id).c_str()));
                add(record, "archive_position", entry.archive);
                add(record, "observed_assessment_version", entry.record.assessment ? entry.record.assessment->version : 0);
                json_object_array_add(records, record);
            }
            json_object_object_add(output.get(), "retained", records);
            add(output.get(), "lookups", plan.lookups);
            add(output.get(), "edges", plan.edges);
            add(output.get(), "record_bytes", plan.record_bytes);
            json_object_object_add(output.get(), "deletion_authorized", json_object_new_boolean(false));
        } else if ((argc == 4 || argc == 5) && std::string_view(argv[1]) == "prune") {
            CachePruneLimits limits;
            if (argc == 5) {
                if (std::string_view(argv[4]) != "--apply")
                    throw Error(ErrorCode::Schema, "Expected --apply or no pruning option");
                limits.apply = true;
            }
            if (!std::filesystem::is_directory(argv[2]))
                throw Error(ErrorCode::Missing, "Maintenance requires an existing dedicated cache directory");
            const auto cursor = std::string_view(argv[3]);
            std::optional<BlockId> after;
            if (cursor != "-") after = BlockId{parse_id(cursor).digest};
            BlockCache cache(argv[2]); // No upstream capability: local temporary cache only.
            const auto page = cache.prune(after, limits);
            json_object_object_add(output.get(), "dry_run", json_object_new_boolean(!limits.apply));
            json_object_object_add(output.get(), "evictable", blocks(page.evictable));
            json_object_object_add(output.get(), "complete", json_object_new_boolean(page.complete));
            json_object_object_add(output.get(), "next", page.next ? json_object_new_string(hex(*page.next).c_str()) : nullptr);
            add(output.get(), "examined", page.examined);
            add(output.get(), "evicted", page.evicted);
            add(output.get(), "ssd_bytes", page.ssd_bytes);
            add(output.get(), "ram_bytes", page.ram_bytes);
        } else if (argc == 4 && std::string_view(argv[1]) == "compact") {
            if (!std::filesystem::is_regular_file(argv[2]))
                throw Error(ErrorCode::Missing, "Compaction requires an existing journal");
            Journal journal(argv[2]);
            const auto copied = journal.compact_copy(argv[3]);
            json_object_object_add(output.get(), "database", json_object_new_string(copied.database.c_str()));
            add(output.get(), "source_bytes", copied.source_bytes);
            add(output.get(), "copy_bytes", copied.copy_bytes);
            add(output.get(), "objects", copied.objects);
            add(output.get(), "assessments", copied.assessments);
            json_object_object_add(output.get(), "original_untouched", json_object_new_boolean(true));
            json_object_object_add(output.get(), "published", json_object_new_boolean(false));
        } else {
            throw Error(ErrorCode::Schema,
                "Usage: knowledge-maintain plan JOURNAL ROOT [OPTIONAL_ID ...] [--snapshot DIRECTORY ROOT_HASH] | "
                "prune CACHE_DIRECTORY AFTER_OR_DASH [--apply] | compact JOURNAL NEW_PRIVATE_DIRECTORY");
        }
        std::cout << json_object_to_json_string_ext(output.get(), JSON_C_TO_STRING_PLAIN) << '\n';
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
