#include "retained_source.hpp"

namespace axiom::knowledge {

RetainedBlockSource::RetainedBlockSource(std::shared_ptr<BlockCache> cache, std::size_t maximum_blocks)
    : cache_(std::move(cache)), maximum_blocks_(maximum_blocks) {
    if (!cache_) throw Error(ErrorCode::Permission, "Retained source requires a host cache capability");
    if (maximum_blocks == 0 || maximum_blocks > 65536)
        throw Error(ErrorCode::Budget, "Invalid retained-source block limit");
}
Bytes RetainedBlockSource::read(const BlockId& id, std::size_t maximum_bytes) {
    if (held_.contains(id)) return cache_->read(id, maximum_bytes);
    if (held_.size() == maximum_blocks_) throw Error(ErrorCode::Budget, "Retained-source block limit exceeded");
    const std::array ids{id};
    auto lease = cache_->lease(ids, maximum_bytes);
    auto bytes = cache_->read(id, maximum_bytes);
    held_.emplace(id, std::move(lease));
    return bytes;
}

} // namespace axiom::knowledge
