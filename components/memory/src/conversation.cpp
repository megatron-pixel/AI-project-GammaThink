#include "conversation.hpp"

#include <algorithm>
#include <sqlite3.h>

namespace axiom::context {
namespace {
using knowledge::Error;
using knowledge::ErrorCode;
constexpr int application_id = 0x47435431;
constexpr std::size_t maximum_content_bytes = 65536;
constexpr std::size_t maximum_page_bytes = 4 * 1024 * 1024;

void require(bool value, ErrorCode code, const char* message) {
    if (!value) throw Error(code, message);
}
void sql_check(int code, sqlite3* db) {
    if (code == SQLITE_OK || code == SQLITE_ROW || code == SQLITE_DONE) return;
    const auto basic = code & 0xff;
    throw Error(basic == SQLITE_INTERRUPT ? ErrorCode::Budget :
                basic == SQLITE_CORRUPT || basic == SQLITE_NOTADB ? ErrorCode::Integrity :
                ErrorCode::Storage, db ? sqlite3_errmsg(db) : "Cannot open conversation database");
}
void execute(sqlite3* db, const std::string& sql) {
    sql_check(sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr), db);
}
struct Work {
    sqlite3* db;
    std::uint64_t remaining;
    Work(sqlite3* value, std::uint64_t limit) : db(value), remaining(limit) {
        sqlite3_progress_handler(db, 1, [](void* p) {
            auto& count = static_cast<Work*>(p)->remaining;
            if (count <= 1) return 1;
            --count;
            return 0;
        }, this);
    }
    void finish() { sqlite3_progress_handler(db, 0, nullptr, nullptr); }
    ~Work() { finish(); }
};
struct Transaction {
    sqlite3* db;
    explicit Transaction(sqlite3* value) : db(value) {
        require(sqlite3_get_autocommit(db), ErrorCode::Schema,
                "Cannot write or nest a conversation snapshot");
        execute(db, "BEGIN IMMEDIATE");
    }
    void commit(Work& work) { work.finish(); execute(db, "COMMIT"); }
    ~Transaction() {
        sqlite3_progress_handler(db, 0, nullptr, nullptr);
        if (!sqlite3_get_autocommit(db)) sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
    }
};
struct Statement {
    sqlite3* db;
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> value{nullptr, sqlite3_finalize};
    Statement(sqlite3* database, const std::string& sql) : db(database) {
        sqlite3_stmt* raw = nullptr;
        const auto code = sqlite3_prepare_v2(db, sql.c_str(), -1, &raw, nullptr);
        value.reset(raw);
        sql_check(code, db);
    }
    void text(int index, std::string_view s) {
        sql_check(sqlite3_bind_text64(value.get(), index, s.empty() ? "" : s.data(),
                                     s.size(), SQLITE_TRANSIENT, SQLITE_UTF8), db);
    }
    void integer(int index, std::uint64_t n) {
        require(n <= INT64_MAX, ErrorCode::Schema, "Conversation integer overflow");
        sql_check(sqlite3_bind_int64(value.get(), index, static_cast<sqlite3_int64>(n)), db);
    }
    bool row() {
        const auto code = sqlite3_step(value.get());
        sql_check(code, db);
        return code == SQLITE_ROW;
    }
    void done() { require(!row(), ErrorCode::Storage, "Unexpected conversation result row"); }
    std::uint64_t number(int column) const {
        require(sqlite3_column_type(value.get(), column) == SQLITE_INTEGER &&
                sqlite3_column_int64(value.get(), column) >= 0, ErrorCode::Integrity,
                "Invalid conversation integer");
        return static_cast<std::uint64_t>(sqlite3_column_int64(value.get(), column));
    }
    std::string string(int column) const {
        require(sqlite3_column_type(value.get(), column) == SQLITE_TEXT,
                ErrorCode::Integrity, "Invalid conversation text");
        const auto length = sqlite3_column_bytes(value.get(), column);
        require(length >= 0 && static_cast<std::size_t>(length) <= maximum_content_bytes,
                ErrorCode::Budget, "Conversation text limit exceeded");
        const auto* p = sqlite3_column_text(value.get(), column);
        std::string result(reinterpret_cast<const char*>(p), static_cast<std::size_t>(length));
        knowledge::validate_text(result);
        return result;
    }
};
void text(std::string_view value, std::size_t maximum = maximum_content_bytes) {
    require(!value.empty(), ErrorCode::Schema, "Empty conversation field");
    knowledge::validate_text(value, maximum);
}
void session_exists(sqlite3* db, std::string_view session) {
    text(session, 256);
    Statement query(db, "SELECT 1 FROM sessions WHERE id=?");
    query.text(1, session);
    require(query.row(), ErrorCode::Missing, "Unknown conversation session");
}
void source_exists(sqlite3* db, std::string_view session, std::uint64_t id, bool user = false) {
    Statement query(db, "SELECT role FROM messages WHERE session=? AND id=?");
    query.text(1, session);
    query.integer(2, id);
    require(query.row(), ErrorCode::Missing, "Missing conversation source message");
    require(!user || query.number(0) == static_cast<unsigned>(Role::User),
            ErrorCode::Schema, "Pinned corrections require a user source");
}
knowledge::Digest content_hash(std::string_view content) {
    return knowledge::digest({reinterpret_cast<const std::uint8_t*>(content.data()), content.size()});
}
Message message(const Statement& query) {
    Message result;
    result.id = query.number(0);
    result.session = query.string(1);
    const auto role = query.number(2);
    require(role >= 1 && role <= 3, ErrorCode::Integrity, "Invalid conversation role");
    result.role = static_cast<Role>(role);
    result.timestamp = query.string(3);
    result.content = query.string(4);
    result.content_hash = content_hash(result.content);
    require(query.string(5) == knowledge::hex({result.content_hash}), ErrorCode::Integrity,
            "Conversation content hash mismatch");
    return result;
}
void page_limits(std::size_t limit, std::size_t bytes) {
    require(limit > 0 && limit <= knowledge::maximum_items &&
            bytes > 0 && bytes <= maximum_page_bytes, ErrorCode::Budget,
            "Conversation page budget exceeded");
}
MessagePage messages(Statement& query, std::size_t limit, std::size_t bytes) {
    MessagePage result;
    std::size_t used = 0;
    while (query.row()) {
        if (result.messages.size() == limit) {
            result.complete = false;
            result.next = result.messages.back().id;
            break;
        }
        // Inspect the byte length before copying the SQLite value.
        const auto size = sqlite3_column_bytes(query.value.get(), 4);
        require(size >= 0 && static_cast<std::size_t>(size) <= bytes - used,
                ErrorCode::Budget, "Conversation content budget exceeded");
        auto item = message(query);
        used += item.content.size();
        result.messages.push_back(std::move(item));
    }
    return result;
}
Constraint read_constraint(const Statement& query) {
    Constraint result;
    result.key = query.string(0);
    result.revision = query.number(1);
    result.source_message = query.number(2);
    result.content = query.string(3);
    const auto active = query.number(4);
    require(active <= 1, ErrorCode::Integrity, "Invalid pinned constraint state");
    result.active = active != 0;
    return result;
}
} // namespace

std::string ConversationMemory::project(std::string_view session) {
    text(session, 256);
    auto* db = database_.get();
    Work work(db, limits_.sql_instructions);
    Statement query(db, "SELECT project FROM sessions WHERE id=?");
    query.text(1, session);
    require(query.row(), ErrorCode::Missing, "Unknown conversation session");
    return query.string(0);
}

void ConversationMemory::Close::operator()(sqlite3* db) const noexcept { sqlite3_close(db); }
ConversationMemory::~ConversationMemory() = default;

ConversationMemory::ConversationMemory(const std::filesystem::path& path, ConversationLimits limits)
    : limits_(limits) {
    require(limits.cache_kib > 0 && limits.cache_kib <= 65536 && limits.sql_instructions > 0,
            ErrorCode::Budget, "Invalid conversation database limits");
    sqlite3* raw = nullptr;
    const auto code = sqlite3_open_v2(path.c_str(), &raw,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX, nullptr);
    database_.reset(raw);
    sql_check(code, raw);
    auto* db = database_.get();
    Statement identity(db, "PRAGMA application_id");
    require(identity.row(), ErrorCode::Storage, "Missing database application identity");
    const auto app = identity.number(0);
    // Refuse unrelated databases before any persistent PRAGMA or schema write.
    if (app == 0) {
        Statement tables(db, "SELECT count(*) FROM sqlite_schema");
        require(tables.row() && tables.number(0) == 0, ErrorCode::Schema,
                "Not an empty conversation database");
    } else {
        require(app == application_id, ErrorCode::Schema, "Not a conversation database");
    }
    identity.done();
    Statement version(db, "PRAGMA user_version");
    require(version.row(), ErrorCode::Storage, "Missing conversation schema version");
    const auto schema = version.number(0);
    version.done();
    require((app == 0 && schema == 0) || (app == application_id && schema == 1),
            ErrorCode::Schema, "Unsupported conversation schema");
    execute(db, "PRAGMA foreign_keys=ON");
    execute(db, "PRAGMA journal_mode=WAL");
    execute(db, "PRAGMA synchronous=FULL");
    execute(db, "PRAGMA cache_size=-" + std::to_string(limits.cache_kib));
    Work work(db, limits.sql_instructions);
    if (app == 0) {
        Transaction transaction(db);
        execute(db,
            "CREATE TABLE sessions(id TEXT PRIMARY KEY,project TEXT NOT NULL);"
            "CREATE TABLE messages(id INTEGER PRIMARY KEY AUTOINCREMENT,session TEXT NOT NULL "
            "REFERENCES sessions(id),role INTEGER NOT NULL CHECK(role BETWEEN 1 AND 3),"
            "timestamp TEXT NOT NULL,content TEXT NOT NULL,content_hash TEXT NOT NULL);"
            "CREATE INDEX session_messages ON messages(session,id);"
            "CREATE VIRTUAL TABLE message_fts USING fts5(content,content='messages',content_rowid='id');"
            "CREATE TRIGGER message_insert AFTER INSERT ON messages BEGIN "
            "INSERT INTO message_fts(rowid,content) VALUES(new.id,new.content); END;"
            "CREATE TRIGGER message_no_update BEFORE UPDATE ON messages BEGIN "
            "SELECT RAISE(ABORT,'Original messages are immutable'); END;"
            "CREATE TRIGGER message_no_delete BEFORE DELETE ON messages BEGIN "
            "SELECT RAISE(ABORT,'Original messages are immutable'); END;"
            "CREATE TABLE summaries(session TEXT NOT NULL REFERENCES sessions(id),"
            "version INTEGER NOT NULL,first_message INTEGER NOT NULL REFERENCES messages(id),"
            "last_message INTEGER NOT NULL REFERENCES messages(id),content TEXT NOT NULL,"
            "PRIMARY KEY(session,version));"
            "CREATE TABLE constraints(session TEXT NOT NULL REFERENCES sessions(id),"
            "key TEXT NOT NULL,revision INTEGER NOT NULL,source_message INTEGER NOT NULL "
            "REFERENCES messages(id),content TEXT NOT NULL,active INTEGER NOT NULL CHECK(active IN(0,1)),"
            "PRIMARY KEY(session,key,revision));"
            "PRAGMA application_id=" + std::to_string(application_id) + ";PRAGMA user_version=1;");
        transaction.commit(work);
    }
}

void ConversationMemory::open_session(std::string_view session, std::string_view project) {
    text(session, 256);
    text(project, 256);
    auto* db = database_.get();
    Work work(db, limits_.sql_instructions);
    Transaction transaction(db);
    Statement insert(db, "INSERT INTO sessions(id,project) VALUES(?,?) ON CONFLICT(id) DO NOTHING");
    insert.text(1, session); insert.text(2, project); insert.done();
    Statement query(db, "SELECT project FROM sessions WHERE id=?");
    query.text(1, session);
    require(query.row() && query.string(0) == project, ErrorCode::Schema,
            "Session project cannot be changed");
    query.done();
    transaction.commit(work);
}
Message ConversationMemory::append(std::string_view session, Role role,
                                   std::string_view timestamp, std::string_view content) {
    text(timestamp, 256); text(content);
    require(role >= Role::User && role <= Role::System, ErrorCode::Schema, "Unknown message role");
    auto* db = database_.get();
    Work work(db, limits_.sql_instructions);
    session_exists(db, session);
    Message result{0, std::string(session), std::string(timestamp), std::string(content),
                   role, content_hash(content)};
    Transaction transaction(db);
    Statement insert(db, "INSERT INTO messages(session,role,timestamp,content,content_hash) "
                         "VALUES(?,?,?,?,?)");
    insert.text(1, session); insert.integer(2, static_cast<unsigned>(role));
    insert.text(3, timestamp); insert.text(4, content);
    insert.text(5, knowledge::hex({result.content_hash})); insert.done();
    result.id = static_cast<std::uint64_t>(sqlite3_last_insert_rowid(db));
    transaction.commit(work);
    return result;
}
Message ConversationMemory::append_response(const Message& original,
    std::span<const Constraint> expected_pins, std::string_view timestamp, std::string_view content) {
    text(timestamp, 256); text(content);
    require(original.role == Role::User && original.id > 0, ErrorCode::Schema,
            "Response source must be an original user message");
    require(expected_pins.size() <= 256, ErrorCode::Budget, "Response pin count exceeded");
    auto* db = database_.get();
    Work work(db, limits_.sql_instructions);
    session_exists(db, original.session);
    Transaction transaction(db);
    Statement latest(db, "SELECT id,session,role,timestamp,content,content_hash FROM messages "
                         "WHERE session=? ORDER BY id DESC LIMIT 1");
    latest.text(1, original.session);
    require(latest.row() && message(latest) == original, ErrorCode::Integrity,
            "Response no longer matches the exact current user turn");
    latest.done();
    Statement pins(db, "SELECT c.key,c.revision,c.source_message,c.content,c.active FROM constraints c "
                      "WHERE session=? AND revision=(SELECT max(revision) FROM constraints x "
                      "WHERE x.session=c.session AND x.key=c.key) AND active=1 ORDER BY c.key LIMIT 257");
    pins.text(1, original.session);
    std::size_t count = 0;
    while (pins.row()) {
        require(count < expected_pins.size() && read_constraint(pins) == expected_pins[count],
                ErrorCode::Integrity, "Active corrections changed before response persistence");
        ++count;
    }
    require(count == expected_pins.size(), ErrorCode::Integrity,
            "Active corrections changed before response persistence");
    Message result{0, original.session, std::string(timestamp), std::string(content),
                   Role::Assistant, content_hash(content)};
    Statement insert(db, "INSERT INTO messages(session,role,timestamp,content,content_hash) VALUES(?,?,?,?,?)");
    insert.text(1, result.session); insert.integer(2, static_cast<unsigned>(result.role));
    insert.text(3, timestamp); insert.text(4, content);
    insert.text(5, knowledge::hex({result.content_hash})); insert.done();
    result.id = static_cast<std::uint64_t>(sqlite3_last_insert_rowid(db));
    transaction.commit(work);
    return result;
}
std::optional<Message> ConversationMemory::get(std::string_view session, std::uint64_t id) {
    auto* db = database_.get();
    Work work(db, limits_.sql_instructions);
    session_exists(db, session);
    Statement query(db, "SELECT id,session,role,timestamp,content,content_hash "
                        "FROM messages WHERE session=? AND id=?");
    query.text(1, session); query.integer(2, id);
    if (!query.row()) return {};
    return message(query);
}
void ConversationMemory::read_snapshot(const std::function<void()>& read) {
    auto* db = database_.get();
    require(sqlite3_get_autocommit(db), ErrorCode::Schema, "Cannot nest a conversation snapshot");
    execute(db, "BEGIN");
    try {
        {
            Work work(db, limits_.sql_instructions);
            Statement pin(db, "SELECT id FROM sessions LIMIT 1");
            if (pin.row()) pin.done();
        }
        read();
        execute(db, "COMMIT");
    } catch (...) {
        sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
        throw;
    }
}
MessagePage ConversationMemory::recent(std::string_view session, std::size_t limit, std::size_t bytes) {
    page_limits(limit, bytes);
    auto* db = database_.get();
    Work work(db, limits_.sql_instructions);
    session_exists(db, session);
    Statement query(db, "SELECT id,session,role,timestamp,content,content_hash FROM messages "
                        "WHERE session=? ORDER BY id DESC LIMIT ?");
    query.text(1, session); query.integer(2, limit + 1);
    auto result = messages(query, limit, bytes);
    std::reverse(result.messages.begin(), result.messages.end());
    // recent() is a window, not a forward search cursor.
    result.next.reset();
    return result;
}
MessagePage ConversationMemory::search(std::string_view session, std::string_view phrase,
                                       std::uint64_t after, std::size_t limit, std::size_t bytes) {
    page_limits(limit, bytes); text(phrase, 4096);
    auto* db = database_.get();
    Work work(db, limits_.sql_instructions);
    session_exists(db, session);
    std::string literal = "\"";
    for (const auto c : phrase) { literal += c; if (c == '"') literal += '"'; }
    literal += '"';
    // Keep indexed phrase matches outermost; a message-first plan repeats the
    // FTS lookup for every unrelated row in a long conversation.
    Statement query(db, "SELECT m.id,m.session,m.role,m.timestamp,m.content,m.content_hash "
                        "FROM message_fts CROSS JOIN messages m ON m.id=message_fts.rowid "
                        "WHERE message_fts MATCH ? AND m.session=? AND message_fts.rowid>? "
                        "ORDER BY message_fts.rowid LIMIT ?");
    query.text(1, literal); query.text(2, session);
    query.integer(3, after); query.integer(4, limit + 1);
    return messages(query, limit, bytes);
}
MessagePage ConversationMemory::messages_after(std::string_view session, std::uint64_t after,
                                              std::size_t limit, std::size_t bytes) {
    page_limits(limit, bytes);
    auto* db = database_.get();
    Work work(db, limits_.sql_instructions);
    session_exists(db, session);
    Statement query(db, "SELECT id,session,role,timestamp,content,content_hash FROM messages "
                        "WHERE session=? AND id>? ORDER BY id LIMIT ?");
    query.text(1, session); query.integer(2, after); query.integer(3, limit + 1);
    return messages(query, limit, bytes);
}
Summary ConversationMemory::save_summary(std::string_view session, Summary value,
                                         std::uint64_t expected_version) {
    text(value.content);
    require(value.version == 0 && value.first_message <= value.last_message,
            ErrorCode::Schema, "Invalid summary source range or caller version");
    auto* db = database_.get();
    Work work(db, limits_.sql_instructions);
    Transaction transaction(db);
    session_exists(db, session);
    source_exists(db, session, value.first_message);
    source_exists(db, session, value.last_message);
    Statement previous(db, "SELECT coalesce(max(version),0) FROM summaries WHERE session=?");
    previous.text(1, session);
    require(previous.row() && previous.number(0) == expected_version, ErrorCode::Integrity,
            "Stale conversation summary version");
    previous.done();
    require(expected_version < INT64_MAX, ErrorCode::Budget, "Summary versions exhausted");
    value.version = expected_version + 1;
    Statement insert(db, "INSERT INTO summaries VALUES(?,?,?,?,?)");
    insert.text(1, session); insert.integer(2, value.version);
    insert.integer(3, value.first_message); insert.integer(4, value.last_message);
    insert.text(5, value.content); insert.done();
    transaction.commit(work);
    return value;
}
std::optional<Summary> ConversationMemory::summary(std::string_view session, std::uint64_t version) {
    auto* db = database_.get();
    Work work(db, limits_.sql_instructions);
    session_exists(db, session);
    Statement query(db, "SELECT version,first_message,last_message,content FROM summaries "
                        "WHERE session=? AND (?=0 OR version=?) ORDER BY version DESC LIMIT 1");
    query.text(1, session); query.integer(2, version); query.integer(3, version);
    if (!query.row()) return {};
    return Summary{query.number(0), query.number(1), query.number(2), query.string(3)};
}
Constraint ConversationMemory::revise_constraint(std::string_view session, Constraint value,
                                                 std::uint64_t expected_revision) {
    text(value.key, 256); text(value.content);
    require(value.revision == 0, ErrorCode::Schema, "Constraint revision is database-owned");
    auto* db = database_.get();
    Work work(db, limits_.sql_instructions);
    Transaction transaction(db);
    session_exists(db, session);
    source_exists(db, session, value.source_message, true);
    Statement previous(db, "SELECT revision,source_message FROM constraints "
                           "WHERE session=? AND key=? ORDER BY revision DESC LIMIT 1");
    previous.text(1, session); previous.text(2, value.key);
    if (previous.row()) {
        require(previous.number(0) == expected_revision &&
                previous.number(1) <= value.source_message, ErrorCode::Integrity,
                "Stale pinned constraint correction");
        previous.done();
    } else {
        require(expected_revision == 0, ErrorCode::Integrity, "Missing previous constraint revision");
    }
    require(expected_revision < INT64_MAX, ErrorCode::Budget, "Constraint revisions exhausted");
    value.revision = expected_revision + 1;
    Statement insert(db, "INSERT INTO constraints VALUES(?,?,?,?,?,?)");
    insert.text(1, session); insert.text(2, value.key); insert.integer(3, value.revision);
    insert.integer(4, value.source_message); insert.text(5, value.content);
    insert.integer(6, value.active); insert.done();
    transaction.commit(work);
    return value;
}
std::vector<Constraint> ConversationMemory::constraints(std::string_view session, std::size_t limit) {
    page_limits(limit, maximum_page_bytes);
    auto* db = database_.get();
    Work work(db, limits_.sql_instructions);
    session_exists(db, session);
    Statement query(db, "SELECT c.key,c.revision,c.source_message,c.content,c.active FROM constraints c "
                        "WHERE session=? AND revision=(SELECT max(revision) FROM constraints x "
                        "WHERE x.session=c.session AND x.key=c.key) AND active=1 ORDER BY c.key LIMIT ?");
    query.text(1, session); query.integer(2, limit + 1);
    std::vector<Constraint> result;
    std::size_t used = 0;
    while (query.row()) {
        require(result.size() < limit, ErrorCode::Budget, "Pinned constraint count exceeded");
        const auto size = sqlite3_column_bytes(query.value.get(), 3);
        require(size >= 0 && static_cast<std::size_t>(size) <= maximum_page_bytes - used,
                ErrorCode::Budget, "Pinned constraint content budget exceeded");
        result.push_back(read_constraint(query));
        used += result.back().content.size();
    }
    return result;
}
std::optional<Constraint> ConversationMemory::constraint(std::string_view session, std::string_view key,
                                                        std::uint64_t revision) {
    text(key, 256);
    auto* db = database_.get();
    Work work(db, limits_.sql_instructions);
    session_exists(db, session);
    Statement query(db, "SELECT key,revision,source_message,content,active FROM constraints "
                        "WHERE session=? AND key=? AND (?=0 OR revision=?) ORDER BY revision DESC LIMIT 1");
    query.text(1, session); query.text(2, key);
    query.integer(3, revision); query.integer(4, revision);
    if (!query.row()) return {};
    return read_constraint(query);
}
std::vector<Constraint> ConversationMemory::constraint_states(std::string_view session, std::size_t limit) {
    page_limits(limit, maximum_page_bytes);
    auto* db = database_.get();
    Work work(db, limits_.sql_instructions);
    session_exists(db, session);
    Statement query(db, "SELECT c.key,c.revision,c.source_message,c.content,c.active FROM constraints c "
                        "WHERE session=? AND revision=(SELECT max(revision) FROM constraints x "
                        "WHERE x.session=c.session AND x.key=c.key) ORDER BY c.key LIMIT ?");
    query.text(1, session); query.integer(2, limit + 1);
    std::vector<Constraint> result;
    std::size_t used = 0;
    while (query.row()) {
        require(result.size() < limit, ErrorCode::Budget, "Constraint state count exceeded");
        const auto size = sqlite3_column_bytes(query.value.get(), 3);
        require(size >= 0 && static_cast<std::size_t>(size) <= maximum_page_bytes - used,
                ErrorCode::Budget, "Constraint state content budget exceeded");
        result.push_back(read_constraint(query));
        used += result.back().content.size();
    }
    return result;
}

} // namespace axiom::context
