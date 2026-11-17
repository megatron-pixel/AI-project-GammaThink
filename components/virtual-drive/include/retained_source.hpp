#pragma once

#include "block_cache.hpp"

#include <map>

namespace axiom::knowledge {

// Host-selected view lifetime, using an already-authorized cache capability.
// Leases ONLY blocks actually read: no whole-archive prefetch, inferred network
// permission, catalogue lookup, permanent pin or retrieval-quota reset.
class RetainedBlockSource final : public BlockSource {
public:
    explicit RetainedBlockSource(std::shared_ptr<BlockCache> cache, std::size_t maximum_blocks = 4096);
    Bytes read(const BlockId& id, std::size_t maximum_bytes) override;
private:
    std::shared_ptr<BlockCache> cache_;
    std::size_t maximum_blocks_;
    std::map<BlockId, BlockLease> held_;
};

} // namespace axiom::knowledge
