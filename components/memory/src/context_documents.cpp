#include "context_documents.hpp"

#include <algorithm>
#include <sqlite3.h>

namespace axiom::context {
namespace {
using knowledge::Error;
using knowledge::ErrorCode;
constexpr int application_id = 0x47434431;
void require(bool condition, ErrorCode code, const char* message) {
    if (!condition) throw Error(code, message);
}
void sql_check(int code, sqlite3* db) {
    if (code == SQLITE_OK || code == SQLITE_ROW || code == SQLITE_DONE) return;
    const auto basic = code & 0xff;
    throw Error(basic == SQLITE_INTERRUPT ? ErrorCode::Budget :
        basic == SQLITE_CORRUPT || basic == SQLITE_NOTADB ? ErrorCode::Integrity :
        ErrorCode::Storage, db ? sqlite3_errmsg(db) : "Cannot open context documents");
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
    explicit Transaction(sqlite3* value) : db(value) { execute(db, "BEGIN IMMEDIATE"); }
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
    void text(int index, std::string_view text) {
        sql_check(sqlite3_bind_text64(value.get(), index, text.empty() ? "" : text.data(),
            text.size(), SQLITE_TRANSIENT, SQLITE_UTF8), db);
    }
    void integer(int index, std::uint64_t n) {
        require(n <= INT64_MAX, ErrorCode::Schema, "Context document integer overflow");
        sql_check(sqlite3_bind_int64(value.get(), index, static_cast<sqlite3_int64>(n)), db);
    }
    bool row() {
        const auto code = sqlite3_step(value.get());
        sql_check(code, db);
        return code == SQLITE_ROW;
    }
    void done() { require(!row(), ErrorCode::Storage, "Unexpected context document result"); }
    std::uint64_t number(int column) const {
        require(sqlite3_column_type(value.get(), column) == SQLITE_INTEGER &&
                sqlite3_column_int64(value.get(), column) >= 0,
                ErrorCode::Integrity, "Invalid context document integer");
        return static_cast<std::uint64_t>(sqlite3_column_int64(value.get(), column));
    }
    std::string string(int column, std::size_t maximum = 1048576) const {
        require(sqlite3_column_type(value.get(), column) == SQLITE_TEXT,
                ErrorCode::Integrity, "Invalid context document text");
        const auto length = sqlite3_column_bytes(value.get(), column);
        require(length >= 0 && static_cast<std::size_t>(length) <= maximum,
                ErrorCode::Budget, "Context document text budget exceeded");
        const auto* data = sqlite3_column_text(value.get(), column);
        std::string result(reinterpret_cast<const char*>(data), static_cast<std::size_t>(length));
        knowledge::validate_text(result, maximum);
        return result;
    }
};
knowledge::Digest hash(std::string_view content) {
    return knowledge::digest({reinterpret_cast<const std::uint8_t*>(content.data()), content.size()});
}
void field(std::string_view value, std::size_t maximum, bool empty = false) {
    require(empty || !value.empty(), ErrorCode::Schema, "Empty context document field");
    knowledge::validate_text(value, maximum);
}
void scope(std::string_view project, std::string_view session, std::string_view key) {
    field(project, 256);
    field(session, 256, true);
    field(key, 256);
}
constexpr auto columns = "d.project,d.session,d.key,d.uri,d.timestamp,d.kind,d.attribution,"
                         "d.revision,d.active,d.content,d.content_hash";
ContextDocument document(const Statement& query, std::size_t maximum) {
    ContextDocument result;
    result.project = query.string(0, 256);
    result.session = query.string(1, 256);
    result.key = query.string(2, 256);
    result.uri = query.string(3, 4096);
    result.timestamp = query.string(4, 256);
    const auto kind = query.number(5), attribution = query.number(6), active = query.number(8);
    require(kind >= 1 && kind <= 4 && attribution >= 1 && attribution <= 2 && active <= 1,
            ErrorCode::Integrity, "Invalid context document metadata");
    result.kind = static_cast<DocumentKind>(kind);
    result.attribution = static_cast<DocumentAttribution>(attribution);
    result.revision = query.number(7);
    require(result.revision > 0, ErrorCode::Integrity, "Invalid context document revision");
    result.active = active != 0;
    result.content = query.string(9, maximum);
    result.content_hash = hash(result.content);
    require(query.string(10, 64) == knowledge::hex({result.content_hash}), ErrorCode::Integrity,
            "Context document content digest mismatch");
    return result;
}
}

struct ContextDocuments::Impl {
    struct Close { void operator()(sqlite3* db) const noexcept { sqlite3_close(db); } };
    std::unique_ptr<sqlite3, Close> db;
    DocumentLimits limits;
};
ContextDocuments::~ContextDocuments() = default;
ContextDocuments::ContextDocuments(const std::filesystem::path& path, DocumentLimits limits)
    : impl_(std::make_unique<Impl>()) {
    require(limits.source_bytes > 0 && limits.source_bytes <= 1048576 &&
            limits.chunk_bytes >= 4 && limits.chunk_bytes <= 16384 &&
            limits.database.cache_kib > 0 && limits.database.cache_kib <= 65536 &&
            limits.database.sql_instructions > 0, ErrorCode::Budget, "Invalid context document limits");
    impl_->limits = limits;
    sqlite3* raw = nullptr;
    const auto opened = sqlite3_open_v2(path.c_str(), &raw,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX, nullptr);
    impl_->db.reset(raw);
    sql_check(opened, raw);
    auto* db = impl_->db.get();
    Statement identity(db, "PRAGMA application_id");
    require(identity.row(), ErrorCode::Storage, "Missing context document database identity");
    const auto app = identity.number(0);
    identity.done();
    if (app == 0) {
        Statement tables(db, "SELECT count(*) FROM sqlite_schema");
        require(tables.row() && tables.number(0) == 0, ErrorCode::Schema,
                "Not an empty context document database");
    } else require(app == application_id, ErrorCode::Schema, "Not a context document database");
    Statement version(db, "PRAGMA user_version");
    require(version.row(), ErrorCode::Storage, "Missing context document schema version");
    const auto schema = version.number(0);
    version.done();
    require((app == 0 && schema == 0) || (app == application_id && schema == 1),
            ErrorCode::Schema, "Unsupported context document schema");
    execute(db, "PRAGMA foreign_keys=ON");
    execute(db, "PRAGMA journal_mode=WAL");
    execute(db, "PRAGMA synchronous=FULL");
    execute(db, "PRAGMA cache_size=-" + std::to_string(limits.database.cache_kib));
    if (app == 0) {
        Work work(db, limits.database.sql_instructions);
        Transaction transaction(db);
        execute(db,
            "CREATE TABLE documents(id INTEGER PRIMARY KEY,project TEXT NOT NULL,session TEXT NOT NULL,"
            "key TEXT NOT NULL,uri TEXT NOT NULL,timestamp TEXT NOT NULL,kind INTEGER NOT NULL,"
            "attribution INTEGER NOT NULL,revision INTEGER NOT NULL,active INTEGER NOT NULL,"
            "content TEXT NOT NULL,content_hash TEXT NOT NULL,UNIQUE(project,session,key,revision));"
            "CREATE TABLE chunks(id INTEGER PRIMARY KEY,document INTEGER NOT NULL REFERENCES documents(id),"
            "offset INTEGER NOT NULL,content TEXT NOT NULL,content_hash TEXT NOT NULL);"
            "CREATE INDEX document_chunks ON chunks(document,offset);"
            "CREATE VIRTUAL TABLE chunk_fts USING fts5(content,content='chunks',content_rowid='id');"
            "CREATE TRIGGER chunk_insert AFTER INSERT ON chunks BEGIN "
            "INSERT INTO chunk_fts(rowid,content) VALUES(new.id,new.content); END;"
            "CREATE TRIGGER document_no_update BEFORE UPDATE ON documents BEGIN "
            "SELECT RAISE(ABORT,'Context document revisions are immutable'); END;"
            "CREATE TRIGGER document_no_delete BEFORE DELETE ON documents BEGIN "
            "SELECT RAISE(ABORT,'Context document revisions are immutable'); END;"
            "CREATE TRIGGER chunk_no_update BEFORE UPDATE ON chunks BEGIN "
            "SELECT RAISE(ABORT,'Context document chunks are immutable'); END;"
            "CREATE TRIGGER chunk_no_delete BEFORE DELETE ON chunks BEGIN "
            "SELECT RAISE(ABORT,'Context document chunks are immutable'); END;"
            "PRAGMA application_id=" + std::to_string(application_id) + ";PRAGMA user_version=1;");
        transaction.commit(work);
    }
}

ContextDocument ContextDocuments::save(ContextDocument value, std::uint64_t expected) {
    scope(value.project, value.session, value.key);
    field(value.uri, 4096);
    field(value.timestamp, 256);
    field(value.content, impl_->limits.source_bytes);
    require(value.kind >= DocumentKind::Project && value.kind <= DocumentKind::Summary &&
            value.attribution >= DocumentAttribution::Explicit &&
            value.attribution <= DocumentAttribution::Derived && expected < INT64_MAX,
            ErrorCode::Schema, "Invalid context document metadata");
    auto* db = impl_->db.get();
    Work work(db, impl_->limits.database.sql_instructions);
    Transaction transaction(db);
    Statement previous(db, "SELECT coalesce(max(revision),0) FROM documents WHERE project=? AND session=? AND key=?");
    previous.text(1, value.project); previous.text(2, value.session); previous.text(3, value.key);
    require(previous.row() && previous.number(0) == expected, ErrorCode::Integrity,
            "Context document revision changed");
    previous.done();
    value.revision = expected + 1;
    value.content_hash = hash(value.content);
    Statement insert(db, "INSERT INTO documents(project,session,key,uri,timestamp,kind,attribution,"
        "revision,active,content,content_hash) VALUES(?,?,?,?,?,?,?,?,?,?,?)");
    insert.text(1, value.project); insert.text(2, value.session); insert.text(3, value.key);
    insert.text(4, value.uri); insert.text(5, value.timestamp);
    insert.integer(6, static_cast<unsigned>(value.kind));
    insert.integer(7, static_cast<unsigned>(value.attribution));
    insert.integer(8, value.revision); insert.integer(9, value.active);
    insert.text(10, value.content); insert.text(11, knowledge::hex({value.content_hash})); insert.done();
    const auto id = static_cast<std::uint64_t>(sqlite3_last_insert_rowid(db));
    for (std::size_t offset = 0; offset < value.content.size();) {
        auto end = std::min(value.content.size(), offset + impl_->limits.chunk_bytes);
        // Never split a UTF-8 code point. Source text was validated above.
        while (end < value.content.size() &&
               (static_cast<unsigned char>(value.content[end]) & 0xc0U) == 0x80U) --end;
        if (end < value.content.size()) {
            const auto separator = [](unsigned char c) { return c <= 32; };
            if (!separator(value.content[end]) && !separator(value.content[end - 1])) {
                auto word = end;
                while (word > offset && !separator(value.content[word - 1])) --word;
                if (word > offset) end = word;
            }
        }
        const auto part = std::string_view(value.content).substr(offset, end - offset);
        Statement chunk(db, "INSERT INTO chunks(document,offset,content,content_hash) VALUES(?,?,?,?)");
        chunk.integer(1, id); chunk.integer(2, offset); chunk.text(3, part);
        chunk.text(4, knowledge::hex({hash(part)})); chunk.done();
        offset = end;
    }
    transaction.commit(work);
    return value;
}

std::optional<ContextDocument> ContextDocuments::get(std::string_view project, std::string_view session,
    std::string_view key, std::uint64_t revision) {
    scope(project, session, key);
    auto* db = impl_->db.get();
    Work work(db, impl_->limits.database.sql_instructions);
    Statement query(db, std::string("SELECT ") + columns + " FROM documents d WHERE project=? AND session=? "
        "AND key=? AND (?=0 OR revision=?) ORDER BY revision DESC LIMIT 1");
    query.text(1, project); query.text(2, session); query.text(3, key);
    query.integer(4, revision); query.integer(5, revision);
    if (!query.row()) return {};
    return document(query, impl_->limits.source_bytes);
}

DocumentPage ContextDocuments::search(std::string_view project, std::string_view session,
    std::string_view phrase, std::size_t limit, std::size_t bytes) {
    field(project, 256); field(session, 256); field(phrase, 1024);
    require(limit > 0 && limit <= 256 && bytes > 0 && bytes <= 1048576,
            ErrorCode::Budget, "Invalid context document search budget");
    std::string literal = "\"";
    for (const auto c : phrase) { literal += c; if (c == '"') literal += '"'; }
    literal += '"';
    auto* db = impl_->db.get();
    Work work(db, impl_->limits.database.sql_instructions);
    Statement query(db, std::string("SELECT ") + columns + ",c.offset,c.content,c.content_hash "
        "FROM chunk_fts JOIN chunks c ON c.id=chunk_fts.rowid JOIN documents d ON d.id=c.document "
        "WHERE chunk_fts MATCH ? AND d.project=? AND (d.session='' OR d.session=?) AND d.active=1 "
        "AND NOT EXISTS(SELECT 1 FROM documents newer WHERE newer.project=d.project "
        "AND newer.session=d.session AND newer.key=d.key AND newer.revision>d.revision) "
        "ORDER BY d.id DESC,c.offset LIMIT ?");
    query.text(1, literal); query.text(2, project); query.text(3, session); query.integer(4, limit + 1);
    DocumentPage result;
    std::size_t used = 0;
    while (query.row()) {
        const auto length = sqlite3_column_bytes(query.value.get(), 12);
        require(length >= 0, ErrorCode::Integrity, "Invalid context document chunk length");
        if (result.excerpts.size() == limit || static_cast<std::size_t>(length) > bytes - used) {
            result.complete = false;
            break;
        }
        DocumentExcerpt excerpt;
        excerpt.source = document(query, impl_->limits.source_bytes);
        excerpt.offset = query.number(11);
        excerpt.content = query.string(12, 16384);
        excerpt.content_hash = hash(excerpt.content);
        require(query.string(13, 64) == knowledge::hex({excerpt.content_hash}) &&
                excerpt.offset <= excerpt.source.content.size() &&
                excerpt.content.size() <= excerpt.source.content.size() - excerpt.offset &&
                excerpt.source.content.compare(excerpt.offset, excerpt.content.size(), excerpt.content) == 0,
                ErrorCode::Integrity, "Context document chunk identity mismatch");
        excerpt.source_bytes = excerpt.source.content.size();
        excerpt.source.content.clear();
        used += excerpt.content.size();
        result.excerpts.push_back(std::move(excerpt));
    }
    return result;
}

} // namespace axiom::context
