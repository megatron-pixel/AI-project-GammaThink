#include "journal.hpp"

#include <algorithm>
#include <sqlite3.h>
#include <sys/stat.h>

namespace axiom::knowledge {
namespace {
constexpr int application_id = 0x474b4a31;
constexpr std::size_t maximum_dependencies = maximum_assessment_dependencies;
constexpr std::string_view assessment_columns =
    "version,state,logical,evidence,checker,reason,certificate,checked_dependencies";

void require(bool condition, ErrorCode code, const char* message) {
    if (!condition) {
        throw Error(code, message);
    }
}

void sql_check(int code, sqlite3* database) {
    if (code == SQLITE_OK || code == SQLITE_ROW || code == SQLITE_DONE) {
        return;
    }
    const auto basic = code & 0xff;
    const auto category = basic == SQLITE_INTERRUPT ? ErrorCode::Budget :
        (basic == SQLITE_CORRUPT || basic == SQLITE_NOTADB ? ErrorCode::Integrity : ErrorCode::Storage);
    throw Error(category, database ? sqlite3_errmsg(database) : "Cannot open SQLite journal");
}

void execute(sqlite3* database, const std::string& sql) {
    sql_check(sqlite3_exec(database, sql.c_str(), nullptr, nullptr, nullptr), database);
}

struct Work {
    sqlite3* database;
    std::uint64_t remaining;
    Work(sqlite3* value, std::uint64_t limit) : database(value), remaining(limit) {
        sqlite3_progress_handler(database, 1, [](void* context) -> int {
            auto& work = *static_cast<Work*>(context);
            if (work.remaining <= 1) {
                work.remaining = 0;
                return 1;
            }
            --work.remaining;
            return 0;
        }, this);
    }
    Work(const Work&) = delete;
    Work& operator=(const Work&) = delete;
    void finish() { sqlite3_progress_handler(database, 0, nullptr, nullptr); }
    ~Work() { finish(); }
};

struct Transaction {
    sqlite3* database;
    explicit Transaction(sqlite3* value) : database(value) { execute(database, "BEGIN IMMEDIATE"); }
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
    // Finalization is a fixed transaction operation, outside the query VM meter.
    // A depleted meter must not interrupt COMMIT or prevent rollback.
    void commit(Work& work) {
        work.finish();
        execute(database, "COMMIT");
    }
    ~Transaction() {
        if (!sqlite3_get_autocommit(database)) {
            sqlite3_exec(database, "ROLLBACK", nullptr, nullptr, nullptr);
        }
    }
};

struct Statement {
    sqlite3* database;
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement{nullptr, sqlite3_finalize};
    Statement(sqlite3* value, const std::string& sql) : database(value) {
        sqlite3_stmt* raw = nullptr;
        const auto code = sqlite3_prepare_v2(database, sql.c_str(), -1, &raw, nullptr);
        statement.reset(raw);
        sql_check(code, database);
    }
    void blob(int index, std::span<const std::uint8_t> value) {
        sql_check(value.empty() ? sqlite3_bind_zeroblob(statement.get(), index, 0) :
            sqlite3_bind_blob64(statement.get(), index, value.data(), value.size(), SQLITE_TRANSIENT),
            database);
    }
    void id(int index, const ObjectId& value) { blob(index, value.digest); }
    void null(int index) { sql_check(sqlite3_bind_null(statement.get(), index), database); }
    void cursor(int index, const std::optional<ObjectId>& value) {
        if (value) {
            id(index, *value);
        } else {
            null(index);
        }
    }
    void string(int index, std::string_view value) {
        sql_check(sqlite3_bind_text64(statement.get(), index, value.empty() ? "" : value.data(),
                                     value.size(), SQLITE_TRANSIENT, SQLITE_UTF8), database);
    }
    void integer(int index, std::uint64_t value) {
        require(value <= static_cast<std::uint64_t>(INT64_MAX), ErrorCode::Schema, "SQLite integer overflow");
        sql_check(sqlite3_bind_int64(statement.get(), index, static_cast<sqlite3_int64>(value)), database);
    }
    bool row() {
        const auto code = sqlite3_step(statement.get());
        sql_check(code, database);
        return code == SQLITE_ROW;
    }
    void done() { require(!row(), ErrorCode::Storage, "Unexpected SQLite result row"); }
    void reset() {
        sql_check(sqlite3_reset(statement.get()), database);
        sql_check(sqlite3_clear_bindings(statement.get()), database);
    }
    bool is_null(int column) const {
        return sqlite3_column_type(statement.get(), column) == SQLITE_NULL;
    }
    std::uint64_t number(int column) const {
        require(sqlite3_column_type(statement.get(), column) == SQLITE_INTEGER,
                ErrorCode::Integrity, "Invalid journal integer");
        const auto result = sqlite3_column_int64(statement.get(), column);
        require(result >= 0, ErrorCode::Integrity, "Negative journal integer");
        return static_cast<std::uint64_t>(result);
    }
    std::span<const std::uint8_t> bytes(int column) const {
        require(sqlite3_column_type(statement.get(), column) == SQLITE_BLOB,
                ErrorCode::Integrity, "Invalid journal blob");
        const auto size = sqlite3_column_bytes(statement.get(), column);
        require(size >= 0 && static_cast<std::size_t>(size) <= maximum_object_bytes,
                ErrorCode::Budget, "Journal blob limit exceeded");
        return {static_cast<const std::uint8_t*>(sqlite3_column_blob(statement.get(), column)),
                static_cast<std::size_t>(size)};
    }
    ObjectId identity(int column) const {
        const auto value = bytes(column);
        ObjectId result;
        require(value.size() == result.digest.size(), ErrorCode::Integrity, "Invalid journal identity");
        std::copy(value.begin(), value.end(), result.digest.begin());
        return result;
    }
    std::string text(int column) const {
        require(sqlite3_column_type(statement.get(), column) == SQLITE_TEXT,
                ErrorCode::Integrity, "Invalid journal text");
        const auto size = sqlite3_column_bytes(statement.get(), column);
        require(size >= 0 && static_cast<std::size_t>(size) <= maximum_text_bytes,
                ErrorCode::Budget, "Journal text limit exceeded");
        const auto* value = sqlite3_column_text(statement.get(), column);
        std::string result(reinterpret_cast<const char*>(value), static_cast<std::size_t>(size));
        validate_text(result);
        return result;
    }
};

void limit_rows(std::size_t limit) {
    require(limit > 0 && limit <= maximum_items, ErrorCode::Budget, "Journal row limit exceeded");
}

ObjectPage page(Statement& query, std::size_t limit) {
    ObjectPage result;
    while (query.row()) {
        if (result.objects.size() == limit) {
            result.complete = false;
            result.next = result.objects.back();
            break;
        }
        result.objects.push_back(query.identity(0));
    }
    return result;
}

void validate_assessment(const Assessment& value) {
    require(value.version == 0, ErrorCode::Schema, "Assessment version is journal-owned");
    require(value.state >= KnowledgeState::Provisional && value.state <= KnowledgeState::Archived,
            ErrorCode::Schema, "Unknown knowledge state");
    require(value.logical >= LogicalValidity::Unchecked && value.logical <= LogicalValidity::Invalid,
            ErrorCode::Schema, "Unknown logical validity");
    require(value.evidence >= EvidenceAssessment::Unassessed &&
                value.evidence <= EvidenceAssessment::Conflicting,
            ErrorCode::Schema, "Unknown evidence assessment");
    require(!value.checker.empty(), ErrorCode::Schema, "Missing checker identity/version");
    validate_text(value.checker);
    validate_text(value.reason);
    require(value.checked_dependencies.size() <= maximum_dependencies,
            ErrorCode::Budget, "Assessment dependency limit exceeded");
}

Assessment read_assessment(const Statement& query) {
    Assessment result;
    const auto version = query.number(0), state = query.number(1), logical = query.number(2);
    const auto evidence = query.number(3);
    require(version > 0 && state >= 1 && state <= 6 && logical >= 1 && logical <= 3 &&
                evidence >= 1 && evidence <= 4,
            ErrorCode::Integrity, "Invalid stored assessment");
    result.state = static_cast<KnowledgeState>(state);
    result.logical = static_cast<LogicalValidity>(logical);
    result.evidence = static_cast<EvidenceAssessment>(evidence);
    result.checker = query.text(4);
    result.reason = query.text(5);
    if (!query.is_null(6)) {
        result.certificate = query.identity(6);
    }
    const auto dependencies = query.bytes(7);
    require(dependencies.size() % Digest{}.size() == 0 &&
                dependencies.size() / Digest{}.size() <= maximum_dependencies,
            ErrorCode::Integrity, "Invalid stored assessment dependencies");
    for (std::size_t offset = 0; offset < dependencies.size(); offset += Digest{}.size()) {
        ObjectId id;
        std::copy_n(dependencies.begin() + static_cast<std::ptrdiff_t>(offset), id.digest.size(),
                    id.digest.begin());
        result.checked_dependencies.push_back(id);
    }
    validate_assessment(result);
    result.version = version;
    return result;
}

std::optional<Assessment> latest(sqlite3* database, const ObjectId& id) {
    Statement query(database, "SELECT " + std::string(assessment_columns) +
        " FROM assessments WHERE object=?1 ORDER BY version DESC LIMIT 1");
    query.id(1, id);
    return query.row() ? std::optional<Assessment>(read_assessment(query)) : std::nullopt;
}

Assessment append_assessment(sqlite3* database, const ObjectId& id, Assessment value) {
    validate_assessment(value);
    const auto previous = latest(database, id);
    require(!previous || previous->version < static_cast<std::uint64_t>(INT64_MAX),
            ErrorCode::Budget, "Assessment version limit exceeded");
    value.version = previous ? previous->version + 1 : 1;
    Bytes dependencies;
    for (const auto& dependency : value.checked_dependencies) {
        dependencies.insert(dependencies.end(), dependency.digest.begin(), dependency.digest.end());
    }
    Statement insert(database, "INSERT INTO assessments VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9)");
    insert.id(1, id);
    insert.integer(2, value.version);
    insert.integer(3, static_cast<std::uint8_t>(value.state));
    insert.integer(4, static_cast<std::uint8_t>(value.logical));
    insert.integer(5, static_cast<std::uint8_t>(value.evidence));
    insert.string(6, value.checker);
    insert.string(7, value.reason);
    if (value.certificate) {
        insert.id(8, *value.certificate);
    } else {
        insert.null(8);
    }
    insert.blob(9, dependencies);
    insert.done();
    Statement edge(database, "INSERT OR IGNORE INTO verification_dependencies VALUES(?1,?2,?3)");
    auto indexed_dependencies = value.checked_dependencies;
    if (value.certificate && *value.certificate != id) {
        indexed_dependencies.push_back(*value.certificate);
    }
    for (const auto& dependency : indexed_dependencies) {
        edge.id(1, id);
        edge.integer(2, value.version);
        edge.id(3, dependency);
        edge.done();
        edge.reset();
    }
    return value;
}

} // namespace

void Journal::Close::operator()(sqlite3* database) const noexcept {
    sqlite3_close_v2(database);
}

Journal::Journal(const std::filesystem::path& path, JournalLimits limits) : limits_(limits) {
    require(limits.cache_kib >= 64 && limits.cache_kib <= 65536 && limits.sql_instructions > 0,
            ErrorCode::Budget, "Invalid journal limits");
    const auto name = path.string();
    validate_text(name);
    require(!name.empty(), ErrorCode::Schema, "Missing journal path");
    require(sqlite3_libversion_number() >= 3037000, ErrorCode::Schema, "SQLite 3.37 or newer is required");
    sqlite3* raw = nullptr;
    const auto code = sqlite3_open_v2(name.c_str(), &raw, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
    database_.reset(raw);
    sql_check(code, raw);
    auto* database = database_.get();
    sql_check(sqlite3_extended_result_codes(database, 1), database);
    execute(database, "PRAGMA cache_size=-" + std::to_string(limits.cache_kib));
    execute(database, "PRAGMA temp_store=FILE");
    Statement app(database, "PRAGMA application_id");
    require(app.row(), ErrorCode::Storage, "Missing journal application ID");
    const auto identity = app.number(0);
    app.reset();
    Statement version(database, "PRAGMA user_version");
    require(version.row(), ErrorCode::Storage, "Missing journal schema version");
    const auto schema = version.number(0);
    version.reset();
    if (identity == 0 && schema == 0) {
        Statement tables(database,
            "SELECT count(*) FROM sqlite_schema WHERE type IN ('table','view') AND name NOT LIKE 'sqlite_%'");
        require(tables.row() && tables.number(0) == 0,
                ErrorCode::Schema, "Refusing to modify an unrelated SQLite database");
    } else {
        require(identity == application_id && schema == schema_version,
                ErrorCode::Schema, "Unsupported journal identity/schema");
    }
    execute(database, "PRAGMA foreign_keys=ON");
    execute(database, "PRAGMA trusted_schema=OFF");
    execute(database, "PRAGMA journal_mode=WAL");
    execute(database, "PRAGMA synchronous=FULL");
    if (identity == 0) {
        Transaction transaction(database);
        Work work(database, limits_.sql_instructions);
        execute(database, R"SQL(
CREATE TABLE objects(
 id BLOB PRIMARY KEY CHECK(length(id)=32),
 kind INTEGER NOT NULL CHECK(kind BETWEEN 1 AND 12),
 visibility INTEGER NOT NULL CHECK(visibility BETWEEN 1 AND 2),
 wire BLOB NOT NULL CHECK(length(wire)<=4194304)
) STRICT, WITHOUT ROWID;
CREATE INDEX objects_kind ON objects(kind,id);
CREATE TABLE concepts(
 source BLOB NOT NULL REFERENCES objects(id),
 ordinal INTEGER NOT NULL, value TEXT NOT NULL,
 PRIMARY KEY(source,ordinal)
) STRICT, WITHOUT ROWID;
CREATE INDEX concepts_value ON concepts(value,source);
CREATE TABLE source_locations(
 source BLOB NOT NULL REFERENCES objects(id), ordinal INTEGER NOT NULL,
 archive TEXT NOT NULL, version TEXT NOT NULL, path TEXT NOT NULL,
 PRIMARY KEY(source,ordinal)
) STRICT, WITHOUT ROWID;
CREATE INDEX source_locations_archive ON source_locations(archive,version,source);
CREATE TABLE links(
 source BLOB NOT NULL REFERENCES objects(id), ordinal INTEGER NOT NULL,
 target BLOB NOT NULL CHECK(length(target)=32),
 relationship INTEGER NOT NULL CHECK(relationship BETWEEN 1 AND 8),
 PRIMARY KEY(source,ordinal)
) STRICT, WITHOUT ROWID;
CREATE INDEX links_target ON links(target,relationship,source);
CREATE TABLE dependencies(
 source BLOB NOT NULL REFERENCES objects(id), target BLOB NOT NULL CHECK(length(target)=32),
 PRIMARY KEY(source,target)
) STRICT, WITHOUT ROWID;
CREATE INDEX dependencies_target ON dependencies(target,source);
CREATE VIRTUAL TABLE object_text USING fts5(id UNINDEXED,content,tokenize='unicode61');
CREATE TABLE assessments(
 object BLOB NOT NULL CHECK(length(object)=32),
 version INTEGER NOT NULL CHECK(version>0),
 state INTEGER NOT NULL CHECK(state BETWEEN 1 AND 6),
 logical INTEGER NOT NULL CHECK(logical BETWEEN 1 AND 3),
 evidence INTEGER NOT NULL CHECK(evidence BETWEEN 1 AND 4),
 checker TEXT NOT NULL, reason TEXT NOT NULL,
 certificate BLOB CHECK(certificate IS NULL OR length(certificate)=32),
 checked_dependencies BLOB NOT NULL CHECK(length(checked_dependencies)%32=0 AND length(checked_dependencies)<=524320),
 PRIMARY KEY(object,version)
) STRICT, WITHOUT ROWID;
CREATE TABLE verification_dependencies(
 source BLOB NOT NULL, version INTEGER NOT NULL,
 target BLOB NOT NULL CHECK(length(target)=32),
 PRIMARY KEY(source,version,target),
 FOREIGN KEY(source,version) REFERENCES assessments(object,version)
) STRICT, WITHOUT ROWID;
CREATE INDEX verification_dependencies_target ON verification_dependencies(target,source,version);
PRAGMA application_id=0x474b4a31;
PRAGMA user_version=1;
)SQL");
        transaction.commit(work);
    }
}

Journal::~Journal() = default;

void Journal::read_snapshot(const std::function<void()>& read) {
    auto* database = database_.get();
    require(static_cast<bool>(read), ErrorCode::Schema, "Missing journal snapshot reader");
    require(sqlite3_get_autocommit(database), ErrorCode::Permission, "Nested journal snapshot");
    execute(database, "PRAGMA query_only=ON");
    try {
        execute(database, "BEGIN");
        {
            Work work(database, limits_.sql_instructions);
            Statement pin(database, "SELECT id FROM objects LIMIT 1");
            pin.row();
        }
        read();
        execute(database, "COMMIT");
        execute(database, "PRAGMA query_only=OFF");
    } catch (...) {
        sqlite3_exec(database, "ROLLBACK", nullptr, nullptr, nullptr);
        sqlite3_exec(database, "PRAGMA query_only=OFF", nullptr, nullptr, nullptr);
        throw;
    }
}

JournalCopy Journal::compact_copy(const std::filesystem::path& directory, std::uint64_t maximum_bytes) {
    validate_text(directory.string());
    require(!directory.empty(), ErrorCode::Schema, "Missing compact copy directory");
    require(maximum_bytes > 0 && maximum_bytes <= 1024ULL * 1024 * 1024,
        ErrorCode::Budget, "Invalid compact copy byte budget");
    require(sqlite3_get_autocommit(database_.get()), ErrorCode::Permission, "Cannot copy inside a journal snapshot");
    JournalCopy result;
    result.database = std::filesystem::absolute(directory) / "journal.db";
    const auto scalar = [](sqlite3* database, const std::string& sql) {
        Statement query(database, sql);
        require(query.row(), ErrorCode::Storage, "Missing compact copy count");
        return query.number(0);
    };
    read_snapshot([&] {
        Work source_work(database_.get(), limits_.sql_instructions);
        const auto pages = scalar(database_.get(), "PRAGMA page_count");
        const auto page_size = scalar(database_.get(), "PRAGMA page_size");
        require(page_size > 0 && pages <= maximum_bytes / page_size,
            ErrorCode::Budget, "Journal exceeds compact copy byte budget");
        result.source_bytes = pages * page_size;
        result.objects = scalar(database_.get(), "SELECT count(*) FROM objects");
        result.assessments = scalar(database_.get(), "SELECT count(*) FROM assessments");
        {
            Statement records(database_.get(), "SELECT id,kind,visibility,wire FROM objects ORDER BY id");
            while (records.row()) {
                const auto object = decode_object(records.identity(0), records.bytes(3));
                require(records.number(1) == static_cast<std::uint8_t>(object.kind)
                    && records.number(2) == static_cast<std::uint8_t>(object.visibility),
                    ErrorCode::Integrity, "Compact copy object index differs");
            }
            Statement assessments(database_.get(), "SELECT " + std::string(assessment_columns) + " FROM assessments");
            while (assessments.row()) (void)read_assessment(assessments);
        }
        require(mkdir(directory.c_str(), 0700) == 0, ErrorCode::Permission, "Compact copy directory must be new");
        sqlite3* raw = nullptr;
        const auto code = sqlite3_open_v2(result.database.c_str(), &raw, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
        std::unique_ptr<sqlite3, Close> output(raw); sql_check(code, raw);
        execute(raw, "PRAGMA cache_size=-" + std::to_string(limits_.cache_kib));
        execute(raw, "PRAGMA temp_store=FILE");
        execute(raw, "PRAGMA synchronous=FULL");
        auto* pending = sqlite3_backup_init(raw, "main", database_.get(), "main");
        require(pending != nullptr, ErrorCode::Storage, "Cannot initialize coherent journal copy");
        std::unique_ptr<sqlite3_backup, decltype(&sqlite3_backup_finish)> backup(pending, sqlite3_backup_finish);
        int step = SQLITE_OK;
        while (step != SQLITE_DONE) {
            step = sqlite3_backup_step(backup.get(), 256);
            sql_check(step, raw); // Busy/locked errors are reported, not retried.
            require(sqlite3_backup_pagecount(backup.get()) >= 0
                && static_cast<std::uint64_t>(sqlite3_backup_pagecount(backup.get())) <= pages,
                ErrorCode::Budget, "Coherent journal copy page budget changed");
        }
        sql_check(sqlite3_backup_finish(backup.release()), raw);
        execute(raw, "PRAGMA journal_mode=DELETE");
        execute(raw, "PRAGMA max_page_count=" + std::to_string(maximum_bytes / page_size));
        Work output_work(raw, limits_.sql_instructions);
        execute(raw, "VACUUM");
        execute(raw, "INSERT INTO object_text(object_text) VALUES('integrity-check')");
        Statement integrity(raw, "PRAGMA integrity_check(1)");
        require(integrity.row() && integrity.text(0) == "ok" && !integrity.row(),
            ErrorCode::Integrity, "Compacted journal integrity check failed");
        Statement foreign_keys(raw, "PRAGMA foreign_key_check");
        require(!foreign_keys.row(), ErrorCode::Integrity, "Compacted journal foreign key check failed");
        require(scalar(raw, "SELECT count(*) FROM objects") == result.objects
            && scalar(raw, "SELECT count(*) FROM assessments") == result.assessments,
            ErrorCode::Integrity, "Compacted journal history count differs");
        const auto copied_pages = scalar(raw, "PRAGMA page_count");
        const auto copied_page_size = scalar(raw, "PRAGMA page_size");
        require(copied_page_size > 0 && copied_pages <= maximum_bytes / copied_page_size,
            ErrorCode::Budget, "Compacted journal exceeds output byte budget");
        result.copy_bytes = copied_pages * copied_page_size;
    });
    return result;
}

ObjectId Journal::put(const Object& object) {
    const auto wire = encode_object(object);
    const ObjectId id{digest(wire)};
    const auto indexed = indexed_text(object);
    auto* database = database_.get();
    Transaction transaction(database);
    Work work(database, limits_.sql_instructions);
    Statement existing(database, "SELECT kind,visibility,wire FROM objects WHERE id=?1");
    existing.id(1, id);
    if (existing.row()) {
        const auto previous = existing.bytes(2);
        require(existing.number(0) == static_cast<std::uint8_t>(object.kind) &&
                    existing.number(1) == static_cast<std::uint8_t>(object.visibility) &&
                    previous.size() == wire.size() && std::equal(previous.begin(), previous.end(), wire.begin()),
                ErrorCode::Integrity, "Existing journal object disagrees with its identity");
        existing.reset();
        transaction.commit(work);
        return id;
    }
    Statement insert(database, "INSERT INTO objects VALUES(?1,?2,?3,?4)");
    insert.id(1, id);
    insert.integer(2, static_cast<std::uint8_t>(object.kind));
    insert.integer(3, static_cast<std::uint8_t>(object.visibility));
    insert.blob(4, wire);
    insert.done();
    Statement concept_insert(database, "INSERT INTO concepts VALUES(?1,?2,?3)");
    for (std::size_t i = 0; i < object.concepts.size(); ++i) {
        concept_insert.id(1, id);
        concept_insert.integer(2, i);
        concept_insert.string(3, object.concepts[i]);
        concept_insert.done();
        concept_insert.reset();
    }
    Statement source_insert(database, "INSERT INTO source_locations VALUES(?1,?2,?3,?4,?5)");
    for (std::size_t i = 0; i < object.sources.size(); ++i) {
        const auto& source = object.sources[i];
        source_insert.id(1, id);
        source_insert.integer(2, i);
        source_insert.string(3, source.archive_id);
        source_insert.string(4, source.archive_version);
        source_insert.string(5, source.entry_path);
        source_insert.done();
        source_insert.reset();
    }
    Statement link_insert(database, "INSERT INTO links VALUES(?1,?2,?3,?4)");
    const auto all_links = relationships(object);
    for (std::size_t i = 0; i < all_links.size(); ++i) {
        link_insert.id(1, id);
        link_insert.integer(2, i);
        link_insert.id(3, all_links[i].target);
        link_insert.integer(4, static_cast<std::uint8_t>(all_links[i].relationship));
        link_insert.done();
        link_insert.reset();
    }
    Statement dependency_insert(database, "INSERT INTO dependencies VALUES(?1,?2)");
    for (const auto& dependency : prerequisites(object)) {
        dependency_insert.id(1, id);
        dependency_insert.id(2, dependency);
        dependency_insert.done();
        dependency_insert.reset();
    }
    Statement fulltext(database, "INSERT INTO object_text(id,content) VALUES(?1,?2)");
    fulltext.id(1, id);
    fulltext.string(2, indexed);
    fulltext.done();
    transaction.commit(work);
    return id;
}

bool Journal::contains(const ObjectId& id) {
    auto* database = database_.get();
    Work work(database, limits_.sql_instructions);
    Statement query(database, "SELECT 1 FROM objects WHERE id=?1");
    query.id(1, id);
    return query.row();
}

std::optional<Object> Journal::get(const ObjectId& id) {
    auto* database = database_.get();
    Work work(database, limits_.sql_instructions);
    Statement query(database, "SELECT kind,visibility,wire FROM objects WHERE id=?1");
    query.id(1, id);
    if (!query.row()) {
        return std::nullopt;
    }
    auto result = decode_object(id, query.bytes(2));
    require(query.number(0) == static_cast<std::uint8_t>(result.kind) &&
                query.number(1) == static_cast<std::uint8_t>(result.visibility),
            ErrorCode::Integrity, "Journal object index mismatch");
    return result;
}

ObjectPage Journal::objects(std::optional<ObjectId> after, std::size_t limit) {
    limit_rows(limit);
    auto* database = database_.get();
    Work work(database, limits_.sql_instructions);
    Statement query(database, std::string("SELECT id FROM objects ") +
        (after ? "WHERE id>?1 " : "") + "ORDER BY id LIMIT ?2");
    query.cursor(1, after);
    query.integer(2, limit + 1);
    return page(query, limit);
}

ObjectPage Journal::by_kind(ObjectKind kind, std::optional<ObjectId> after, std::size_t limit) {
    require(kind >= ObjectKind::Claim && kind <= ObjectKind::Contradiction,
            ErrorCode::Schema, "Unknown knowledge kind");
    limit_rows(limit);
    auto* database = database_.get();
    Work work(database, limits_.sql_instructions);
    Statement query(database, std::string("SELECT id FROM objects WHERE kind=?1 ") +
        (after ? "AND id>?2 " : "") + "ORDER BY id LIMIT ?3");
    query.integer(1, static_cast<std::uint8_t>(kind));
    query.cursor(2, after);
    query.integer(3, limit + 1);
    return page(query, limit);
}

ObjectPage Journal::by_concept(std::string_view value, std::optional<ObjectId> after, std::size_t limit) {
    validate_text(value);
    limit_rows(limit);
    auto* database = database_.get();
    Work work(database, limits_.sql_instructions);
    Statement query(database, std::string("SELECT DISTINCT source FROM concepts WHERE value=?1 ") +
        (after ? "AND source>?2 " : "") + "ORDER BY source LIMIT ?3");
    query.string(1, value);
    query.cursor(2, after);
    query.integer(3, limit + 1);
    return page(query, limit);
}

ObjectPage Journal::by_source_archive(std::string_view archive, std::optional<std::string_view> version,
                                     std::optional<ObjectId> after, std::size_t limit) {
    validate_text(archive);
    if (version) {
        validate_text(*version);
    }
    limit_rows(limit);
    auto* database = database_.get();
    Work work(database, limits_.sql_instructions);
    Statement query(database, std::string("SELECT DISTINCT source FROM source_locations WHERE archive=?1 ") +
        (version ? "AND version=?2 " : "") + (after ? "AND source>?3 " : "") +
        "ORDER BY source LIMIT ?4");
    query.string(1, archive);
    if (version) {
        query.string(2, *version);
    } else {
        query.null(2);
    }
    query.cursor(3, after);
    query.integer(4, limit + 1);
    return page(query, limit);
}

ObjectPage Journal::search(std::string_view phrase, std::optional<ObjectId> after, std::size_t limit) {
    validate_text(phrase);
    require(!phrase.empty(), ErrorCode::Schema, "Missing search phrase");
    limit_rows(limit);
    std::string escaped = "\"";
    for (const auto character : phrase) {
        escaped += character;
        if (character == '"') {
            escaped += '"';
        }
    }
    escaped += '"';
    auto* database = database_.get();
    Work work(database, limits_.sql_instructions);
    Statement query(database, std::string("SELECT id FROM object_text WHERE object_text MATCH ?1 ") +
        (after ? "AND id>?2 " : "") + "ORDER BY id LIMIT ?3");
    query.string(1, escaped);
    query.cursor(2, after);
    query.integer(3, limit + 1);
    return page(query, limit);
}

ObjectPage Journal::related(const ObjectId& target, Relationship relationship,
                            std::optional<ObjectId> after, std::size_t limit) {
    require(relationship >= Relationship::Requires && relationship <= Relationship::AlternativeTo,
            ErrorCode::Schema, "Unknown relationship");
    limit_rows(limit);
    auto* database = database_.get();
    Work work(database, limits_.sql_instructions);
    Statement query(database, std::string("SELECT DISTINCT source FROM links WHERE target=?1 AND relationship=?2 ") +
        (after ? "AND source>?3 " : "") + "ORDER BY source LIMIT ?4");
    query.id(1, target);
    query.integer(2, static_cast<std::uint8_t>(relationship));
    query.cursor(3, after);
    query.integer(4, limit + 1);
    return page(query, limit);
}

ObjectPage Journal::required_by(const ObjectId& dependency, std::optional<ObjectId> after, std::size_t limit) {
    limit_rows(limit);
    auto* database = database_.get();
    Work work(database, limits_.sql_instructions);
    Statement query(database, std::string(R"SQL(
SELECT DISTINCT source FROM (
 SELECT source FROM dependencies WHERE target=?1
 UNION SELECT v.source FROM verification_dependencies v WHERE v.target=?1
 AND v.version=(SELECT max(a.version) FROM assessments a WHERE a.object=v.source)
) WHERE 1=1 )SQL") + (after ? "AND source>?2 " : "") + "ORDER BY source LIMIT ?3");
    query.id(1, dependency);
    query.cursor(2, after);
    query.integer(3, limit + 1);
    return page(query, limit);
}

Assessment Journal::record_assessment(const ObjectId& id, Assessment value) {
    validate_assessment(value);
    auto* database = database_.get();
    Transaction transaction(database);
    Work work(database, limits_.sql_instructions);
    auto result = append_assessment(database, id, std::move(value));
    transaction.commit(work);
    return result;
}

Assessment Journal::record_revalidated_assessment(const ObjectId& id, Assessment value,
                                                  std::span<const ObservedAssessment> observed) {
    validate_assessment(value);
    require(observed.size() <= maximum_items * 4 + 1, ErrorCode::Budget,
            "Too many observed dependency versions");
    std::vector<ObservedAssessment> versions(observed.begin(), observed.end());
    std::sort(versions.begin(), versions.end(),
              [](const auto& a, const auto& b) { return a.object < b.object; });
    for (std::size_t i = 0; i < versions.size(); ++i) {
        require(versions[i].version <= static_cast<std::uint64_t>(INT64_MAX),
                ErrorCode::Schema, "Invalid observed dependency version");
        require(i == 0 || versions[i - 1].object != versions[i].object,
                ErrorCode::Schema, "Duplicate observed dependency");
    }
    auto guarded_dependencies = value.checked_dependencies;
    if (value.certificate && *value.certificate != id) {
        guarded_dependencies.push_back(*value.certificate);
    }
    for (const auto& dependency : guarded_dependencies) {
        const auto found = std::lower_bound(versions.begin(), versions.end(), dependency,
            [](const auto& version, const auto& object) { return version.object < object; });
        require(found != versions.end() && found->object == dependency, ErrorCode::Schema,
                "Missing observed version for checked dependency");
    }
    auto* database = database_.get();
    Transaction transaction(database);
    Work work(database, limits_.sql_instructions);
    for (const auto& version : versions) {
        const auto current = latest(database, version.object);
        require((current ? current->version : 0) == version.version, ErrorCode::Unavailable,
                "Dependency assessment changed during verification");
    }
    auto result = append_assessment(database, id, std::move(value));
    transaction.commit(work);
    return result;
}

std::optional<Assessment> Journal::assessment(const ObjectId& id) {
    auto* database = database_.get();
    Work work(database, limits_.sql_instructions);
    return latest(database, id);
}

AssessmentPage Journal::assessment_history(const ObjectId& id, std::uint64_t after, std::size_t limit) {
    limit_rows(limit);
    require(after <= static_cast<std::uint64_t>(INT64_MAX), ErrorCode::Schema, "Invalid assessment cursor");
    auto* database = database_.get();
    Work work(database, limits_.sql_instructions);
    Statement query(database, "SELECT " + std::string(assessment_columns) +
        " FROM assessments WHERE object=?1 AND version>?2 ORDER BY version LIMIT ?3");
    query.id(1, id);
    query.integer(2, after);
    query.integer(3, limit + 1);
    AssessmentPage result;
    while (query.row()) {
        if (result.records.size() == limit) {
            result.complete = false;
            result.next = result.records.back().version;
            break;
        }
        result.records.push_back(read_assessment(query));
    }
    return result;
}

std::vector<ObjectId> Journal::invalidate(const ObjectId& dependency, std::string_view reason,
                                         std::size_t maximum_affected) {
    limit_rows(maximum_affected);
    validate_text(reason);
    require(!reason.empty(), ErrorCode::Schema, "Missing invalidation reason");
    auto* database = database_.get();
    Transaction transaction(database);
    Work work(database, limits_.sql_instructions);
    Statement query(database, R"SQL(
WITH RECURSIVE affected(id) AS (
 VALUES(?1)
 UNION SELECT d.source FROM dependencies d JOIN affected p ON d.target=p.id
 UNION SELECT v.source FROM verification_dependencies v JOIN affected p ON v.target=p.id
 WHERE v.version=(SELECT max(a.version) FROM assessments a WHERE a.object=v.source)
 LIMIT ?2
)
SELECT id FROM affected p
WHERE EXISTS(SELECT 1 FROM objects o WHERE o.id=p.id)
   OR EXISTS(SELECT 1 FROM assessments a WHERE a.object=p.id)
)SQL");
    query.id(1, dependency);
    query.integer(2, maximum_affected + 2);
    std::vector<ObjectId> affected;
    while (query.row()) {
        require(affected.size() < maximum_affected, ErrorCode::Budget, "Invalidation closure limit exceeded");
        affected.push_back(query.identity(0));
    }
    for (const auto& id : affected) {
        auto value = latest(database, id).value_or(Assessment{});
        value.version = 0;
        value.state = KnowledgeState::Invalidated;
        value.checker = "dependency-revalidation-v1";
        value.reason = std::string(reason);
        append_assessment(database, id, std::move(value));
    }
    transaction.commit(work);
    std::sort(affected.begin(), affected.end());
    return affected;
}

} // namespace axiom::knowledge
