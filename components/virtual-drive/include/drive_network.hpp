#pragma once

#include "block_cache.hpp"
#include "gateway.hpp"
#include "remote_query.hpp"

namespace axiom::knowledge {

struct DriveNetworkLimits {
    std::size_t sources = 16;
    BlockCacheLimits cache;
    ReadHttpBudgetLimits network;
};

// Owner-thread, bounded read-only source capabilities for one controller/task.
// Offline initially. Configuration comes from the host, never archive/model
// text. Per-source network permission and query disclosure remain explicit.
// Existing transports share revocation and one non-replenishing HTTP budget;
// source caches receive fixed shares of the declared aggregate cache bounds.
class DriveNetwork {
public:
    explicit DriveNetwork(const std::filesystem::path& cache_directory, DriveNetworkLimits limits = {});
    ~DriveNetwork();
    DriveNetwork(const DriveNetwork&) = delete;
    DriveNetwork& operator=(const DriveNetwork&) = delete;
    // Machine keys use 1..64 lowercase ASCII letters/digits/_/-.
    void add(std::string key, GatewayOptions blocks,
             std::optional<RemoteQueryOptions> queries = {});
    void set_network(bool enabled);
    // May be called from another thread to cancel a live transfer.
    void revoke_network() noexcept;
    bool network_enabled() const noexcept;
    void begin_retrieval(RetrievalLimits limits = {});
    DriveAccess access() const;
    ReadHttpStats network_stats() const;
    BlockCacheStats cache_stats(std::string_view key) const;
    RetrievalStats retrieval_stats(std::string_view key) const;
    void pin(std::string_view key, std::span<const BlockId> ids);
    void unpin(std::string_view key, std::span<const BlockId> ids);
    // Active block capabilities retain only the blocks they actually read.
    // Pruning stays local, bounded and dry-run by default.
    CachePrunePage prune(std::string_view key, std::optional<BlockId> after = {}, CachePruneLimits limits = {});
private:
    struct State;
    std::shared_ptr<State> state_;
};

} // namespace axiom::knowledge
