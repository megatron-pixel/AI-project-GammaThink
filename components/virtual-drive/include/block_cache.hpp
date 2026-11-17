#pragma once

#include "gks_snapshot.hpp"

#include <chrono>

namespace axiom::knowledge {

struct BlockCacheLimits {
    std::size_t ram_bytes = 64 * 1024 * 1024;
    std::size_t ram_entries = 1024;
    std::uint64_t ssd_bytes = 1024ULL * 1024 * 1024;
    std::size_t ssd_entries = 65536;
    std::size_t lease_references = 65536;
};
struct BlockCacheStats {
    std::size_t ram_bytes = 0, ram_entries = 0;
    std::uint64_t ssd_bytes = 0;
    std::size_t ssd_entries = 0;
    std::uint64_t ram_hits = 0, ssd_hits = 0, source_reads = 0;
    std::size_t leased_blocks = 0, leased_references = 0;
};
struct RetrievalLimits {
    std::uint64_t reads = 1000000, returned_bytes = 256ULL * 1024 * 1024;
    std::uint64_t upstream_reads = 128, upstream_bytes = 64ULL * 1024 * 1024;
    // Cooperative checkpoints; the upstream retains its own I/O timeouts.
    std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(30);
};
struct RetrievalStats {
    // Failed upstream attempts spend their full requested byte allowance.
    std::uint64_t reads = 0, returned_bytes = 0, upstream_reads = 0, upstream_bytes = 0;
};

// Temporary protection, separate from permanent user pins. Release is
// thread-safe; admission and cache I/O retain the cache's owner-thread contract.
// Leases expire on release or cache destruction and are never persisted.
class BlockLease {
public:
    BlockLease() = default;
    ~BlockLease();
    BlockLease(BlockLease&&) noexcept;
    BlockLease& operator=(BlockLease&&) noexcept;
    BlockLease(const BlockLease&) = delete;
    BlockLease& operator=(const BlockLease&) = delete;
    void release() noexcept;
    bool active() const noexcept;
    std::span<const BlockId> blocks() const noexcept { return ids_; }
private:
    friend class BlockCache;
    struct State;
    BlockLease(std::weak_ptr<State> owner, std::vector<BlockId> ids) noexcept;
    std::weak_ptr<State> owner_;
    std::vector<BlockId> ids_;
};
struct CachePruneLimits {
    std::size_t examined = 64, candidates = 32;
    bool apply = false;
};
struct CachePrunePage {
    std::vector<BlockId> evictable;
    std::optional<BlockId> next;
    bool complete = true;
    std::size_t examined = 0, evicted = 0;
    std::uint64_t ssd_bytes = 0;
    std::size_t ram_bytes = 0;
};

// Owner-thread cache of verified stored blocks, not decoded reasoning objects.
// The directory is private, dedicated and exclusively locked. A null upstream
// reads only previously cached data; misses remain Unavailable.
class BlockCache final : public BlockSource {
public:
    BlockCache(const std::filesystem::path& directory,
               std::shared_ptr<BlockSource> upstream = {}, BlockCacheLimits limits = {});
    ~BlockCache();
    BlockCache(const BlockCache&) = delete;
    BlockCache& operator=(const BlockCache&) = delete;
    Bytes read(const BlockId& id, std::size_t maximum_bytes) override;
    BlockCacheStats stats() const;
    void set_offline(bool value);
    // Explicit new session, not a reset of upstream or lifetime cache quotas.
    void begin_retrieval(RetrievalLimits limits = {});
    RetrievalStats retrieval_stats() const;
    // Add/remove explicit stored-block pins atomically. Pin fetches misses;
    // it does not infer a snapshot's dependency closure from its root.
    void pin(std::span<const BlockId> ids);
    void unpin(std::span<const BlockId> ids);
    std::vector<BlockId> pinned() const;
    // Fetches/verifies the complete set into SSD or returns no lease. Admission
    // never promotes these blocks to permanent pins or resets retrieval quotas.
    BlockLease lease(std::span<const BlockId> ids,
                     std::size_t maximum_block_bytes = 16 + gks_max_block_bytes + 65536);
    // Keyset-bounded local cleanup; no upstream reads or retrieval reset.
    // Dry run by default. Permanent pins, pending admission and live leases
    // are protected on every call. CoK objects/snapshots/journals are untouched.
    CachePrunePage prune(std::optional<BlockId> after = {}, CachePruneLimits limits = {});
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace axiom::knowledge
