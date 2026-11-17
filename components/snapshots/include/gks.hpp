#pragma once

#include "retrieval.hpp"

namespace axiom::knowledge {

// Storage block identity: neither a knowledge ObjectId nor an IPFS CID.
struct BlockId {
    Digest digest{};
    auto operator<=>(const BlockId&) const = default;
};
inline constexpr std::uint16_t gks_version = 1;
inline constexpr std::size_t gks_max_block_bytes = 8 * 1024 * 1024;
inline constexpr std::size_t gks_max_key_bytes = 3 * maximum_text_bytes + 32;

struct GksLimits {
    std::size_t block_bytes = gks_max_block_bytes;
    std::size_t records = maximum_items;
    std::size_t key_bytes = gks_max_key_bytes;
    std::size_t index_depth = 16;
};
struct ObjectChunk {
    // Strictly increasing ObjectIds. Canonical GKOB bytes remain unchanged.
    // Stored assessments are observations, not locally authoritative imports.
    std::vector<KnowledgeRecord> records;
};
struct IndexPosition {
    Bytes key;
    ObjectId object;
    auto operator<=>(const IndexPosition&) const = default;
};
struct IndexEntry {
    IndexPosition position;
    BlockId block;
    std::uint32_t ordinal = 0;
    bool operator==(const IndexEntry&) const = default;
};
struct IndexChild {
    IndexPosition first, last;
    BlockId block;
    bool operator==(const IndexChild&) const = default;
};
struct IndexChunk {
    std::string key_format;
    // Zero: leaf entries. Positive: non-overlapping inclusive child ranges.
    std::uint32_t level = 0;
    std::vector<IndexEntry> entries;
    std::vector<IndexChild> children;
    bool operator==(const IndexChunk&) const = default;
};
enum class IndexKind : std::uint8_t {
    Objects = 1, Kind, Concept, Source, Text, Relationship, Prerequisite
};
inline constexpr std::array<std::string_view, 7> gks_index_formats{
    "object-id-v1", "object-kind-v1", "concept-utf8-v1", "source-nul-version-v1",
    "unicode61-position-v1", "target-relationship-v1", "prerequisite-id-v1"};
struct IndexRoot {
    IndexKind kind = IndexKind::Objects;
    std::string key_format;
    std::uint32_t level = 0;
    BlockId block;
    bool operator==(const IndexRoot&) const = default;
};
struct GksManifest {
    std::string name, revision;
    std::uint16_t object_schema = schema_version;
    std::uint64_t object_count = 0;
    // Sorted unique kinds. An empty snapshot has no index roots.
    std::vector<IndexRoot> indexes;
    std::vector<BlockId> parents;
    bool operator==(const GksManifest&) const = default;
};

// SHA256 over the stored envelope/compressed bytes; does not decode them.
BlockId identify_block(std::span<const std::uint8_t> wire);
std::string hex(const BlockId& id);
BlockId parse_block_id(std::string_view value);
Bytes encode_object_chunk(const ObjectChunk& chunk, GksLimits limits = {});
ObjectChunk decode_object_chunk(const BlockId& expected, std::span<const std::uint8_t> wire,
                                GksLimits limits = {});
Bytes encode_index_chunk(const IndexChunk& chunk, GksLimits limits = {});
IndexChunk decode_index_chunk(const BlockId& expected, std::span<const std::uint8_t> wire,
                               GksLimits limits = {});
Bytes encode_gks_manifest(const GksManifest& manifest, GksLimits limits = {});
GksManifest decode_gks_manifest(const BlockId& expected, std::span<const std::uint8_t> wire,
                                GksLimits limits = {});

} // namespace axiom::knowledge
