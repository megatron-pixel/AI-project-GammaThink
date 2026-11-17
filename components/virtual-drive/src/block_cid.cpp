#include "block_cid.hpp"

#include <algorithm>

namespace axiom::knowledge {
namespace {
constexpr std::array<std::uint8_t, 4> prefix{0x01, 0x55, 0x12, 0x20};
constexpr std::string_view alphabet = "abcdefghijklmnopqrstuvwxyz234567";
void require(bool condition, ErrorCode code, const char* message) {
    if (!condition) throw Error(code, message);
}
}

BlockCid block_cid(const BlockId& id) { return {id.digest}; }
BlockId block_id(const BlockCid& cid) { return {cid.digest}; }
Bytes encode_block_cid(const BlockCid& cid) {
    Bytes result(prefix.begin(), prefix.end());
    result.insert(result.end(), cid.digest.begin(), cid.digest.end());
    return result;
}
BlockCid decode_block_cid(std::span<const std::uint8_t> wire) {
    // All four permitted varints fit one byte. This exact prefix also rejects
    // non-minimal varints, CIDv0, other codecs/hashes and other digest sizes.
    require(wire.size() == prefix.size() + Digest{}.size()
        && std::equal(prefix.begin(), prefix.end(), wire.begin()),
        ErrorCode::Schema, "Unsupported or malformed GKS block CID");
    BlockCid result;
    std::copy_n(wire.begin() + prefix.size(), result.digest.size(), result.digest.begin());
    return result;
}
std::string cid_text(const BlockCid& cid) {
    std::string result = "b";
    result.reserve(59);
    std::uint32_t buffer = 0;
    unsigned bits = 0;
    for (const auto byte : encode_block_cid(cid)) {
        buffer = (buffer << 8U) | byte;
        bits += 8;
        while (bits >= 5) {
            bits -= 5;
            result.push_back(alphabet[(buffer >> bits) & 31U]);
            buffer &= (1U << bits) - 1U;
        }
    }
    if (bits) result.push_back(alphabet[(buffer << (5U - bits)) & 31U]);
    return result;
}
BlockCid parse_block_cid(std::string_view text) {
    require(text.size() == 59 && text.front() == 'b', ErrorCode::Schema,
            "Expected canonical lowercase base32 GKS block CID");
    Bytes wire;
    wire.reserve(36);
    std::uint32_t buffer = 0;
    unsigned bits = 0;
    for (std::size_t i = 1; i < text.size(); ++i) {
        const auto value = alphabet.find(text[i]);
        require(value != std::string_view::npos, ErrorCode::Schema, "Invalid CID base32 character");
        buffer = (buffer << 5U) | static_cast<std::uint32_t>(value);
        bits += 5;
        if (bits >= 8) {
            bits -= 8;
            wire.push_back(static_cast<std::uint8_t>(buffer >> bits));
            buffer &= (1U << bits) - 1U;
        }
    }
    require(bits == 2 && buffer == 0, ErrorCode::Schema, "Noncanonical CID base32 padding bits");
    return decode_block_cid(wire);
}
void verify_block_cid(const BlockCid& cid, std::span<const std::uint8_t> bytes) {
    require(bytes.size() <= maximum_cid_block_bytes, ErrorCode::Budget, "CID block byte budget exceeded");
    require(digest(bytes) == cid.digest, ErrorCode::Integrity, "CID differs from stored block bytes");
}

} // namespace axiom::knowledge
