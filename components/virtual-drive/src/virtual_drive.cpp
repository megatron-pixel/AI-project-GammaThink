#include "virtual_drive.hpp"
#include "remote_query.hpp"

#include <algorithm>
#include <functional>
#include <set>
#include <sqlite3.h>

namespace axiom::knowledge {
namespace {
void require(bool value, ErrorCode code, const char* message) {
    if (!value) throw Error(code, message);
}
void bounds(const IndexQueryLimits& limits) {
    require(limits.nodes > 0 && limits.nodes <= 4096 && limits.entries > 0 && limits.entries <= 1000000
        && limits.stored_bytes > 0 && limits.stored_bytes <= 64 * 1024 * 1024
        && limits.work > 0 && limits.work <= 100000000 && limits.phrase_tokens > 0 && limits.phrase_tokens <= 256,
        ErrorCode::Budget, "Invalid indexed query limits");
}
Bytes bytes(std::string_view value) {
    if (value.empty()) return {};
    return {reinterpret_cast<const std::uint8_t*>(value.data()),
            reinterpret_cast<const std::uint8_t*>(value.data()) + value.size()};
}
void text(std::span<const std::uint8_t> value) {
    if (!value.empty()) validate_text({reinterpret_cast<const char*>(value.data()), value.size()});
}
void key_valid(IndexKind kind, const Bytes& key) {
    switch (kind) {
    case IndexKind::Objects:
        require(key.empty(), ErrorCode::Integrity, "Invalid identity index key"); break;
    case IndexKind::Kind:
        require(key.size() == 1 && key[0] >= 1 && key[0] <= 12, ErrorCode::Integrity, "Invalid kind index key"); break;
    case IndexKind::Concept: text(key); break;
    case IndexKind::Source: {
        const auto separator = std::find(key.begin(), key.end(), 0);
        require(separator != key.end(), ErrorCode::Integrity, "Missing source index delimiter");
        const auto size = static_cast<std::size_t>(separator - key.begin());
        text(std::span(key).first(size)); text(std::span(key).subspan(size + 1)); break;
    }
    case IndexKind::Text:
        require(key.size() > 5 && key[key.size() - 5] == 0, ErrorCode::Integrity, "Invalid token index key");
        text(std::span(key).first(key.size() - 5)); break;
    case IndexKind::Relationship:
        require(key.size() == 33 && key.back() >= 1 && key.back() <= 8,
                ErrorCode::Integrity, "Invalid relationship index key"); break;
    case IndexKind::Prerequisite:
        require(key.size() == 32, ErrorCode::Integrity, "Invalid prerequisite index key"); break;
    }
}
struct Band {
    Bytes key;
    bool exact = true;
    std::optional<ObjectId> after;
    bool contains(const Bytes& value) const {
        return exact ? value == key : value.size() >= key.size() && std::equal(key.begin(), key.end(), value.begin());
    }
    bool overlaps(const IndexChild& child) const {
        if (child.last.key < key) return false;
        if (exact) {
            if (child.first.key > key) return false;
            if (after && child.first.key == key && child.last.key == key && child.last.object <= *after) return false;
        } else {
            // Prefix bands here always end with NUL (source or token delimiter).
            auto upper = key; ++upper.back();
            if (child.first.key >= upper) return false;
        }
        return true;
    }
};
struct Tree {
    const GksSnapshot& snapshot;
    IndexQueryLimits limits;
    std::size_t nodes = 0, entries = 0, stored_bytes = 0;
    std::uint64_t remaining;
    Tree(const GksSnapshot& source, IndexQueryLimits budget) : snapshot(source), limits(budget), remaining(budget.work) {}
    void tick() {
        require(remaining > 0, ErrorCode::Budget, "Indexed query work exhausted"); --remaining;
    }
    IndexChunk page(const IndexRoot& root, const BlockId& id, std::uint32_t level,
                    const std::optional<IndexChild>& expected) {
        tick();
        require(nodes < limits.nodes, ErrorCode::Budget, "Indexed query node budget exceeded"); ++nodes;
        const auto wire = snapshot.block(id, limits.stored_bytes - stored_bytes);
        stored_bytes += wire.size();
        auto result = decode_index_chunk(id, wire);
        require(result.level == level && result.key_format == root.key_format,
                ErrorCode::Integrity, "Indexed query child profile or depth differs");
        const auto size = level ? result.children.size() : result.entries.size();
        require(size <= limits.entries - entries, ErrorCode::Budget, "Indexed query entry budget exceeded");
        entries += size;
        if (level) {
            for (const auto& child : result.children) {
                key_valid(root.kind, child.first.key); key_valid(root.kind, child.last.key);
            }
        } else {
            for (const auto& entry : result.entries) key_valid(root.kind, entry.position.key);
        }
        const auto& first = level ? result.children.front().first : result.entries.front().position;
        const auto& last = level ? result.children.back().last : result.entries.back().position;
        require(!expected || (first == expected->first && last == expected->last),
                ErrorCode::Integrity, "Indexed query child range differs");
        return result;
    }
    bool scan(const IndexRoot& root, const BlockId& id, std::uint32_t level, const Band& band,
              const std::function<bool(const IndexEntry&)>& visit, std::optional<IndexChild> expected = {}) {
        const auto current = page(root, id, level, expected);
        if (level) {
            for (const auto& child : current.children) {
                tick();
                if (band.overlaps(child) && !scan(root, child.block, level - 1, band, visit, child)) return false;
            }
        } else {
            for (const auto& entry : current.entries) {
                tick();
                if (band.contains(entry.position.key) && (!band.after || entry.position.object > *band.after)
                    && !visit(entry)) return false;
            }
        }
        return true;
    }
    std::optional<IndexEntry> find(const IndexRoot& root, const IndexPosition& position) {
        auto id = root.block;
        auto level = root.level;
        std::optional<IndexChild> expected;
        for (;;) {
            const auto current = page(root, id, level, expected);
            if (!level) {
                const auto entry = std::lower_bound(current.entries.begin(), current.entries.end(), position,
                    [](const auto& a, const auto& b) { return a.position < b; });
                return entry != current.entries.end() && entry->position == position
                    ? std::optional<IndexEntry>(*entry) : std::nullopt;
            }
            const auto child = std::find_if(current.children.begin(), current.children.end(),
                [&](const auto& value) { return value.first <= position && position <= value.last; });
            if (child == current.children.end()) return {};
            expected = *child; id = child->block; --level;
        }
    }
};
void sql_check(int code, sqlite3* db) {
    if (code == SQLITE_OK || code == SQLITE_DONE || code == SQLITE_ROW) return;
    throw Error((code & 0xff) == SQLITE_INTERRUPT ? ErrorCode::Budget : ErrorCode::Storage, sqlite3_errmsg(db));
}
std::vector<Bytes> tokenize(std::string_view phrase, Tree& tree) {
    sqlite3* raw = nullptr;
    const auto opened = sqlite3_open(":memory:", &raw);
    std::unique_ptr<sqlite3, decltype(&sqlite3_close_v2)> db(raw, sqlite3_close_v2);
    sql_check(opened, raw);
    sqlite3_progress_handler(raw, 1, [](void* data) -> int {
        auto& left = static_cast<Tree*>(data)->remaining;
        if (!left) return 1;
        --left; return 0;
    }, &tree);
    sql_check(sqlite3_exec(raw, "PRAGMA cache_size=-64;"
        "CREATE VIRTUAL TABLE input USING fts5(content,tokenize='unicode61');"
        "CREATE VIRTUAL TABLE terms USING fts5vocab(input,'instance');", nullptr, nullptr, nullptr), raw);
    sqlite3_stmt* statement = nullptr;
    auto code = sqlite3_prepare_v2(raw, "INSERT INTO input(rowid,content) VALUES(1,?1)", -1, &statement, nullptr);
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> insert(statement, sqlite3_finalize);
    sql_check(code, raw);
    sql_check(sqlite3_bind_text64(statement, 1, phrase.data(), phrase.size(), SQLITE_TRANSIENT, SQLITE_UTF8), raw);
    sql_check(sqlite3_step(statement), raw);
    statement = nullptr;
    code = sqlite3_prepare_v2(raw, "SELECT term,offset FROM terms WHERE doc=1 ORDER BY offset", -1, &statement, nullptr);
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> query(statement, sqlite3_finalize);
    sql_check(code, raw);
    std::vector<Bytes> result;
    while ((code = sqlite3_step(statement)) == SQLITE_ROW) {
        require(result.size() < tree.limits.phrase_tokens, ErrorCode::Budget, "Search phrase token budget exceeded");
        require(sqlite3_column_int64(statement, 1) == static_cast<sqlite3_int64>(result.size()),
                ErrorCode::Integrity, "Unexpected unicode61 token position");
        const auto size = sqlite3_column_bytes(statement, 0);
        const auto* data = static_cast<const std::uint8_t*>(sqlite3_column_blob(statement, 0));
        require(size > 0 && size <= static_cast<int>(maximum_text_bytes), ErrorCode::Budget, "Search token exceeds bounds");
        result.emplace_back(data, data + size);
    }
    sql_check(code, raw);
    return result;
}
Bytes term_key(const Bytes& term, std::uint32_t position) {
    auto key = term; key.push_back(0);
    for (unsigned byte = 0; byte < 4; ++byte) key.push_back(position >> (8 * byte));
    return key;
}
}

GksArchive::GksArchive(const std::filesystem::path& directory, const BlockId& root, IndexQueryLimits limits)
    : snapshot_(directory, root), limits_(limits) { bounds(limits_); }
GksArchive::GksArchive(std::shared_ptr<BlockSource> source, const BlockId& root, IndexQueryLimits limits)
    : snapshot_(std::move(source), root), limits_(limits) { bounds(limits_); }
std::optional<KnowledgeRecord> GksArchive::get(const ObjectId& id) { return snapshot_.get(id); }
ObjectPage GksArchive::query(const KnowledgeQuery& query, std::optional<ObjectId> after, std::size_t limit) {
    validate_knowledge_query(query, limit);
    Tree tree(snapshot_, limits_);
    Band band{{}, true, after};
    IndexKind kind = IndexKind::Objects;
    std::vector<Bytes> terms;
    switch (query.kind) {
    case QueryKind::All: break;
    case QueryKind::Kind:
        kind = IndexKind::Kind; band.key = {static_cast<std::uint8_t>(query.object_kind)}; break;
    case QueryKind::Concept:
        kind = IndexKind::Concept; band.key = bytes(query.value); break;
    case QueryKind::Source:
        kind = IndexKind::Source; band.key = bytes(query.value); band.key.push_back(0);
        band.exact = query.source_version.has_value();
        if (query.source_version) {
            const auto revision = bytes(*query.source_version);
            band.key.insert(band.key.end(), revision.begin(), revision.end());
        }
        break;
    case QueryKind::Text:
        kind = IndexKind::Text; terms = tokenize(query.value, tree);
        if (terms.empty()) return {};
        band.key = terms.front(); band.key.push_back(0); band.exact = false; break;
    case QueryKind::Related:
        kind = IndexKind::Relationship; band.key.assign(query.target.digest.begin(), query.target.digest.end());
        band.key.push_back(static_cast<std::uint8_t>(query.relationship)); break;
    case QueryKind::RequiredBy:
        kind = IndexKind::Prerequisite; band.key.assign(query.target.digest.begin(), query.target.digest.end()); break;
    }
    const auto& indexes = snapshot_.manifest().indexes;
    const auto root = std::find_if(indexes.begin(), indexes.end(), [&](const auto& value) { return value.kind == kind; });
    if (root == indexes.end()) return {};
    require(root->key_format == gks_index_formats[static_cast<std::uint8_t>(kind) - 1],
            ErrorCode::Schema, "Unsupported snapshot query index profile");
    std::set<ObjectId> identities;
    tree.scan(*root, root->block, root->level, band, [&](const IndexEntry& entry) {
        if (identities.contains(entry.position.object)) return true;
        if (terms.size() > 1) {
            const auto& key = entry.position.key;
            std::uint32_t position = 0;
            for (unsigned i = 0; i < 4; ++i) position |= std::uint32_t(key[key.size() - 4 + i]) << (8 * i);
            for (std::size_t i = 1; i < terms.size(); ++i) {
                tree.tick();
                if (i > UINT32_MAX - position) return true;
                const auto next = tree.find(*root, {term_key(terms[i], position + i), entry.position.object});
                if (!next) return true;
                require(next->block == entry.block && next->ordinal == entry.ordinal,
                        ErrorCode::Integrity, "Token index object locations differ");
            }
        }
        identities.insert(entry.position.object);
        if (band.exact) return identities.size() <= limit;
        if (identities.size() > limit + 1) identities.erase(std::prev(identities.end()));
        return true;
    });
    ObjectPage result;
    result.complete = identities.size() <= limit;
    for (const auto& id : identities) {
        if (result.objects.size() == limit) break;
        result.objects.push_back(id);
    }
    if (!result.complete) result.next = result.objects.back();
    return result;
}

DriveView::DriveView(Journal& journal, std::vector<MountState> mounts, IndexQueryLimits limits, DriveAccess access)
    : mounts_(std::move(mounts)), journal_(journal) {
    bounds(limits);
    require(mounts_.size() < 16, ErrorCode::Budget, "Drive mounts exceed provider budget including journal");
    std::set<std::string> aliases;
    std::vector<KnowledgeArchive*> providers{&journal_};
    for (const auto& mount : mounts_) {
        validate_text(mount.alias, 128);
        require(!mount.alias.empty() && mount.binding.has_value() && mount.generation > 0 && aliases.insert(mount.alias).second,
                ErrorCode::Schema, "Invalid drive mount state");
        const auto& binding = *mount.binding;
        const auto matches = [&](const GksManifest& manifest) {
            require(manifest.name == binding.archive.name && manifest.revision == binding.archive.revision,
                    ErrorCode::Integrity, "Drive manifest differs from mount metadata");
        };
        if (binding.backend == MountBackend::Local) {
            require(binding.source.empty(), ErrorCode::Schema, "Local mount contains a remote source");
            auto archive = std::make_unique<GksArchive>(binding.directory, binding.archive.root, limits);
            matches(archive->manifest()); archives_.push_back(std::move(archive));
        } else {
            require(binding.backend == MountBackend::RemoteBlocks || binding.backend == MountBackend::RemoteQuery,
                    ErrorCode::Schema, "Unknown drive backend");
            validate_text(binding.source, 128);
            require(binding.directory.empty() && !binding.source.empty(), ErrorCode::Schema, "Invalid remote binding");
            require(access.blocks && (binding.backend != MountBackend::RemoteQuery || access.queries),
                    ErrorCode::Permission, "Remote mount has no runtime authorization");
            auto source = access.blocks(binding.source);
            require(static_cast<bool>(source), ErrorCode::Permission, "Remote source key is not authorized");
            if (binding.backend == MountBackend::RemoteBlocks) {
                auto archive = std::make_unique<GksArchive>(std::move(source), binding.archive.root, limits);
                matches(archive->manifest()); archives_.push_back(std::move(archive));
            } else {
                auto archive = std::make_unique<RemoteQueryArchive>(
                    std::move(source), binding.archive.root, access.queries(binding.source), limits);
                matches(archive->manifest()); archives_.push_back(std::move(archive));
            }
        }
        providers.push_back(archives_.back().get());
    }
    knowledge_ = std::make_unique<UnifiedKnowledge>(std::move(providers));
}
std::unique_ptr<DriveView> VirtualDrive::open_view(std::span<const MountState> accepted_mounts) {
    auto mounts = registry_.mounts();
    require(accepted_mounts.size() < 16 && mounts.size() + accepted_mounts.size() < 16,
        ErrorCode::Budget, "Combined drive mounts exceed provider budget including journal");
    mounts.insert(mounts.end(), accepted_mounts.begin(), accepted_mounts.end());
    return std::make_unique<DriveView>(journal_, std::move(mounts), limits_, access_);
}

} // namespace axiom::knowledge
