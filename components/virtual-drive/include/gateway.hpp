#pragma once

#include "gks_snapshot.hpp"
#include "read_http.hpp"

namespace axiom::knowledge {

struct GatewayOptions : ReadHttpOptions {
    // Public IPFS ecosystems commonly limit blocks to 2 MiB. Larger local
    // GKS blocks require an explicitly configured compatible private gateway.
    std::size_t block_bytes = 2 * 1024 * 1024;
};
using GatewayStats = ReadHttpStats;
class GatewayBlocks final : public BlockSource {
public:
    explicit GatewayBlocks(GatewayOptions options);
    Bytes read(const BlockId& id, std::size_t maximum_bytes) override;
    const GatewayStats& stats() const { return http_.stats(); }
private:
    ReadHttp http_;
    std::size_t block_bytes_;
};

} // namespace axiom::knowledge
