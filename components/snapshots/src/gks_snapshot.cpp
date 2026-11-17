#include "gks_snapshot.hpp"

#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <sqlite3.h>
#include <sys/stat.h>
#include <unistd.h>

namespace axiom::knowledge {
namespace {
constexpr std::size_t stored_limit = 16 + gks_max_block_bytes + 65536;
void require(bool value, ErrorCode code, const char* message) {
    if (!value) throw Error(code, message);
}
struct Descriptor {
    int value;
    explicit Descriptor(int fd) : value(fd) {}
    ~Descriptor() { if (value >= 0) close(value); }
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
};
int open_directory(const std::filesystem::path& path) {
    validate_text(path.string());
    const auto fd = open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    require(fd >= 0, ErrorCode::Storage, "Cannot open GKS block directory");
    return fd;
}
Bytes read_block(int directory, const BlockId& id, std::size_t maximum_bytes = stored_limit) {
    const auto name = hex(id) + ".gksb";
    Descriptor file(openat(directory, name.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK));
    require(file.value >= 0, errno == ENOENT ? ErrorCode::Unavailable : ErrorCode::Storage,
            "Cannot read GKS block");
    struct stat status{};
    require(fstat(file.value, &status) == 0 && S_ISREG(status.st_mode),
            ErrorCode::Storage, "GKS block is not a regular file");
    require(status.st_size >= 16 && static_cast<std::uint64_t>(status.st_size) <= stored_limit,
            ErrorCode::Budget, "GKS stored file exceeds bounds");
    require(static_cast<std::uint64_t>(status.st_size) <= maximum_bytes,
            ErrorCode::Budget, "GKS block read budget exceeded");
    Bytes bytes(static_cast<std::size_t>(status.st_size));
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto size = read(file.value, bytes.data() + offset, bytes.size() - offset);
        require(size > 0, ErrorCode::Storage, "Incomplete GKS block read");
        offset += static_cast<std::size_t>(size);
    }
    std::uint8_t extra;
    require(read(file.value, &extra, 1) == 0, ErrorCode::Integrity, "GKS file changed during read");
    require(identify_block(bytes) == id, ErrorCode::Integrity, "GKS file hash mismatch");
    return bytes;
}
class LocalBlocks final : public BlockSource {
public:
    explicit LocalBlocks(const std::filesystem::path& path) : directory_(open_directory(path)) {}
    Bytes read(const BlockId& id, std::size_t maximum_bytes) override {
        return read_block(directory_.value, id, maximum_bytes);
    }
private:
    Descriptor directory_;
};
BlockId put_block(int directory, const Bytes& wire, SnapshotCompilation& stats, const SnapshotLimits& limits) {
    const auto id = identify_block(wire);
    const auto name = hex(id) + ".gksb";
    Descriptor file(openat(directory, name.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
    if (file.value < 0 && errno == EEXIST) {
        require(read_block(directory, id) == wire, ErrorCode::Integrity, "Existing GKS bytes differ");
        return id;
    }
    require(file.value >= 0, ErrorCode::Storage, "Cannot create GKS block");
    require(wire.size() <= limits.output_bytes - stats.output_bytes, ErrorCode::Budget, "GKS output budget exceeded");
    std::size_t offset = 0;
    while (offset < wire.size()) {
        const auto size = write(file.value, wire.data() + offset, wire.size() - offset);
        require(size > 0, ErrorCode::Storage, "Incomplete GKS block write");
        offset += static_cast<std::size_t>(size);
    }
    require(fchmod(file.value, 0400) == 0 && fsync(file.value) == 0,
            ErrorCode::Storage, "Cannot finalize GKS block");
    stats.output_bytes += wire.size();
    return id;
}
void sql_check(int code, sqlite3* db) {
    if (code == SQLITE_OK || code == SQLITE_ROW || code == SQLITE_DONE) return;
    const auto basic = code & 0xff;
    throw Error(basic == SQLITE_FULL || basic == SQLITE_TOOBIG || basic == SQLITE_INTERRUPT
                    ? ErrorCode::Budget : ErrorCode::Storage, sqlite3_errmsg(db));
}
void sql(sqlite3* db, const std::string& text) {
    sql_check(sqlite3_exec(db, text.c_str(), nullptr, nullptr, nullptr), db);
}
struct Statement {
    sqlite3* db;
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> value{nullptr, sqlite3_finalize};
    Statement(sqlite3* database, const std::string& text) : db(database) {
        sqlite3_stmt* raw = nullptr;
        const auto code = sqlite3_prepare_v2(db, text.c_str(), -1, &raw, nullptr);
        value.reset(raw); sql_check(code, db);
    }
    void integer(int slot, std::uint64_t data) {
        require(data <= INT64_MAX, ErrorCode::Budget, "GKS SQLite integer exceeds bounds");
        sql_check(sqlite3_bind_int64(value.get(), slot, data), db);
    }
    void blob(int slot, std::span<const std::uint8_t> bytes) {
        sql_check(bytes.empty() ? sqlite3_bind_zeroblob(value.get(), slot, 0)
            : sqlite3_bind_blob64(value.get(), slot, bytes.data(), bytes.size(), SQLITE_TRANSIENT), db);
    }
    bool row() {
        const auto code = sqlite3_step(value.get());
        sql_check(code, db); return code == SQLITE_ROW;
    }
    void done() {
        require(!row(), ErrorCode::Storage, "Unexpected GKS workspace result");
        sql_check(sqlite3_reset(value.get()), db);
        sql_check(sqlite3_clear_bindings(value.get()), db);
    }
    Bytes bytes(int column) const {
        const auto size = sqlite3_column_bytes(value.get(), column);
        const auto* data = static_cast<const std::uint8_t*>(sqlite3_column_blob(value.get(), column));
        return size ? Bytes(data, data + size) : Bytes{};
    }
    Digest hash(int column) const {
        const auto data = bytes(column);
        require(data.size() == Digest{}.size(), ErrorCode::Integrity, "Invalid GKS workspace hash");
        Digest result; std::copy(data.begin(), data.end(), result.begin()); return result;
    }
    std::uint64_t number(int column) const {
        const auto result = sqlite3_column_int64(value.get(), column);
        require(result >= 0, ErrorCode::Integrity, "Negative GKS workspace integer");
        return static_cast<std::uint64_t>(result);
    }
};
struct Scratch {
    std::filesystem::path path;
    std::unique_ptr<sqlite3, decltype(&sqlite3_close_v2)> db{nullptr, sqlite3_close_v2};
    std::uint64_t remaining;
    Scratch(const std::filesystem::path& directory, const SnapshotLimits& limits)
        : path(directory / "index-work.sqlite"), remaining(limits.sql_instructions) {
        sqlite3* raw = nullptr;
        const auto code = sqlite3_open_v2(path.c_str(), &raw, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
        db.reset(raw); sql_check(code, raw);
        sqlite3_progress_handler(raw, 1, [](void* context) -> int {
            auto& left = static_cast<Scratch*>(context)->remaining;
            if (left <= 1) { left = 0; return 1; }
            --left; return 0;
        }, this);
        sql(raw, "PRAGMA page_size=4096");
        sql(raw, "PRAGMA max_page_count=" + std::to_string(limits.workspace_bytes / 4096));
        sql(raw, "PRAGMA cache_size=-" + std::to_string(limits.cache_kib));
        sql(raw, "PRAGMA temp_store=FILE");
        sql(raw, "PRAGMA journal_mode=OFF");
        sql(raw, "PRAGMA synchronous=OFF");
        sql(raw, R"SQL(
CREATE TABLE entries(kind INTEGER,key BLOB,object BLOB,block BLOB,ordinal INTEGER,
 PRIMARY KEY(kind,key,object)) WITHOUT ROWID;
CREATE TABLE nodes0(kind INTEGER,level INTEGER,firstkey BLOB,firstobject BLOB,lastkey BLOB,lastobject BLOB,block BLOB,
 PRIMARY KEY(kind,level,firstkey,firstobject)) WITHOUT ROWID;
CREATE TABLE nodes1(kind INTEGER,level INTEGER,firstkey BLOB,firstobject BLOB,lastkey BLOB,lastobject BLOB,block BLOB,
 PRIMARY KEY(kind,level,firstkey,firstobject)) WITHOUT ROWID;
CREATE VIRTUAL TABLE token_input USING fts5(content,tokenize='unicode61');
CREATE VIRTUAL TABLE token_terms USING fts5vocab(token_input,'instance');
)SQL");
    }
    ~Scratch() {
        if (db) sqlite3_progress_handler(db.get(), 0, nullptr, nullptr);
        db.reset();
        std::error_code error;
        std::filesystem::remove(path, error);
    }
};
Bytes key(std::string_view text) {
    if (text.empty()) return {};
    return {reinterpret_cast<const std::uint8_t*>(text.data()),
            reinterpret_cast<const std::uint8_t*>(text.data()) + text.size()};
}
struct Builder {
    int directory;
    const SnapshotLimits& limits;
    SnapshotCompilation stats;
    Scratch scratch;
    Statement insert;
    Builder(int fd, const std::filesystem::path& path, const SnapshotLimits& budget)
        : directory(fd), limits(budget), scratch(path, budget),
          insert(scratch.db.get(), "INSERT OR IGNORE INTO entries VALUES(?1,?2,?3,?4,?5)") {}
    void posting(IndexKind kind, const Bytes& bytes, const ObjectId& object, const BlockId& block,
                 std::uint32_t ordinal) {
        require(stats.index_entries < limits.index_entries, ErrorCode::Budget, "GKS index entry budget exceeded");
        require(bytes.size() <= gks_max_key_bytes, ErrorCode::Budget, "GKS index key exceeds bounds");
        ++stats.index_entries; // Charge attempts too; duplicates cannot evade work budgets.
        insert.integer(1, static_cast<std::uint8_t>(kind)); insert.blob(2, bytes);
        insert.blob(3, object.digest); insert.blob(4, block.digest); insert.integer(5, ordinal); insert.done();
    }
    void flush(ObjectChunk& chunk) {
        if (chunk.records.empty()) return;
        const auto block = put_block(directory, encode_object_chunk(chunk), stats, limits);
        for (std::size_t i = 0; i < chunk.records.size(); ++i) {
            const auto& object = chunk.records[i].object;
            const auto id = identify(object);
            posting(IndexKind::Objects, {}, id, block, i);
            posting(IndexKind::Kind, {static_cast<std::uint8_t>(object.kind)}, id, block, i);
            for (const auto& tag : object.concepts) posting(IndexKind::Concept, key(tag), id, block, i);
            for (const auto& source : object.sources) {
                auto bytes = key(source.archive_id); bytes.push_back(0);
                const auto version = key(source.archive_version);
                bytes.insert(bytes.end(), version.begin(), version.end());
                posting(IndexKind::Source, bytes, id, block, i);
            }
            for (const auto& link : relationships(object)) {
                Bytes bytes(link.target.digest.begin(), link.target.digest.end());
                bytes.push_back(static_cast<std::uint8_t>(link.relationship));
                posting(IndexKind::Relationship, bytes, id, block, i);
            }
            for (const auto& dependency : prerequisites(object)) {
                posting(IndexKind::Prerequisite, Bytes(dependency.digest.begin(), dependency.digest.end()), id, block, i);
            }
            auto* db = scratch.db.get();
            sql(db, "DELETE FROM token_input");
            Statement text(db, "INSERT INTO token_input(rowid,content) VALUES(1,?1)");
            const auto indexed = indexed_text(object);
            sql_check(sqlite3_bind_text64(text.value.get(), 1, indexed.data(), indexed.size(), SQLITE_TRANSIENT, SQLITE_UTF8), db);
            text.done();
            Statement terms(db, "SELECT term,offset FROM token_terms WHERE doc=1");
            while (terms.row()) {
                auto bytes = terms.bytes(0); bytes.push_back(0);
                const auto position = terms.number(1);
                require(position <= UINT32_MAX, ErrorCode::Budget, "GKS token position exceeds bounds");
                for (unsigned byte = 0; byte < 4; ++byte) bytes.push_back(position >> (8 * byte));
                posting(IndexKind::Text, bytes, id, block, i);
            }
        }
        chunk.records.clear();
    }
    BlockId node(IndexKind kind, const IndexChunk& chunk) {
        const auto id = put_block(directory, encode_index_chunk(chunk), stats, limits);
        const auto& first = chunk.level ? chunk.children.front().first : chunk.entries.front().position;
        const auto& last = chunk.level ? chunk.children.back().last : chunk.entries.back().position;
        Statement save(scratch.db.get(), "INSERT INTO nodes" + std::to_string(chunk.level % 2)
            + " VALUES(?1,?2,?3,?4,?5,?6,?7)");
        save.integer(1, static_cast<std::uint8_t>(kind)); save.integer(2, chunk.level);
        save.blob(3, first.key); save.blob(4, first.object.digest);
        save.blob(5, last.key); save.blob(6, last.object.digest); save.blob(7, id.digest); save.done();
        return id;
    }
    std::optional<IndexRoot> build_index(IndexKind kind) {
        const auto kind_value = static_cast<std::uint8_t>(kind);
        const auto format = std::string(gks_index_formats[kind_value - 1]);
        const auto base_size = 12 + format.size();
        Statement rows(scratch.db.get(), "SELECT key,object,block,ordinal FROM entries WHERE kind="
            + std::to_string(kind_value) + " ORDER BY key,object");
        IndexChunk page{format, 0, {}, {}};
        auto bytes = base_size;
        std::size_t count = 0;
        BlockId root;
        while (rows.row()) {
            IndexEntry entry{{rows.bytes(0), ObjectId{rows.hash(1)}}, BlockId{rows.hash(2)},
                             static_cast<std::uint32_t>(rows.number(3))};
            const auto size = 72 + entry.position.key.size();
            if (!page.entries.empty() && (page.entries.size() >= limits.chunk_records
                    || bytes + size > limits.chunk_bytes)) {
                root = node(kind, page); ++count; page.entries.clear(); bytes = base_size;
            }
            bytes += size; page.entries.push_back(std::move(entry));
        }
        if (!page.entries.empty()) { root = node(kind, page); ++count; }
        if (!count) return {};
        std::uint32_t level = 0;
        while (count > 1) {
            require(level < 16, ErrorCode::Budget, "GKS tree depth exceeded");
            Statement children(scratch.db.get(),
                "SELECT firstkey,firstobject,lastkey,lastobject,block FROM nodes"
                + std::to_string(level % 2) + " WHERE kind="
                + std::to_string(kind_value) + " AND level=" + std::to_string(level)
                + " ORDER BY firstkey,firstobject");
            ++level;
            // Never mutate the table an active frontier cursor is reading.
            sql(scratch.db.get(), "DELETE FROM nodes" + std::to_string(level % 2)
                + " WHERE kind=" + std::to_string(kind_value));
            page = {format, level, {}, {}};
            count = 0; bytes = base_size;
            while (children.row()) {
                IndexChild child{{children.bytes(0), ObjectId{children.hash(1)}},
                    {children.bytes(2), ObjectId{children.hash(3)}}, BlockId{children.hash(4)}};
                const auto size = 104 + child.first.key.size() + child.last.key.size();
                // At least two children must fit a branch, even above the soft target.
                if (page.children.size() >= 2 && (page.children.size() >= limits.chunk_records
                        || bytes + size > limits.chunk_bytes)) {
                    root = node(kind, page); ++count; page.children.clear(); bytes = base_size;
                }
                bytes += size; page.children.push_back(std::move(child));
            }
            if (!page.children.empty()) { root = node(kind, page); ++count; }
        }
        return IndexRoot{kind, format, level, root};
    }
};
}

std::size_t snapshot_record_size(const KnowledgeRecord& record) {
    auto size = 37 + encode_object(record.object).size();
    if (record.assessment) {
        const auto& a = *record.assessment;
        size += 8 + 3 + 4 + a.checker.size() + 4 + a.reason.size() + 1
            + (a.certificate ? 32 : 0) + 4 + 32 * a.checked_dependencies.size();
    }
    return size;
}

static SnapshotCompilation compile_snapshot_subset(Journal& journal, const std::filesystem::path& destination,
                                      std::string name, std::string revision, SnapshotLimits limits,
                                      std::optional<std::vector<ObjectId>> selection) {
    validate_text(destination.string()); validate_text(name, 128); validate_text(revision, 128);
    require(!name.empty() && !revision.empty(), ErrorCode::Schema, "Missing GKS name or revision");
    require(limits.chunk_bytes >= 1024 && limits.chunk_bytes <= gks_max_block_bytes
        && limits.chunk_records >= 2 && limits.chunk_records <= maximum_items
        && limits.workspace_bytes >= 65536 && limits.workspace_bytes <= (1ULL << 40)
        && limits.sql_instructions > 0 && limits.cache_kib >= 64 && limits.cache_kib <= 65536,
        ErrorCode::Budget, "Invalid snapshot compilation limits");
    require(mkdir(destination.c_str(), 0700) == 0, ErrorCode::Permission, "GKS destination must be new");
    Descriptor directory(open_directory(destination));
    Builder builder(directory.value, destination, limits);
    journal.read_snapshot([&] {
        ObjectChunk chunk;
        std::size_t bytes = 4;
        std::optional<ObjectId> after;
        std::size_t position = 0;
        for (;;) {
            ObjectPage page;
            if (selection) {
                const auto end = position + std::min(limits.chunk_records, selection->size() - position);
                page.objects.assign(selection->begin() + position, selection->begin() + end);
                position = end;
                page.complete = position == selection->size();
                if (!page.complete) page.next = page.objects.back();
            } else page = journal.objects(after, limits.chunk_records);
            for (const auto& id : page.objects) {
                require(builder.stats.objects < limits.objects, ErrorCode::Budget, "GKS object budget exceeded");
                auto object = journal.get(id);
                require(object.has_value(), selection ? ErrorCode::Missing : ErrorCode::Integrity,
                        "Snapshot object is unavailable in the journal");
                KnowledgeRecord record{std::move(*object), journal.assessment(id)};
                const auto size = snapshot_record_size(record);
                require(size <= gks_max_block_bytes - 4 && size <= limits.input_bytes - builder.stats.input_bytes,
                        ErrorCode::Budget, "GKS input byte budget exceeded");
                if (!chunk.records.empty() && (bytes + size > limits.chunk_bytes
                        || chunk.records.size() >= limits.chunk_records)) {
                    builder.flush(chunk); bytes = 4;
                }
                bytes += size; builder.stats.input_bytes += size; ++builder.stats.objects;
                chunk.records.push_back(std::move(record));
            }
            if (page.complete) break;
            after = page.next;
        }
        builder.flush(chunk);
    });
    GksManifest manifest{std::move(name), std::move(revision), schema_version, builder.stats.objects, {}, {}};
    for (unsigned kind = 1; kind <= gks_index_formats.size(); ++kind) {
        if (auto root = builder.build_index(static_cast<IndexKind>(kind))) manifest.indexes.push_back(*root);
    }
    builder.stats.root = put_block(directory.value, encode_gks_manifest(manifest), builder.stats, limits);
    require(fsync(directory.value) == 0, ErrorCode::Storage, "Cannot finalize GKS directory");
    return builder.stats;
}

SnapshotCompilation compile_snapshot(Journal& journal, const std::filesystem::path& destination,
                                      std::string name, std::string revision, SnapshotLimits limits) {
    return compile_snapshot_subset(journal, destination, std::move(name), std::move(revision), limits, {});
}

SnapshotCompilation compile_selected_snapshot(Journal& journal, std::span<const ObjectId> selection,
                                               const std::filesystem::path& destination,
                                               std::string name, std::string revision, SnapshotLimits limits) {
    require(selection.size() <= limits.objects && selection.size() <= 1000000,
            ErrorCode::Budget, "Snapshot selection exceeds object budget");
    std::vector<ObjectId> selected(selection.begin(), selection.end());
    std::sort(selected.begin(), selected.end());
    require(std::adjacent_find(selected.begin(), selected.end()) == selected.end(),
            ErrorCode::Schema, "Duplicate snapshot selection");
    return compile_snapshot_subset(journal, destination, std::move(name), std::move(revision),
                                   limits, std::move(selected));
}

GksSnapshot::GksSnapshot(const std::filesystem::path& directory, const BlockId& root)
    : GksSnapshot(std::make_shared<LocalBlocks>(directory), root) {}
GksSnapshot::GksSnapshot(std::shared_ptr<BlockSource> source, const BlockId& root)
    : source_(std::move(source)), manifest_(decode_gks_manifest(root, block(root))) {}
GksSnapshot::~GksSnapshot() = default;
Bytes GksSnapshot::block(const BlockId& id, std::size_t maximum_bytes) const {
    require(static_cast<bool>(source_), ErrorCode::Schema, "Missing GKS block source");
    const auto bytes = source_->read(id, std::min(maximum_bytes, stored_limit));
    require(bytes.size() <= maximum_bytes && bytes.size() <= stored_limit,
            ErrorCode::Budget, "GKS source exceeded read bounds");
    require(identify_block(bytes) == id, ErrorCode::Integrity, "GKS source hash mismatch");
    return bytes;
}
std::optional<KnowledgeRecord> GksSnapshot::get(const ObjectId& object) const {
    if (!manifest_.object_count) return {};
    const auto& root = manifest_.indexes.front();
    require(root.key_format == gks_index_formats[0], ErrorCode::Schema, "Unsupported GKS object index profile");
    auto id = root.block;
    auto level = root.level;
    std::optional<IndexChild> expected;
    const IndexPosition position{{}, object};
    for (;;) {
        const auto page = decode_index_chunk(id, block(id));
        require(page.level == level && page.key_format == root.key_format,
                ErrorCode::Integrity, "GKS child level or profile differs");
        const auto& first = level ? page.children.front().first : page.entries.front().position;
        const auto& last = level ? page.children.back().last : page.entries.back().position;
        require(first.key.empty() && last.key.empty() && (!expected
            || (first == expected->first && last == expected->last)),
            ErrorCode::Integrity, "GKS child bounds differ");
        if (!level) {
            for (const auto& entry : page.entries) {
                require(entry.position.key.empty(), ErrorCode::Integrity, "Invalid GKS identity key");
            }
            const auto entry = std::lower_bound(page.entries.begin(), page.entries.end(), position,
                [](const auto& a, const auto& b) { return a.position < b; });
            if (entry == page.entries.end() || entry->position != position) return {};
            auto chunk = decode_object_chunk(entry->block, block(entry->block));
            require(entry->ordinal < chunk.records.size()
                && identify(chunk.records[entry->ordinal].object) == object,
                ErrorCode::Integrity, "GKS object location differs");
            return std::move(chunk.records[entry->ordinal]);
        }
        const auto child = std::find_if(page.children.begin(), page.children.end(),
            [&](const auto& value) { return value.first <= position && position <= value.last; });
        if (child == page.children.end()) return {};
        expected = *child; id = child->block; --level;
    }
}

} // namespace axiom::knowledge
