#include "publisher_catalogue.hpp"
#include "gks_snapshot.hpp"

#include <sqlite3.h>

#include <algorithm>

namespace axiom::knowledge {
namespace {
constexpr std::uint64_t application_id = 0x47504631;
void require(bool value, ErrorCode code, const char* message) {
    if (!value) throw Error(code, message);
}
void label(std::string_view value) {
    validate_text(value, 128);
    require(!value.empty(), ErrorCode::Schema, "Missing catalogue label");
}
void sql_check(int code, sqlite3* database) {
    if (code == SQLITE_OK || code == SQLITE_ROW || code == SQLITE_DONE) return;
    const auto basic = code & 255;
    throw Error(basic == SQLITE_INTERRUPT || basic == SQLITE_FULL ? ErrorCode::Budget :
        basic == SQLITE_CORRUPT || basic == SQLITE_NOTADB ? ErrorCode::Integrity : ErrorCode::Storage,
        sqlite3_errmsg(database));
}
void execute(sqlite3* database, const std::string& sql) {
    sql_check(sqlite3_exec(database, sql.c_str(), nullptr, nullptr, nullptr), database);
}
struct Work {
    sqlite3* database;
    std::uint64_t left;
    Work(sqlite3* db, std::uint64_t count) : database(db), left(count) {
        sqlite3_progress_handler(db, 1, [](void* p) {
            auto& remaining = static_cast<Work*>(p)->left;
            if (remaining <= 1) return 1;
            --remaining; return 0;
        }, this);
    }
    void finish() { sqlite3_progress_handler(database, 0, nullptr, nullptr); }
    ~Work() { finish(); }
};
struct Transaction {
    sqlite3* database;
    explicit Transaction(sqlite3* db) : database(db) { execute(db, "BEGIN IMMEDIATE"); }
    ~Transaction() {
        if (!sqlite3_get_autocommit(database)) sqlite3_exec(database, "ROLLBACK", nullptr, nullptr, nullptr);
    }
    void commit(Work& work) { work.finish(); execute(database, "COMMIT"); }
};
struct Statement {
    sqlite3* database;
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> value{nullptr, sqlite3_finalize};
    Statement(sqlite3* db, const std::string& sql) : database(db) {
        sqlite3_stmt* raw = nullptr;
        const auto code = sqlite3_prepare_v2(db, sql.c_str(), -1, &raw, nullptr);
        value.reset(raw); sql_check(code, db);
    }
    void text(int slot, std::string_view input) {
        sql_check(sqlite3_bind_text64(value.get(), slot, input.data(), input.size(), SQLITE_TRANSIENT, SQLITE_UTF8), database);
    }
    void bytes(int slot, std::span<const std::uint8_t> input) {
        sql_check(sqlite3_bind_blob64(value.get(), slot, input.data(), input.size(), SQLITE_TRANSIENT), database);
    }
    void number(int slot, std::uint64_t input) {
        require(input <= INT64_MAX, ErrorCode::Budget, "Catalogue integer overflow");
        sql_check(sqlite3_bind_int64(value.get(), slot, static_cast<sqlite3_int64>(input)), database);
    }
    bool row() { const auto code = sqlite3_step(value.get()); sql_check(code, database); return code == SQLITE_ROW; }
    void done() { require(!row(), ErrorCode::Storage, "Unexpected catalogue result row"); }
    bool absent(int column) { return sqlite3_column_type(value.get(), column) == SQLITE_NULL; }
    std::uint64_t number(int column) {
        require(sqlite3_column_type(value.get(), column) == SQLITE_INTEGER && sqlite3_column_int64(value.get(), column) >= 0,
            ErrorCode::Integrity, "Invalid catalogue integer");
        return static_cast<std::uint64_t>(sqlite3_column_int64(value.get(), column));
    }
    Bytes bytes(int column, std::size_t maximum) {
        require(sqlite3_column_type(value.get(), column) == SQLITE_BLOB, ErrorCode::Integrity, "Invalid catalogue bytes");
        const auto size = sqlite3_column_bytes(value.get(), column);
        require(size > 0 && static_cast<std::size_t>(size) <= maximum, ErrorCode::Budget, "Catalogue bytes exceed bounds");
        const auto* data = static_cast<const std::uint8_t*>(sqlite3_column_blob(value.get(), column));
        return Bytes(data, data + size);
    }
    Digest hash(int column) {
        const auto input = bytes(column, 32);
        require(input.size() == 32, ErrorCode::Integrity, "Invalid catalogue identity extent");
        Digest result; std::copy(input.begin(), input.end(), result.begin()); return result;
    }
    std::string text(int column) {
        require(sqlite3_column_type(value.get(), column) == SQLITE_TEXT, ErrorCode::Integrity, "Invalid catalogue text");
        const auto size = sqlite3_column_bytes(value.get(), column);
        require(size >= 0 && size <= static_cast<int>(maximum_text_bytes), ErrorCode::Budget, "Catalogue text exceeds bounds");
        const auto* data = reinterpret_cast<const char*>(sqlite3_column_text(value.get(), column));
        std::string result(data, static_cast<std::size_t>(size)); validate_text(result); return result;
    }
};
std::uint64_t count(sqlite3* database, const std::string& sql) {
    Statement query(database, sql);
    require(query.row(), ErrorCode::Storage, "Missing catalogue count"); return query.number(0);
}
void generation(const MountState& current, std::uint64_t expected) {
    require(current.generation == expected, ErrorCode::Unavailable, "Stale catalogue acceptance generation");
    require(expected < INT64_MAX, ErrorCode::Budget, "Catalogue generation exhausted");
}
}
struct PublisherCatalogue::Impl {
    struct Close { void operator()(sqlite3* p) const { sqlite3_close_v2(p); } };
    std::unique_ptr<sqlite3, Close> database;
    PublisherCatalogueLimits limits;
    Impl(const std::filesystem::path& path, PublisherCatalogueLimits policy) : limits(policy) {
        validate_text(path.string());
        require(!path.empty(), ErrorCode::Schema, "Missing publisher catalogue path");
        require(limits.feeds > 0 && limits.feeds <= 4096 && limits.aliases > 0 && limits.aliases <= 4096
            && limits.history > 0 && limits.history <= 65536 && limits.chain > 0 && limits.chain <= 4096
            && limits.active_mounts <= 15 && limits.wire_bytes > 0 && limits.wire_bytes <= 256 * 1024 * 1024
            && limits.cache_kib >= 64 && limits.cache_kib <= 65536 && limits.sql_instructions > 0,
            ErrorCode::Budget, "Invalid publisher catalogue limits");
        sqlite3* raw = nullptr;
        const auto code = sqlite3_open_v2(path.c_str(), &raw, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
        database.reset(raw); sql_check(code, raw);
        sql_check(sqlite3_extended_result_codes(raw, 1), raw);
        execute(raw, "PRAGMA cache_size=-" + std::to_string(limits.cache_kib));
        execute(raw, "PRAGMA temp_store=FILE");
        const auto identity = count(raw, "PRAGMA application_id"), version = count(raw, "PRAGMA user_version");
        if (!identity && !version)
            require(count(raw, "SELECT count(*) FROM sqlite_schema WHERE type IN ('table','view') AND name NOT LIKE 'sqlite_%'") == 0,
                ErrorCode::Schema, "Refusing an unrelated publisher catalogue database");
        else require(identity == application_id && version == 1, ErrorCode::Schema, "Unsupported publisher catalogue schema");
        execute(raw, "PRAGMA foreign_keys=ON");
        execute(raw, "PRAGMA trusted_schema=OFF");
        execute(raw, "PRAGMA journal_mode=WAL");
        execute(raw, "PRAGMA synchronous=FULL");
        if (!identity) {
            Transaction transaction(raw); Work work(raw, limits.sql_instructions);
            execute(raw, R"SQL(
CREATE TABLE feeds(id BLOB PRIMARY KEY CHECK(length(id)=32),
 publisher BLOB NOT NULL CHECK(length(publisher)=32), wire BLOB NOT NULL) STRICT, WITHOUT ROWID;
CREATE TABLE acceptance(alias TEXT NOT NULL, generation INTEGER NOT NULL CHECK(generation>0),
 feed BLOB REFERENCES feeds(id), offer INTEGER, backend INTEGER NOT NULL CHECK(backend BETWEEN 1 AND 3),
 location TEXT, PRIMARY KEY(alias,generation),
 CHECK((feed IS NULL AND offer IS NULL AND location IS NULL AND backend=1)
    OR (feed IS NOT NULL AND offer>=0 AND location IS NOT NULL))) STRICT, WITHOUT ROWID;
PRAGMA application_id=0x47504631;
PRAGMA user_version=1;
)SQL");
            transaction.commit(work);
        }
    }
    std::optional<VerifiedPublisherFeed> load(const PublisherFeedId& id, std::optional<PublisherKey> pinned = {}) {
        Statement query(database.get(), "SELECT publisher,wire FROM feeds WHERE id=?1");
        query.bytes(1, id.digest);
        if (!query.row()) return {};
        const auto publisher = query.hash(0);
        if (pinned) require(publisher == *pinned, ErrorCode::Permission, "Stored feed differs from host-pinned publisher");
        const auto wire = query.bytes(1, 1024 * 1024);
        require(digest(wire) == id.digest, ErrorCode::Integrity, "Stored publisher feed identity differs");
        return verify_publisher_feed(wire, publisher);
    }
    bool chain(const PublisherFeedId& id, const PublisherKey& pinned) {
        auto current = load(id, pinned);
        require(current.has_value(), ErrorCode::Missing, "Publisher feed is not discovered");
        for (std::size_t depth = 0; ; ++depth) {
            require(depth < limits.chain, ErrorCode::Budget, "Publisher chain exceeds traversal limit");
            const auto& content = current->content();
            if (!content.previous) return true;
            auto prior = load(*content.previous, pinned);
            if (!prior) return false;
            require(prior->content().channel == content.channel && prior->content().sequence + 1 == content.sequence,
                ErrorCode::Integrity, "Publisher predecessor channel or sequence differs");
            current = std::move(prior);
        }
    }
    MountState decode_state(std::string_view alias, Statement& query) {
        MountState result{std::string(alias), query.number(0), {}};
        if (query.absent(1)) {
            require(query.absent(2) && query.number(3) == 1 && query.absent(4), ErrorCode::Integrity, "Invalid catalogue tombstone");
            return result;
        }
        const auto feed = load({query.hash(1)});
        require(feed.has_value(), ErrorCode::Integrity, "Accepted publisher feed is absent");
        const auto ordinal = query.number(2), backend = query.number(3);
        require(ordinal < feed->content().offers.size() && backend >= 1 && backend <= 3,
            ErrorCode::Integrity, "Invalid catalogue acceptance");
        const auto& offer = feed->content().offers[ordinal];
        MountBinding binding{{offer.name, offer.revision, offer.snapshot}, {}};
        binding.backend = static_cast<MountBackend>(backend);
        const auto location = query.text(4);
        if (binding.backend == MountBackend::Local) {
            binding.directory = location;
            require(binding.directory.is_absolute(), ErrorCode::Integrity, "Relative catalogue snapshot path");
        } else { binding.source = location; label(binding.source); }
        result.binding = std::move(binding); return result;
    }
    MountState state(std::string_view alias) {
        Statement query(database.get(),
            "SELECT generation,feed,offer,backend,location FROM acceptance WHERE alias=?1 ORDER BY generation DESC LIMIT 1");
        query.text(1, alias);
        return query.row() ? decode_state(alias, query) : MountState{std::string(alias), 0, {}};
    }
    std::vector<MountState> active() {
        Statement query(database.get(), "SELECT a.alias FROM acceptance a WHERE a.feed IS NOT NULL AND NOT EXISTS("
            "SELECT 1 FROM acceptance newer WHERE newer.alias=a.alias AND newer.generation>a.generation) ORDER BY a.alias");
        std::vector<MountState> result;
        while (query.row()) {
            require(result.size() < limits.active_mounts, ErrorCode::Budget, "Catalogue active mount quota exceeded");
            result.push_back(state(query.text(0)));
        }
        return result;
    }
    MountState bind(std::string_view alias, const PublisherFeedId& id, std::size_t offer,
                    MountBackend backend, std::string_view location, std::uint64_t expected) {
        Transaction transaction(database.get()); Work work(database.get(), limits.sql_instructions);
        const auto current = state(alias); generation(current, expected);
        require(count(database.get(), "SELECT count(*) FROM acceptance") < limits.history,
            ErrorCode::Budget, "Catalogue acceptance history quota exceeded");
        if (current.generation == 0)
            require(count(database.get(), "SELECT count(DISTINCT alias) FROM acceptance") < limits.aliases,
                ErrorCode::Budget, "Catalogue alias quota exceeded");
        require(current.binding || active().size() < limits.active_mounts, ErrorCode::Budget, "Catalogue active mount quota exceeded");
        Statement insert(database.get(), "INSERT INTO acceptance VALUES(?1,?2,?3,?4,?5,?6)");
        insert.text(1, alias); insert.number(2, expected + 1); insert.bytes(3, id.digest);
        insert.number(4, offer); insert.number(5, static_cast<std::uint8_t>(backend)); insert.text(6, location); insert.done();
        const auto result = state(alias); transaction.commit(work); return result;
    }
    SnapshotOffer selection(const PublisherFeedId& id, std::size_t ordinal, const PublisherKey& pinned) {
        require(chain(id, pinned), ErrorCode::Unavailable, "Publisher predecessor feed is unavailable");
        const auto feed = load(id, pinned);
        require(ordinal < feed->content().offers.size(), ErrorCode::Schema, "Publisher offer ordinal is absent");
        return feed->content().offers[ordinal];
    }
};
PublisherCatalogue::PublisherCatalogue(const std::filesystem::path& path, PublisherCatalogueLimits limits)
    : impl_(std::make_unique<Impl>(path, limits)) {}
PublisherCatalogue::~PublisherCatalogue() = default;
void PublisherCatalogue::discover(const VerifiedPublisherFeed& feed) {
    Transaction transaction(impl_->database.get()); Work work(impl_->database.get(), impl_->limits.sql_instructions);
    if (const auto existing = impl_->load(feed.id())) {
        require(existing->publisher() == feed.publisher() && std::equal(existing->wire().begin(), existing->wire().end(),
            feed.wire().begin(), feed.wire().end()), ErrorCode::Integrity, "Conflicting stored publisher feed");
        transaction.commit(work); return;
    }
    require(count(impl_->database.get(), "SELECT count(*) FROM feeds") < impl_->limits.feeds,
        ErrorCode::Budget, "Publisher feed quota exceeded");
    const auto used = count(impl_->database.get(), "SELECT coalesce(sum(length(wire)),0) FROM feeds");
    require(used <= impl_->limits.wire_bytes && feed.wire().size() <= impl_->limits.wire_bytes - used,
        ErrorCode::Budget, "Publisher catalogue byte quota exceeded");
    Statement insert(impl_->database.get(), "INSERT INTO feeds VALUES(?1,?2,?3)");
    insert.bytes(1, feed.id().digest); insert.bytes(2, feed.publisher()); insert.bytes(3, feed.wire()); insert.done();
    transaction.commit(work);
}
VerifiedPublisherFeed PublisherCatalogue::get(const PublisherFeedId& id, const PublisherKey& pinned) {
    Work work(impl_->database.get(), impl_->limits.sql_instructions);
    auto result = impl_->load(id, pinned);
    require(result.has_value(), ErrorCode::Missing, "Publisher feed is not discovered"); return std::move(*result);
}
PublisherFeedPage PublisherCatalogue::feeds(std::optional<PublisherFeedId> after, std::size_t limit) {
    require(limit > 0 && limit <= 64, ErrorCode::Budget, "Invalid publisher feed page limit");
    Work work(impl_->database.get(), impl_->limits.sql_instructions);
    Statement query(impl_->database.get(), after ? "SELECT id FROM feeds WHERE id>?1 ORDER BY id LIMIT ?2" :
        "SELECT id FROM feeds ORDER BY id LIMIT ?2");
    if (after) query.bytes(1, after->digest);
    query.number(2, limit + 1);
    PublisherFeedPage page;
    while (query.row()) {
        if (page.feeds.size() == limit) { page.complete = false; page.next = page.feeds.back(); break; }
        page.feeds.push_back({query.hash(0)});
    }
    return page;
}
bool PublisherCatalogue::chain_complete(const PublisherFeedId& id, const PublisherKey& pinned) {
    Work work(impl_->database.get(), impl_->limits.sql_instructions); return impl_->chain(id, pinned);
}
MountState PublisherCatalogue::accept_local(std::string_view alias, const PublisherFeedId& id, std::size_t ordinal,
    const PublisherKey& pinned, const std::filesystem::path& directory, std::uint64_t expected) {
    label(alias);
    generation(resolve(alias), expected);
    SnapshotOffer offer;
    { Work work(impl_->database.get(), impl_->limits.sql_instructions); offer = impl_->selection(id, ordinal, pinned); }
    GksSnapshot snapshot(directory, offer.snapshot);
    require(snapshot.manifest().name == offer.name && snapshot.manifest().revision == offer.revision,
        ErrorCode::Integrity, "Publisher offer differs from snapshot manifest");
    std::error_code error;
    const auto location = std::filesystem::canonical(directory, error);
    require(!error, ErrorCode::Storage, "Cannot resolve accepted snapshot directory"); validate_text(location.string());
    return impl_->bind(alias, id, ordinal, MountBackend::Local, location.string(), expected);
}
MountState PublisherCatalogue::accept_remote(std::string_view alias, const PublisherFeedId& id, std::size_t ordinal,
    const PublisherKey& pinned, MountBackend backend, std::string_view source_key,
    std::shared_ptr<BlockSource> source, std::uint64_t expected) {
    label(alias); label(source_key);
    require(backend == MountBackend::RemoteBlocks || backend == MountBackend::RemoteQuery,
        ErrorCode::Schema, "Remote acceptance requires a remote backend");
    generation(resolve(alias), expected);
    SnapshotOffer offer;
    { Work work(impl_->database.get(), impl_->limits.sql_instructions); offer = impl_->selection(id, ordinal, pinned); }
    GksSnapshot snapshot(std::move(source), offer.snapshot);
    require(snapshot.manifest().name == offer.name && snapshot.manifest().revision == offer.revision,
        ErrorCode::Integrity, "Publisher offer differs from snapshot manifest");
    return impl_->bind(alias, id, ordinal, backend, source_key, expected);
}
MountState PublisherCatalogue::resolve(std::string_view alias) {
    label(alias); Work work(impl_->database.get(), impl_->limits.sql_instructions); return impl_->state(alias);
}
std::vector<MountState> PublisherCatalogue::mounts() {
    Work work(impl_->database.get(), impl_->limits.sql_instructions); return impl_->active();
}
CatalogueAcceptancePage PublisherCatalogue::history(std::string_view alias, std::uint64_t after, std::size_t limit) {
    label(alias);
    require(limit > 0 && limit <= 64, ErrorCode::Budget, "Invalid catalogue history page limit");
    Work work(impl_->database.get(), impl_->limits.sql_instructions);
    Statement query(impl_->database.get(),
        "SELECT generation,feed,offer,backend,location FROM acceptance WHERE alias=?1 AND generation>?2 ORDER BY generation LIMIT ?3");
    query.text(1, alias); query.number(2, after); query.number(3, limit + 1);
    CatalogueAcceptancePage page;
    while (query.row()) {
        if (page.records.size() == limit) {
            page.complete = false; page.next = page.records.back().state.generation; break;
        }
        CatalogueAcceptance record{impl_->decode_state(alias, query), {}, {}};
        if (!query.absent(1)) {
            record.feed = PublisherFeedId{query.hash(1)};
            record.offer = query.number(2);
        }
        page.records.push_back(std::move(record));
    }
    return page;
}
MountState PublisherCatalogue::unmount(std::string_view alias, std::uint64_t expected) {
    label(alias);
    Transaction transaction(impl_->database.get()); Work work(impl_->database.get(), impl_->limits.sql_instructions);
    const auto current = impl_->state(alias); generation(current, expected);
    require(current.binding.has_value(), ErrorCode::Missing, "Catalogue alias is not mounted");
    require(count(impl_->database.get(), "SELECT count(*) FROM acceptance") < impl_->limits.history,
        ErrorCode::Budget, "Catalogue acceptance history quota exceeded");
    Statement insert(impl_->database.get(), "INSERT INTO acceptance(alias,generation,backend) VALUES(?1,?2,1)");
    insert.text(1, alias); insert.number(2, expected + 1); insert.done();
    transaction.commit(work); return {std::string(alias), expected + 1, {}};
}

} // namespace axiom::knowledge
