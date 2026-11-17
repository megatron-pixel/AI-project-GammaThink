#include "archive_registry.hpp"
#include "gks_snapshot.hpp"

#include <algorithm>
#include <sqlite3.h>

namespace axiom::knowledge {
namespace {
constexpr int application_id = 0x474b5231;
constexpr std::string_view mount_columns = "m.alias,m.generation,m.archive,m.revision,m.directory,v.root,m.backend";
void require(bool condition, ErrorCode code, const char* message) {
    if (!condition) throw Error(code, message);
}
void label(std::string_view value) {
    validate_text(value, 128);
    require(!value.empty(), ErrorCode::Schema, "Missing archive or mount label");
}
void sql_check(int code, sqlite3* database) {
    if (code == SQLITE_OK || code == SQLITE_ROW || code == SQLITE_DONE) return;
    const auto basic = code & 0xff;
    throw Error(basic == SQLITE_INTERRUPT || basic == SQLITE_FULL ? ErrorCode::Budget
        : basic == SQLITE_CORRUPT || basic == SQLITE_NOTADB ? ErrorCode::Integrity : ErrorCode::Storage,
        sqlite3_errmsg(database));
}
void execute(sqlite3* database, const std::string& text) {
    sql_check(sqlite3_exec(database, text.c_str(), nullptr, nullptr, nullptr), database);
}
struct Work {
    sqlite3* database;
    std::uint64_t remaining;
    Work(sqlite3* db, std::uint64_t limit) : database(db), remaining(limit) {
        sqlite3_progress_handler(db, 1, [](void* pointer) -> int {
            auto& left = static_cast<Work*>(pointer)->remaining;
            if (left <= 1) { left = 0; return 1; }
            --left; return 0;
        }, this);
    }
    void finish() { sqlite3_progress_handler(database, 0, nullptr, nullptr); }
    ~Work() { finish(); }
    Work(const Work&) = delete;
    Work& operator=(const Work&) = delete;
};
struct Transaction {
    sqlite3* database;
    explicit Transaction(sqlite3* db) : database(db) { execute(db, "BEGIN IMMEDIATE"); }
    void commit(Work& work) { work.finish(); execute(database, "COMMIT"); }
    ~Transaction() {
        if (!sqlite3_get_autocommit(database)) sqlite3_exec(database, "ROLLBACK", nullptr, nullptr, nullptr);
    }
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
};
struct Statement {
    sqlite3* database;
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> value{nullptr, sqlite3_finalize};
    Statement(sqlite3* db, const std::string& text) : database(db) {
        sqlite3_stmt* raw = nullptr;
        const auto code = sqlite3_prepare_v2(db, text.c_str(), -1, &raw, nullptr);
        value.reset(raw); sql_check(code, db);
    }
    void text(int slot, std::string_view text) {
        sql_check(sqlite3_bind_text64(value.get(), slot, text.data(), text.size(), SQLITE_TRANSIENT, SQLITE_UTF8), database);
    }
    void integer(int slot, std::uint64_t number) {
        require(number <= INT64_MAX, ErrorCode::Budget, "Registry integer overflow");
        sql_check(sqlite3_bind_int64(value.get(), slot, number), database);
    }
    void hash(int slot, const BlockId& id) {
        sql_check(sqlite3_bind_blob(value.get(), slot, id.digest.data(), id.digest.size(), SQLITE_TRANSIENT), database);
    }
    bool row() {
        const auto code = sqlite3_step(value.get());
        sql_check(code, database); return code == SQLITE_ROW;
    }
    void done() {
        require(!row(), ErrorCode::Storage, "Unexpected registry result row");
        sql_check(sqlite3_reset(value.get()), database);
    }
    bool absent(int column) const { return sqlite3_column_type(value.get(), column) == SQLITE_NULL; }
    std::uint64_t number(int column) const {
        require(sqlite3_column_type(value.get(), column) == SQLITE_INTEGER
            && sqlite3_column_int64(value.get(), column) >= 0, ErrorCode::Integrity, "Invalid registry integer");
        return static_cast<std::uint64_t>(sqlite3_column_int64(value.get(), column));
    }
    std::string text(int column) const {
        require(sqlite3_column_type(value.get(), column) == SQLITE_TEXT, ErrorCode::Integrity, "Invalid registry text");
        const auto size = sqlite3_column_bytes(value.get(), column);
        require(size >= 0 && static_cast<std::size_t>(size) <= maximum_text_bytes,
                ErrorCode::Budget, "Registry text exceeds bounds");
        const auto* data = reinterpret_cast<const char*>(sqlite3_column_text(value.get(), column));
        std::string result(data, static_cast<std::size_t>(size));
        validate_text(result); return result;
    }
    BlockId hash(int column) const {
        require(sqlite3_column_type(value.get(), column) == SQLITE_BLOB
            && sqlite3_column_bytes(value.get(), column) == 32, ErrorCode::Integrity, "Invalid registry root identity");
        BlockId result;
        const auto* data = static_cast<const std::uint8_t*>(sqlite3_column_blob(value.get(), column));
        std::copy_n(data, result.digest.size(), result.digest.begin()); return result;
    }
};
std::uint64_t count(sqlite3* database, const std::string& query) {
    Statement statement(database, query);
    require(statement.row(), ErrorCode::Storage, "Missing registry count");
    return statement.number(0);
}
std::optional<ArchiveVersion> version(sqlite3* database, std::string_view name, std::string_view revision) {
    Statement query(database, "SELECT root FROM versions WHERE name=?1 AND revision=?2");
    query.text(1, name); query.text(2, revision);
    if (!query.row()) return {};
    return ArchiveVersion{std::string(name), std::string(revision), query.hash(0)};
}
MountState read_mount(const Statement& query) {
    MountState result{query.text(0), query.number(1), {}};
    label(result.alias);
    require(result.generation > 0, ErrorCode::Integrity, "Invalid registry mount generation");
    if (query.absent(2)) {
        require(query.absent(3) && query.absent(4) && query.absent(5) && query.number(6) == 1,
                ErrorCode::Integrity, "Invalid mount tombstone");
        return result;
    }
    ArchiveVersion archive{query.text(2), query.text(3), query.hash(5)};
    label(archive.name); label(archive.revision);
    const auto backend = query.number(6);
    require(backend >= 1 && backend <= 3, ErrorCode::Integrity, "Invalid stored mount backend");
    MountBinding binding{std::move(archive), {}};
    binding.backend = static_cast<MountBackend>(backend);
    if (binding.backend == MountBackend::Local) {
        binding.directory = query.text(4);
        require(binding.directory.is_absolute(), ErrorCode::Integrity, "Relative stored mount directory");
    } else { binding.source = query.text(4); label(binding.source); }
    result.binding = std::move(binding);
    return result;
}
MountState state(sqlite3* database, std::string_view alias) {
    Statement query(database, "SELECT " + std::string(mount_columns)
        + " FROM mounts m LEFT JOIN versions v ON v.name=m.archive AND v.revision=m.revision WHERE m.alias=?1");
    query.text(1, alias);
    return query.row() ? read_mount(query) : MountState{std::string(alias), 0, {}};
}
void generation(const MountState& current, std::uint64_t expected) {
    require(current.generation == expected, ErrorCode::Unavailable, "Stale mount generation");
    require(expected < INT64_MAX, ErrorCode::Budget, "Mount generation exhausted");
}
}

void ArchiveRegistry::Close::operator()(sqlite3* database) const noexcept { sqlite3_close_v2(database); }
ArchiveRegistry::ArchiveRegistry(const std::filesystem::path& path, RegistryLimits limits) : limits_(limits) {
    validate_text(path.string());
    require(!path.empty(), ErrorCode::Schema, "Missing registry path");
    require(limits.cache_kib >= 64 && limits.cache_kib <= 65536 && limits.sql_instructions > 0
        && limits.versions > 0 && limits.versions <= maximum_items && limits.aliases > 0
        && limits.aliases <= maximum_items && limits.active_mounts <= 16,
        ErrorCode::Budget, "Invalid registry limits");
    require(sqlite3_libversion_number() >= 3037000, ErrorCode::Schema, "SQLite 3.37 or newer is required");
    sqlite3* raw = nullptr;
    const auto code = sqlite3_open_v2(path.c_str(), &raw, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
    database_.reset(raw); sql_check(code, raw);
    sql_check(sqlite3_extended_result_codes(raw, 1), raw);
    execute(raw, "PRAGMA cache_size=-" + std::to_string(limits.cache_kib));
    execute(raw, "PRAGMA temp_store=FILE");
    const auto identity = count(raw, "PRAGMA application_id"), schema = count(raw, "PRAGMA user_version");
    if (!identity && !schema) {
        require(count(raw, "SELECT count(*) FROM sqlite_schema WHERE type IN ('table','view')"
                           " AND name NOT LIKE 'sqlite_%'") == 0,
                ErrorCode::Schema, "Refusing an unrelated registry database");
    } else {
        require(identity == application_id && (schema == 1 || schema == 2),
                ErrorCode::Schema, "Unsupported archive registry schema");
    }
    execute(raw, "PRAGMA foreign_keys=ON");
    execute(raw, "PRAGMA trusted_schema=OFF");
    execute(raw, "PRAGMA journal_mode=WAL");
    execute(raw, "PRAGMA synchronous=FULL");
    if (!identity) {
        Transaction transaction(raw); Work work(raw, limits.sql_instructions);
        execute(raw, R"SQL(
CREATE TABLE versions(
 name TEXT NOT NULL, revision TEXT NOT NULL, root BLOB NOT NULL CHECK(length(root)=32),
 PRIMARY KEY(name,revision)
) STRICT, WITHOUT ROWID;
CREATE TABLE mounts(
 alias TEXT PRIMARY KEY, generation INTEGER NOT NULL CHECK(generation>0),
 archive TEXT, revision TEXT, directory TEXT,
 backend INTEGER NOT NULL DEFAULT 1 CHECK(backend BETWEEN 1 AND 3),
 FOREIGN KEY(archive,revision) REFERENCES versions(name,revision),
 CHECK((archive IS NULL AND revision IS NULL AND directory IS NULL)
    OR (archive IS NOT NULL AND revision IS NOT NULL AND directory IS NOT NULL))
) STRICT, WITHOUT ROWID;
PRAGMA application_id=0x474b5231;
PRAGMA user_version=2;
)SQL");
        transaction.commit(work);
    } else if (schema == 1) {
        Transaction transaction(raw); Work work(raw, limits.sql_instructions);
        if (count(raw, "PRAGMA user_version") == 1) {
            execute(raw, "ALTER TABLE mounts ADD COLUMN backend INTEGER NOT NULL DEFAULT 1 CHECK(backend BETWEEN 1 AND 3)");
            execute(raw, "PRAGMA user_version=2");
        }
        transaction.commit(work);
    }
}
ArchiveRegistry::~ArchiveRegistry() = default;

ArchiveVersion ArchiveRegistry::register_snapshot(const std::filesystem::path& directory, const BlockId& root) {
    GksSnapshot snapshot(directory, root);
    return register_version({snapshot.manifest().name, snapshot.manifest().revision, root});
}
ArchiveVersion ArchiveRegistry::register_snapshot(std::shared_ptr<BlockSource> source, const BlockId& root) {
    GksSnapshot snapshot(std::move(source), root);
    return register_version({snapshot.manifest().name, snapshot.manifest().revision, root});
}
ArchiveVersion ArchiveRegistry::register_version(const ArchiveVersion& archive) {
    label(archive.name); label(archive.revision);
    auto* database = database_.get();
    Transaction transaction(database); Work work(database, limits_.sql_instructions);
    const auto old = version(database, archive.name, archive.revision);
    if (old) {
        require(*old == archive, ErrorCode::Integrity, "Archive version already binds another root");
    } else {
        require(count(database, "SELECT count(*) FROM versions") < limits_.versions,
                ErrorCode::Budget, "Archive version quota exceeded");
        Statement insert(database, "INSERT INTO versions VALUES(?1,?2,?3)");
        insert.text(1, archive.name); insert.text(2, archive.revision); insert.hash(3, archive.root); insert.done();
    }
    transaction.commit(work); return archive;
}
std::optional<ArchiveVersion> ArchiveRegistry::lookup(std::string_view name, std::string_view revision) {
    label(name); label(revision);
    Work work(database_.get(), limits_.sql_instructions);
    return version(database_.get(), name, revision);
}
MountState ArchiveRegistry::resolve(std::string_view alias) {
    label(alias);
    Work work(database_.get(), limits_.sql_instructions);
    return state(database_.get(), alias);
}
std::vector<MountState> ArchiveRegistry::mounts() {
    Work work(database_.get(), limits_.sql_instructions);
    Statement query(database_.get(), "SELECT " + std::string(mount_columns)
        + " FROM mounts m LEFT JOIN versions v ON v.name=m.archive AND v.revision=m.revision"
          " WHERE m.archive IS NOT NULL ORDER BY m.alias LIMIT " + std::to_string(limits_.active_mounts + 1));
    std::vector<MountState> result;
    while (query.row()) {
        require(result.size() < limits_.active_mounts, ErrorCode::Budget, "Active mount quota exceeded");
        result.push_back(read_mount(query));
    }
    return result;
}
MountState ArchiveRegistry::mount(std::string_view alias, std::string_view name, std::string_view revision,
                                  const std::filesystem::path& directory, std::uint64_t expected_generation) {
    label(alias); label(name); label(revision);
    generation(resolve(alias), expected_generation);
    const auto archive = lookup(name, revision);
    require(archive.has_value(), ErrorCode::Missing, "Unregistered archive version");
    GksSnapshot admitted(directory, archive->root);
    require(admitted.manifest().name == archive->name && admitted.manifest().revision == archive->revision,
            ErrorCode::Integrity, "Mount manifest differs from registered identity");
    std::error_code error;
    const auto absolute = std::filesystem::canonical(directory, error);
    require(!error, ErrorCode::Storage, "Cannot canonicalize mount directory");
    validate_text(absolute.string());
    return bind(alias, *archive, absolute.string(), MountBackend::Local, expected_generation);
}
MountState ArchiveRegistry::mount_remote(std::string_view alias, std::string_view name, std::string_view revision,
                                         MountBackend backend, std::string_view source_key,
                                         std::shared_ptr<BlockSource> source, std::uint64_t expected_generation) {
    label(alias); label(name); label(revision); label(source_key);
    require(backend == MountBackend::RemoteBlocks || backend == MountBackend::RemoteQuery,
            ErrorCode::Schema, "Remote mount requires a remote backend");
    generation(resolve(alias), expected_generation);
    const auto archive = lookup(name, revision);
    require(archive.has_value(), ErrorCode::Missing, "Unregistered archive version");
    // Admission I/O precedes the writer transaction. Root and generation are
    // checked again during bind, so slow remote reads hold no catalogue lock.
    GksSnapshot admitted(std::move(source), archive->root);
    require(admitted.manifest().name == archive->name && admitted.manifest().revision == archive->revision,
            ErrorCode::Integrity, "Mount manifest differs from registered identity");
    return bind(alias, *archive, source_key, backend, expected_generation);
}
MountState ArchiveRegistry::bind(std::string_view alias, const ArchiveVersion& archive, std::string_view location,
                                 MountBackend backend, std::uint64_t expected_generation) {
    auto* database = database_.get();
    Transaction transaction(database); Work work(database, limits_.sql_instructions);
    const auto current = state(database, alias); generation(current, expected_generation);
    require(version(database, archive.name, archive.revision) == archive,
            ErrorCode::Integrity, "Registered archive changed during admission");
    require(count(database, "SELECT count(*) FROM mounts WHERE archive IS NOT NULL")
                + (current.binding ? 0 : 1) <= limits_.active_mounts,
            ErrorCode::Budget, "Active mount quota exceeded");
    require(current.generation || count(database, "SELECT count(*) FROM mounts") < limits_.aliases,
            ErrorCode::Budget, "Mount alias quota exceeded");
    Statement update(database, "INSERT INTO mounts(alias,generation,archive,revision,directory,backend) VALUES(?1,?2,?3,?4,?5,?6)"
        " ON CONFLICT(alias) DO UPDATE SET generation=excluded.generation,archive=excluded.archive,"
        " revision=excluded.revision,directory=excluded.directory,backend=excluded.backend");
    update.text(1, alias); update.integer(2, expected_generation + 1);
    update.text(3, archive.name); update.text(4, archive.revision); update.text(5, location);
    update.integer(6, static_cast<std::uint8_t>(backend)); update.done();
    MountBinding binding{archive, {}};
    binding.backend = backend;
    if (backend == MountBackend::Local) binding.directory = std::string(location);
    else binding.source = location;
    MountState result{std::string(alias), expected_generation + 1, std::move(binding)};
    transaction.commit(work); return result;
}
MountState ArchiveRegistry::unmount(std::string_view alias, std::uint64_t expected_generation) {
    label(alias);
    auto* database = database_.get();
    Transaction transaction(database); Work work(database, limits_.sql_instructions);
    const auto current = state(database, alias); generation(current, expected_generation);
    require(current.binding.has_value(), ErrorCode::Missing, "Alias is not mounted");
    Statement update(database, "UPDATE mounts SET generation=?2,archive=NULL,revision=NULL,directory=NULL,backend=1 WHERE alias=?1");
    update.text(1, alias); update.integer(2, expected_generation + 1); update.done();
    transaction.commit(work);
    return {std::string(alias), expected_generation + 1, {}};
}

} // namespace axiom::knowledge
