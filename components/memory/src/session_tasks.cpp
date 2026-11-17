#include "session_tasks.hpp"

#include <algorithm>
#include <sqlite3.h>

namespace axiom::context {
namespace {
using knowledge::Error;
using knowledge::ErrorCode;
constexpr int application_id = 0x47545331;
constexpr std::size_t maximum_certificates = 64;
constexpr const char* action_schema =
    "CREATE TABLE task_actions(session TEXT NOT NULL,revision INTEGER NOT NULL,"
    "action TEXT NOT NULL,receipt TEXT NOT NULL,PRIMARY KEY(session,revision),"
    "FOREIGN KEY(session,revision) REFERENCES task_events(session,revision));"
    "CREATE TRIGGER task_action_no_update BEFORE UPDATE ON task_actions BEGIN "
    "SELECT RAISE(ABORT,'Task action events are immutable'); END;"
    "CREATE TRIGGER task_action_no_delete BEFORE DELETE ON task_actions BEGIN "
    "SELECT RAISE(ABORT,'Task action events are immutable'); END;";
constexpr const char* refinement_schema =
    "CREATE TABLE task_refinements(session TEXT NOT NULL,revision INTEGER NOT NULL,"
    "parent_task TEXT NOT NULL,task TEXT NOT NULL,receipt TEXT NOT NULL,"
    "PRIMARY KEY(session,revision),UNIQUE(session,task),"
    "FOREIGN KEY(session,revision) REFERENCES task_events(session,revision));"
    "CREATE TRIGGER task_refinement_no_update BEFORE UPDATE ON task_refinements BEGIN "
    "SELECT RAISE(ABORT,'Task refinements are immutable'); END;"
    "CREATE TRIGGER task_refinement_no_delete BEFORE DELETE ON task_refinements BEGIN "
    "SELECT RAISE(ABORT,'Task refinements are immutable'); END;";
void require(bool condition, ErrorCode code, const char* message) {
    if (!condition) throw Error(code, message);
}
void sql_check(int code, sqlite3* db) {
    if (code == SQLITE_OK || code == SQLITE_ROW || code == SQLITE_DONE) return;
    const auto basic = code & 0xff;
    throw Error(basic == SQLITE_INTERRUPT ? ErrorCode::Budget :
        basic == SQLITE_CORRUPT || basic == SQLITE_NOTADB ? ErrorCode::Integrity : ErrorCode::Storage,
        db ? sqlite3_errmsg(db) : "Cannot open session task database");
}
void execute(sqlite3* db, const std::string& sql) {
    sql_check(sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr), db);
}
struct Work {
    sqlite3* db;
    std::uint64_t remaining;
    Work(sqlite3* database, std::uint64_t budget) : db(database), remaining(budget) {
        sqlite3_progress_handler(db, 1, [](void* pointer) {
            auto& count = static_cast<Work*>(pointer)->remaining;
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
    explicit Transaction(sqlite3* database) : db(database) { execute(db, "BEGIN IMMEDIATE"); }
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
    void text(int index, std::string_view input) {
        sql_check(sqlite3_bind_text64(value.get(), index, input.data(), input.size(),
                                     SQLITE_TRANSIENT, SQLITE_UTF8), db);
    }
    void number(int index, std::uint64_t input) {
        require(input <= INT64_MAX, ErrorCode::Schema, "Session task integer overflow");
        sql_check(sqlite3_bind_int64(value.get(), index, static_cast<sqlite3_int64>(input)), db);
    }
    bool row() {
        const auto code = sqlite3_step(value.get());
        sql_check(code, db);
        return code == SQLITE_ROW;
    }
    void done() { require(!row(), ErrorCode::Storage, "Unexpected session task row"); }
    std::uint64_t number(int column) const {
        require(sqlite3_column_type(value.get(), column) == SQLITE_INTEGER &&
            sqlite3_column_int64(value.get(), column) >= 0, ErrorCode::Integrity,
            "Invalid session task integer");
        return static_cast<std::uint64_t>(sqlite3_column_int64(value.get(), column));
    }
    std::string text(int column) const {
        require(sqlite3_column_type(value.get(), column) == SQLITE_TEXT,
                ErrorCode::Integrity, "Invalid session task text");
        const auto bytes = sqlite3_column_bytes(value.get(), column);
        require(bytes > 0 && bytes <= 256, ErrorCode::Integrity, "Invalid session task text size");
        const auto* raw = sqlite3_column_text(value.get(), column);
        std::string result(reinterpret_cast<const char*>(raw), static_cast<std::size_t>(bytes));
        knowledge::validate_text(result, 256);
        return result;
    }
};
void session_name(std::string_view session) {
    require(!session.empty(), ErrorCode::Schema, "Empty task session");
    knowledge::validate_text(session, 256);
}
constexpr const char* columns = "session,revision,user_message,content_hash,task_id,phase,certificates";
SessionTask read_task(const Statement& query) {
    SessionTask result;
    result.session = query.text(0);
    result.revision = query.number(1);
    result.user_message = query.number(2);
    result.message_hash = knowledge::parse_id(query.text(3)).digest;
    result.ordered_task = knowledge::parse_id(query.text(4));
    const auto phase = query.number(5);
    require(result.revision > 0 && result.user_message > 0 &&
        phase >= static_cast<unsigned>(TaskPhase::Interpreted) &&
        phase <= static_cast<unsigned>(TaskPhase::Conflict), ErrorCode::Integrity, "Invalid task state");
    result.phase = static_cast<TaskPhase>(phase);
    require(sqlite3_column_type(query.value.get(), 6) == SQLITE_BLOB,
            ErrorCode::Integrity, "Invalid task certificate references");
    const auto count = sqlite3_column_bytes(query.value.get(), 6);
    require(count >= 0 && static_cast<std::size_t>(count) <= maximum_certificates * 32 &&
        count % 32 == 0, ErrorCode::Integrity, "Invalid task certificate extent");
    const auto* data = static_cast<const std::uint8_t*>(sqlite3_column_blob(query.value.get(), 6));
    for (int offset = 0; offset < count; offset += 32) {
        knowledge::ObjectId id;
        std::copy_n(data + offset, 32, id.digest.begin());
        result.certificates.push_back(id);
    }
    return result;
}
std::optional<SessionTask> latest(sqlite3* db, std::string_view session) {
    Statement query(db, std::string("SELECT ") + columns +
        " FROM task_events WHERE session=? ORDER BY revision DESC LIMIT 1");
    query.text(1, session);
    if (!query.row()) return std::nullopt;
    return read_task(query);
}
void insert(sqlite3* db, const SessionTask& event) {
    require(event.certificates.size() <= maximum_certificates, ErrorCode::Budget,
            "Too many task certificate references");
    Statement write(db, "INSERT INTO task_events VALUES(?,?,?,?,?,?,?)");
    write.text(1, event.session); write.number(2, event.revision); write.number(3, event.user_message);
    write.text(4, knowledge::hex({event.message_hash})); write.text(5, knowledge::hex(event.ordered_task));
    write.number(6, static_cast<unsigned>(event.phase));
    knowledge::Bytes certificates;
    for (const auto& id : event.certificates) certificates.insert(certificates.end(), id.digest.begin(), id.digest.end());
    if (certificates.empty()) sql_check(sqlite3_bind_zeroblob(write.value.get(), 7, 0), db);
    else sql_check(sqlite3_bind_blob(write.value.get(), 7, certificates.data(),
        static_cast<int>(certificates.size()), SQLITE_TRANSIENT), db);
    write.done();
}
bool allows(TaskPhase from, TaskPhase to) {
    switch (from) {
    case TaskPhase::Interpreted:
        return to == TaskPhase::Reasoning || to == TaskPhase::Failed || to == TaskPhase::Cancelled;
    case TaskPhase::Reasoning:
        return to == TaskPhase::Checked || to == TaskPhase::Conflict || to == TaskPhase::Unresolved ||
               to == TaskPhase::Failed || to == TaskPhase::Cancelled;
    case TaskPhase::Unresolved:
        return to == TaskPhase::Reasoning || to == TaskPhase::Cancelled;
    case TaskPhase::Checked: case TaskPhase::Failed: case TaskPhase::Cancelled: case TaskPhase::Conflict:
        return false;
    }
    return false;
}
}

void SessionTasks::Close::operator()(sqlite3* db) const noexcept { sqlite3_close(db); }
SessionTasks::~SessionTasks() = default;
SessionTasks::SessionTasks(const std::filesystem::path& path, SessionTaskLimits limits) : limits_(limits) {
    require(limits.cache_kib > 0 && limits.cache_kib <= 65536 && limits.sql_instructions > 0,
            ErrorCode::Budget, "Invalid session task limits");
    sqlite3* raw = nullptr;
    const auto code = sqlite3_open_v2(path.c_str(), &raw,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX, nullptr);
    database_.reset(raw);
    sql_check(code, raw);
    Statement identity(raw, "PRAGMA application_id");
    require(identity.row(), ErrorCode::Storage, "Missing task database identity");
    const auto app = identity.number(0);
    identity.done();
    if (app == 0) {
        Statement schema(raw, "SELECT count(*) FROM sqlite_schema");
        require(schema.row() && schema.number(0) == 0, ErrorCode::Schema, "Not an empty task database");
    } else require(app == application_id, ErrorCode::Schema, "Not a session task database");
    Statement version(raw, "PRAGMA user_version");
    require(version.row(), ErrorCode::Storage, "Missing task schema version");
    const auto schema = version.number(0);
    version.done();
    require((app == 0 && schema == 0) || (app == application_id && (schema >= 1 && schema <= 3)),
            ErrorCode::Schema, "Unsupported session task schema");
    execute(raw, "PRAGMA journal_mode=WAL");
    execute(raw, "PRAGMA synchronous=FULL");
    execute(raw, "PRAGMA foreign_keys=ON");
    execute(raw, "PRAGMA cache_size=-" + std::to_string(limits.cache_kib));
    Work work(raw, limits.sql_instructions);
    if (app == 0) {
        Transaction transaction(raw);
        execute(raw,
            "CREATE TABLE task_events(session TEXT NOT NULL,revision INTEGER NOT NULL CHECK(revision>0),"
            "user_message INTEGER NOT NULL CHECK(user_message>0),content_hash TEXT NOT NULL,"
            "task_id TEXT NOT NULL,phase INTEGER NOT NULL CHECK(phase BETWEEN 1 AND 7),"
            "certificates BLOB NOT NULL,PRIMARY KEY(session,revision));"
            "CREATE TRIGGER task_no_update BEFORE UPDATE ON task_events BEGIN "
            "SELECT RAISE(ABORT,'Task events are immutable'); END;"
            "CREATE TRIGGER task_no_delete BEFORE DELETE ON task_events BEGIN "
            "SELECT RAISE(ABORT,'Task events are immutable'); END;"
            "PRAGMA application_id=" + std::to_string(application_id) + ";");
        execute(raw, action_schema);
        execute(raw, refinement_schema);
        execute(raw, "PRAGMA user_version=3;");
        transaction.commit(work);
    } else if (schema == 1) {
        Transaction transaction(raw);
        execute(raw, action_schema);
        execute(raw, refinement_schema);
        execute(raw, "PRAGMA user_version=3;");
        transaction.commit(work);
    } else if (schema == 2) {
        Transaction transaction(raw);
        execute(raw, refinement_schema);
        execute(raw, "PRAGMA user_version=3;");
        transaction.commit(work);
    }
    path_ = path == ":memory:" ? path : std::filesystem::canonical(path);
}
std::optional<SessionTask> SessionTasks::active(std::string_view session) const {
    session_name(session);
    Work work(database_.get(), limits_.sql_instructions);
    return latest(database_.get(), session);
}
TaskHistory SessionTasks::history(std::string_view session, std::uint64_t after, std::size_t limit) const {
    session_name(session);
    require(limit > 0 && limit <= 1024, ErrorCode::Budget, "Session task page limit exceeded");
    Work work(database_.get(), limits_.sql_instructions);
    Statement query(database_.get(), std::string("SELECT ") + columns +
        " FROM task_events WHERE session=? AND revision>? ORDER BY revision LIMIT ?");
    query.text(1, session); query.number(2, after); query.number(3, limit + 1);
    TaskHistory result;
    while (query.row()) {
        if (result.events.size() == limit) { result.complete = false; break; }
        result.events.push_back(read_task(query));
        result.next = result.events.back().revision;
    }
    return result;
}
std::vector<CompletedTaskAction> SessionTasks::completed_actions(std::string_view session,
    const knowledge::ObjectId& task, std::uint64_t through_revision, std::size_t limit) const {
    session_name(session);
    require(limit > 0 && limit <= 128, ErrorCode::Budget, "Task action history bound exceeded");
    Work work(database_.get(), limits_.sql_instructions);
    Statement query(database_.get(), "SELECT a.revision,a.action,a.receipt FROM task_actions a "
        "JOIN task_events e ON e.session=a.session AND e.revision=a.revision "
        "WHERE a.session=? AND e.task_id=? AND a.revision<=? ORDER BY a.revision LIMIT ?");
    query.text(1, session); query.text(2, knowledge::hex(task));
    query.number(3, through_revision); query.number(4, limit + 1);
    std::vector<CompletedTaskAction> result;
    while (query.row()) {
        require(result.size() < limit, ErrorCode::Budget, "Task action history bound exceeded");
        result.push_back({query.number(0), query.text(1), knowledge::parse_id(query.text(2))});
    }
    return result;
}
SessionTask SessionTasks::begin(ConversationMemory& memory, std::string_view session,
    std::uint64_t user_message, const knowledge::ObjectId& ordered_task, std::uint64_t expected_revision) {
    session_name(session);
    const auto source = memory.get(session, user_message);
    require(source && source->role == Role::User, ErrorCode::Schema, "Task source must be an original user message");
    auto* db = database_.get();
    Work work(db, limits_.sql_instructions);
    Transaction transaction(db);
    const auto previous = latest(db, session);
    require((previous ? previous->revision : 0) == expected_revision,
            ErrorCode::Schema, "Session task revision changed");
    require(!previous || user_message > previous->user_message,
            ErrorCode::Schema, "New tasks require a later original user request");
    require(expected_revision < INT64_MAX, ErrorCode::Budget, "Session task revisions exhausted");
    SessionTask event{std::string(session), expected_revision + 1, user_message,
        source->content_hash, ordered_task, TaskPhase::Interpreted, {}};
    insert(db, event);
    transaction.commit(work);
    return event;
}
std::optional<TaskRefinement> SessionTasks::refinement(std::string_view session,
    const knowledge::ObjectId& task) const {
    session_name(session);
    auto* db = database_.get();
    Work work(db, limits_.sql_instructions);
    Statement query(db, "SELECT revision,parent_task,receipt FROM task_refinements WHERE session=? AND task=?");
    query.text(1, session); query.text(2, knowledge::hex(task));
    if (!query.row()) return {};
    TaskRefinement result{static_cast<std::uint64_t>(query.number(0)), knowledge::parse_id(query.text(1)),
        task, knowledge::parse_id(query.text(2))};
    require(!query.row(), ErrorCode::Integrity, "Ambiguous task refinement history");
    return result;
}
SessionTask SessionTasks::refine(ConversationMemory& memory, std::string_view session,
    const knowledge::ObjectId& task, const knowledge::ObjectId& receipt, std::uint64_t expected_revision) {
    session_name(session);
    auto* db = database_.get();
    Work work(db, limits_.sql_instructions);
    Transaction transaction(db);
    const auto previous = latest(db, session);
    require(previous && previous->revision == expected_revision, ErrorCode::Schema, "Session task revision changed");
    require(previous->phase != TaskPhase::Checked && previous->phase != TaskPhase::Cancelled,
        ErrorCode::Schema, "Completed or cancelled tasks cannot be refined");
    require(task != previous->ordered_task, ErrorCode::Schema, "Refinement must change the captured plan");
    const auto source = memory.get(session, previous->user_message);
    const auto current = memory.recent(session, 1);
    require(source && source->role == Role::User && source->content_hash == previous->message_hash &&
        !current.messages.empty() && current.messages.back().id == source->id,
        ErrorCode::Schema, "Refinement requires the unchanged current original user request");
    require(expected_revision < INT64_MAX, ErrorCode::Budget, "Session task revisions exhausted");
    SessionTask event{std::string(session), expected_revision + 1, previous->user_message,
        previous->message_hash, task, TaskPhase::Interpreted, {}};
    insert(db, event);
    Statement write(db, "INSERT INTO task_refinements VALUES(?,?,?,?,?)");
    write.text(1, session); write.number(2, event.revision);
    write.text(3, knowledge::hex(previous->ordered_task)); write.text(4, knowledge::hex(task));
    write.text(5, knowledge::hex(receipt)); write.done();
    transaction.commit(work);
    return event;
}
SessionTask SessionTasks::transition(std::string_view session, std::uint64_t expected_revision,
    TaskPhase phase, const std::vector<knowledge::ObjectId>& certificates,
    const std::optional<CompletedTaskAction>& action) {
    session_name(session);
    auto* db = database_.get();
    Work work(db, limits_.sql_instructions);
    Transaction transaction(db);
    auto previous = latest(db, session);
    require(previous && previous->revision == expected_revision,
            ErrorCode::Schema, "Session task revision changed");
    require(allows(previous->phase, phase), ErrorCode::Schema, "Invalid session task transition");
    require((phase != TaskPhase::Checked && phase != TaskPhase::Conflict) || !certificates.empty() ||
        (phase == TaskPhase::Checked && action.has_value()), ErrorCode::Schema,
        "Checked task metadata requires native certificates or an operational completion marker");
    require((phase != TaskPhase::Reasoning && phase != TaskPhase::Cancelled && phase != TaskPhase::Failed) ||
        certificates.empty(), ErrorCode::Schema, "Non-result task state cannot carry certificates");
    if (action) {
        require(previous->phase == TaskPhase::Reasoning &&
            (phase == TaskPhase::Checked || phase == TaskPhase::Conflict || phase == TaskPhase::Unresolved),
            ErrorCode::Schema, "Task action marker requires a completed operation");
        require(action->revision == expected_revision + 1 && !action->action.empty() && action->action.size() <= 64,
            ErrorCode::Schema, "Invalid task action completion marker");
        knowledge::validate_text(action->action, 64);
    }
    require(expected_revision < INT64_MAX, ErrorCode::Budget, "Session task revisions exhausted");
    ++previous->revision;
    previous->phase = phase;
    previous->certificates = certificates;
    insert(db, *previous);
    if (action) {
        Statement write(db, "INSERT INTO task_actions VALUES(?,?,?,?)");
        write.text(1, session); write.number(2, previous->revision);
        write.text(3, action->action); write.text(4, knowledge::hex(action->receipt));
        write.done();
    }
    transaction.commit(work);
    return *previous;
}

} // namespace axiom::context
