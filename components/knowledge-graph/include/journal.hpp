#pragma once

#include "knowledge.hpp"

#include <filesystem>
#include <functional>
#include <memory>

struct sqlite3;

namespace axiom::knowledge {

struct JournalLimits {
    std::uint32_t cache_kib = 4096;
    std::uint64_t sql_instructions = 10000000;
};

struct ObjectPage {
    std::vector<ObjectId> objects;
    bool complete = true;
    std::optional<ObjectId> next;
};
struct AssessmentPage {
    std::vector<Assessment> records;
    bool complete = true;
    std::optional<std::uint64_t> next;
};
struct ObservedAssessment {
    ObjectId object;
    // Zero means that no assessment existed when the checker read the input.
    std::uint64_t version = 0;
};
struct JournalCopy {
    std::filesystem::path database;
    std::uint64_t source_bytes = 0, copy_bytes = 0, objects = 0, assessments = 0;
};

// One owner thread per connection. Parallel readers use separate connections.
// Writes are atomic; SQLite contention is reported, not retried indefinitely.
class Journal {
public:
    explicit Journal(const std::filesystem::path& path, JournalLimits limits = {});
    ~Journal();
    Journal(const Journal&) = delete;
    Journal& operator=(const Journal&) = delete;

    ObjectId put(const Object& object);
    bool contains(const ObjectId& id);
    std::optional<Object> get(const ObjectId& id);
    // Pins one read-only SQLite view before entering the callback. Calls on
    // this connection share that view; independent WAL writers may continue.
    // No nested snapshot or write transaction is allowed on this connection.
    void read_snapshot(const std::function<void()>& read);
    // Non-destructive cold/compact copy of one coherent local view. The new
    // private directory must not exist. All historical assessments and indexes
    // are retained; no original object, dependency or database file is deleted.
    // Copies a bounded number of SQLite pages without busy retries, then VACUUMs
    // only the copy under this connection's SQL instruction/cache limits.
    JournalCopy compact_copy(const std::filesystem::path& new_directory,
                             std::uint64_t maximum_source_bytes = 64 * 1024 * 1024);
    ObjectPage objects(std::optional<ObjectId> after = {}, std::size_t limit = 64);
    ObjectPage by_kind(ObjectKind kind, std::optional<ObjectId> after = {}, std::size_t limit = 64);
    ObjectPage by_concept(std::string_view value, std::optional<ObjectId> after = {},
                          std::size_t limit = 64);
    ObjectPage by_source_archive(std::string_view archive, std::optional<std::string_view> version = {},
                                 std::optional<ObjectId> after = {}, std::size_t limit = 64);
    ObjectPage search(std::string_view phrase, std::optional<ObjectId> after = {},
                      std::size_t limit = 64);
    ObjectPage related(const ObjectId& target, Relationship relationship,
                       std::optional<ObjectId> after = {}, std::size_t limit = 64);
    ObjectPage required_by(const ObjectId& dependency, std::optional<ObjectId> after = {},
                           std::size_t limit = 64);

    // Trusted local checker API; untrusted imports do not supply authoritative
    // assessment rows. Version zero requests a new, monotonically assigned row.
    // Targets may be remote; an assessment does not assert local availability.
    Assessment record_assessment(const ObjectId& id, Assessment assessment);
    // Check observed versions and append under the same write transaction.
    // Every checked dependency must be covered; stale observations never
    // revive an invalidated proof. This does not assert data availability.
    Assessment record_revalidated_assessment(const ObjectId& id, Assessment assessment,
                                              std::span<const ObservedAssessment> observed);
    std::optional<Assessment> assessment(const ObjectId& id);
    AssessmentPage assessment_history(const ObjectId& id, std::uint64_t after = 0,
                                      std::size_t limit = 64);

    // Invalidate reuse, not formal truth. Previous assessments and objects stay.
    // External dependency IDs are allowed even when the root is not in the journal.
    // The complete local closure is changed atomically or the operation fails.
    std::vector<ObjectId> invalidate(const ObjectId& dependency, std::string_view reason,
                                      std::size_t maximum_affected = maximum_items);

private:
    struct Close {
        void operator()(sqlite3* database) const noexcept;
    };
    std::unique_ptr<sqlite3, Close> database_;
    JournalLimits limits_;
};

} // namespace axiom::knowledge
