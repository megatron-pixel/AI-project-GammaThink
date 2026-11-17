#include "gateway.hpp"
#include "block_cid.hpp"

#include <algorithm>

namespace axiom::knowledge {

GatewayBlocks::GatewayBlocks(GatewayOptions options) : http_(options), block_bytes_(options.block_bytes) {
    if (!block_bytes_ || block_bytes_ > maximum_cid_block_bytes)
        throw Error(ErrorCode::Budget, "Invalid gateway block budget");
}
Bytes GatewayBlocks::read(const BlockId& id, std::size_t maximum_bytes) {
    const auto target = "/ipfs/" + cid_text(block_cid(id)) + "?format=raw";
    auto bytes = http_.raw_get(target, std::min(maximum_bytes, block_bytes_));
    verify_block_cid(block_cid(id), bytes);
    return bytes;
}

} // namespace axiom::knowledge
