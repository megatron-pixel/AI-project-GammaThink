#pragma once

#include "gks.hpp"

namespace axiom::knowledge {

struct SnapshotLimits {
    std::size_t chunk_bytes = 256 * 1024;
    std::size_t chunk_records = 64;
    std::uint64_t objects = 1000000;
    std::uint64_t input_bytes = 1024ULL * 1024 * 1024;
    std::uint64_t output_bytes = 1024ULL * 1024 * 1024;
    std::uint64_t index_entries = 8000000;
    std::uint64_t workspace_bytes = 1024ULL * 1024 * 1024;
    std::uint64_t sql_instructions = 1000000000;
    std::uint32_t cache_kib = 4096;
};
struct SnapshotCompilation {
    BlockId root;
    std::uint64_t objects = 0, input_bytes = 0, output_bytes = 0, index_entries = 0;
};
// Exact uncompressed record contribution to SnapshotCompilation::input_bytes.
std::size_t snapshot_record_size(const KnowledgeRecord& record);

// Destination must not exist. Contains private local data, not a publication.
// Failure may leave unreferenced files, never returns a mountable root.
SnapshotCompilation compile_snapshot(Journal& journal, const std::filesystem::path& destination,
                                      std::string name, std::string revision,
                                      SnapshotLimits limits = {});

// Export only these exact identities, preserving their canonical records and
// observed assessment revisions. Does not infer a dependency closure, grant
// publication permission, or export unrelated private journal records.
SnapshotCompilation compile_selected_snapshot(Journal& journal, std::span<const ObjectId> selection,
                                               const std::filesystem::path& destination,
                                               std::string name, std::string revision,
                                               SnapshotLimits limits = {});

// Owner-thread read-only source. Implementations must bound allocation before
// returning bytes; the snapshot independently rechecks identity and extents.
class BlockSource {
public:
    virtual ~BlockSource() = default;
    virtual Bytes read(const BlockId& id, std::size_t maximum_bytes) = 0;
};

// Caller supplies the expected root identity. The archive view is read-only;
// an injected source may populate its own private cache.
// No imported assessment is made authoritative in a local journal.
class GksSnapshot {
public:
    GksSnapshot(const std::filesystem::path& directory, const BlockId& root);
    GksSnapshot(std::shared_ptr<BlockSource> source, const BlockId& root);
    ~GksSnapshot();
    GksSnapshot(const GksSnapshot&) = delete;
    GksSnapshot& operator=(const GksSnapshot&) = delete;
    const GksManifest& manifest() const { return manifest_; }
    std::optional<KnowledgeRecord> get(const ObjectId& object) const;
    // Missing blocks are Unavailable, malformed blocks are errors, not misses.
    Bytes block(const BlockId& id, std::size_t maximum_bytes = 16 + gks_max_block_bytes + 65536) const;
private:
    std::shared_ptr<BlockSource> source_;
    GksManifest manifest_;
};

} // namespace axiom::knowledge
