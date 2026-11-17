#pragma once

#include "gks.hpp"

namespace axiom::knowledge {

// GKS transport profile: CIDv1, raw codec, sha2-256/32-byte digest.
// A distinct type from both a knowledge ObjectId and a stored BlockId.
struct BlockCid {
    Digest digest{};
    auto operator<=>(const BlockCid&) const = default;
};
inline constexpr std::size_t maximum_cid_block_bytes = 16 + gks_max_block_bytes + 65536;

BlockCid block_cid(const BlockId& id);
BlockId block_id(const BlockCid& cid);
Bytes encode_block_cid(const BlockCid& cid);
BlockCid decode_block_cid(std::span<const std::uint8_t> wire);
// Canonical lowercase unpadded multibase base32 only. Not a general CID parser.
std::string cid_text(const BlockCid& cid);
BlockCid parse_block_cid(std::string_view text);
// Checks size and stored-byte hash, not GKS syntax, signatures or truth.
void verify_block_cid(const BlockCid& cid, std::span<const std::uint8_t> bytes);

} // namespace axiom::knowledge
