#include "gks.hpp"

#include <algorithm>
#include <zstd.h>

namespace axiom::knowledge {
namespace {
constexpr std::size_t envelope_bytes = 16;
constexpr std::size_t max_stored_bytes = gks_max_block_bytes + 65536;
enum class BlockKind : std::uint8_t { Objects = 1, Index, Manifest };

void require(bool value, ErrorCode code, const char* message) {
    if (!value) throw Error(code, message);
}
void bounds(const GksLimits& limits) {
    require(limits.block_bytes > 0 && limits.block_bytes <= gks_max_block_bytes
        && limits.records > 0 && limits.records <= maximum_items
        && limits.key_bytes > 0 && limits.key_bytes <= gks_max_key_bytes
        && limits.index_depth <= 16, ErrorCode::Budget, "Invalid GKS decoding limits");
}
struct Writer {
    GksLimits limits;
    Bytes bytes;
    void append(std::span<const std::uint8_t> value) {
        require(value.size() <= limits.block_bytes - bytes.size(), ErrorCode::Budget, "GKS block limit exceeded");
        bytes.insert(bytes.end(), value.begin(), value.end());
    }
    void number(std::uint64_t value, unsigned width) {
        for (unsigned i = 0; i < width; ++i) {
            const auto byte = static_cast<std::uint8_t>(value >> (8 * i));
            append({&byte, 1});
        }
    }
    void count(std::size_t value) {
        require(value <= limits.records, ErrorCode::Budget, "GKS record limit exceeded");
        number(value, 4);
    }
    void text(std::string_view value) {
        validate_text(value);
        number(value.size(), 4);
        append({reinterpret_cast<const std::uint8_t*>(value.data()), value.size()});
    }
    void position(const IndexPosition& value) {
        require(value.key.size() <= limits.key_bytes, ErrorCode::Budget, "GKS key limit exceeded");
        number(value.key.size(), 4);
        append(value.key);
        append(value.object.digest);
    }
};
struct Reader {
    GksLimits limits;
    std::span<const std::uint8_t> bytes;
    std::size_t cursor = 0;
    std::span<const std::uint8_t> take(std::size_t size) {
        require(size <= bytes.size() - cursor, ErrorCode::Schema, "Truncated GKS block");
        const auto result = bytes.subspan(cursor, size);
        cursor += size;
        return result;
    }
    std::uint64_t number(unsigned width) {
        std::uint64_t result = 0;
        const auto part = take(width);
        for (unsigned i = 0; i < width; ++i) result |= std::uint64_t(part[i]) << (8 * i);
        return result;
    }
    std::size_t count() {
        const auto size = number(4);
        require(size <= limits.records, ErrorCode::Budget, "GKS record limit exceeded");
        return static_cast<std::size_t>(size);
    }
    Digest hash() {
        Digest result;
        const auto part = take(result.size());
        std::copy(part.begin(), part.end(), result.begin());
        return result;
    }
    std::string text() {
        const auto size = number(4);
        require(size <= maximum_text_bytes, ErrorCode::Budget, "GKS text limit exceeded");
        const auto part = take(size);
        std::string value(part.begin(), part.end());
        validate_text(value);
        return value;
    }
    IndexPosition position() {
        const auto size = number(4);
        require(size <= limits.key_bytes, ErrorCode::Budget, "GKS key limit exceeded");
        const auto part = take(size);
        return {Bytes(part.begin(), part.end()), ObjectId{hash()}};
    }
    void end() { require(cursor == bytes.size(), ErrorCode::Schema, "Trailing GKS data"); }
};
Bytes pack(BlockKind kind, const Writer& body) {
    Bytes compressed(ZSTD_compressBound(body.bytes.size()));
    const auto size = ZSTD_compress(compressed.data(), compressed.size(),
                                    body.bytes.data(), body.bytes.size(), 3);
    require(!ZSTD_isError(size) && size <= max_stored_bytes, ErrorCode::Storage, "GKS compression failed");
    compressed.resize(size);
    Writer header{{envelope_bytes + max_stored_bytes, maximum_items, gks_max_key_bytes, 16}, {}};
    constexpr std::uint8_t magic[]{'G', 'K', 'S', 'B'};
    header.append(magic);
    header.number(gks_version, 2);
    header.number(static_cast<std::uint8_t>(kind), 1);
    header.number(1, 1); // One known-size, dictionary-free Zstandard frame.
    header.number(body.bytes.size(), 4);
    header.number(size, 4);
    header.append(compressed);
    return std::move(header.bytes);
}
Bytes unpack(const BlockId& expected, std::span<const std::uint8_t> wire,
             BlockKind kind, GksLimits limits) {
    bounds(limits);
    require(wire.size() >= envelope_bytes && wire.size() <= envelope_bytes + max_stored_bytes,
            ErrorCode::Budget, "GKS stored block size exceeds bounds");
    require(identify_block(wire) == expected, ErrorCode::Integrity, "GKS block hash mismatch");
    Reader header{limits, wire};
    constexpr std::uint8_t magic[]{'G', 'K', 'S', 'B'};
    const auto actual_magic = header.take(4);
    require(std::equal(actual_magic.begin(), actual_magic.end(), std::begin(magic))
        && header.number(2) == gks_version && header.number(1) == static_cast<std::uint8_t>(kind)
        && header.number(1) == 1, ErrorCode::Schema, "Unsupported GKS block envelope");
    const auto raw_size = header.number(4), stored_size = header.number(4);
    require(raw_size > 0 && raw_size <= limits.block_bytes,
            ErrorCode::Budget, "GKS decompression size exceeds bounds");
    require(stored_size == wire.size() - envelope_bytes, ErrorCode::Schema, "GKS stored extent mismatch");
    const auto compressed = header.take(stored_size);
    require(ZSTD_getFrameContentSize(compressed.data(), compressed.size()) == raw_size
        && ZSTD_getDictID_fromFrame(compressed.data(), compressed.size()) == 0,
        ErrorCode::Schema, "GKS frame size or dictionary differs");
    const auto frame_size = ZSTD_findFrameCompressedSize(compressed.data(), compressed.size());
    require(!ZSTD_isError(frame_size) && frame_size == stored_size,
            ErrorCode::Schema, "GKS requires exactly one complete frame");
    Bytes result(raw_size);
    const auto decoded = ZSTD_decompress(result.data(), result.size(), compressed.data(), compressed.size());
    require(!ZSTD_isError(decoded) && decoded == raw_size, ErrorCode::Integrity, "Invalid compressed GKS content");
    return result;
}
void assessment(Writer& writer, const Assessment& value) {
    require(value.version > 0 && !value.checker.empty()
        && value.state >= KnowledgeState::Provisional && value.state <= KnowledgeState::Archived
        && value.logical >= LogicalValidity::Unchecked && value.logical <= LogicalValidity::Invalid
        && value.evidence >= EvidenceAssessment::Unassessed && value.evidence <= EvidenceAssessment::Conflicting,
        ErrorCode::Schema, "Invalid GKS assessment observation");
    writer.number(value.version, 8);
    writer.number(static_cast<std::uint8_t>(value.state), 1);
    writer.number(static_cast<std::uint8_t>(value.logical), 1);
    writer.number(static_cast<std::uint8_t>(value.evidence), 1);
    writer.text(value.checker); writer.text(value.reason);
    writer.number(value.certificate.has_value(), 1);
    if (value.certificate) writer.append(value.certificate->digest);
    require(value.checked_dependencies.size() <= maximum_assessment_dependencies,
            ErrorCode::Budget, "GKS assessment dependency limit exceeded");
    writer.number(value.checked_dependencies.size(), 4);
    for (const auto& dependency : value.checked_dependencies) writer.append(dependency.digest);
}
Assessment assessment(Reader& reader) {
    Assessment result;
    result.version = reader.number(8);
    result.state = static_cast<KnowledgeState>(reader.number(1));
    result.logical = static_cast<LogicalValidity>(reader.number(1));
    result.evidence = static_cast<EvidenceAssessment>(reader.number(1));
    result.checker = reader.text(); result.reason = reader.text();
    const auto has_certificate = reader.number(1);
    require(has_certificate <= 1, ErrorCode::Schema, "Invalid GKS optional certificate flag");
    if (has_certificate) result.certificate = ObjectId{reader.hash()};
    const auto count = reader.number(4);
    require(count <= maximum_assessment_dependencies, ErrorCode::Budget, "GKS assessment dependency limit exceeded");
    for (std::size_t i = 0; i < count; ++i) result.checked_dependencies.push_back(ObjectId{reader.hash()});
    Writer check{reader.limits, {}};
    assessment(check, result);
    return result;
}
void index_valid(const IndexChunk& chunk, GksLimits limits) {
    validate_text(chunk.key_format, 128);
    require(!chunk.key_format.empty() && chunk.level <= 16,
            ErrorCode::Schema, "Invalid GKS index format or depth");
    require(chunk.level <= limits.index_depth && chunk.entries.size() <= limits.records
        && chunk.children.size() <= limits.records, ErrorCode::Budget, "GKS index budget exceeded");
    if (chunk.level == 0) {
        require(!chunk.entries.empty() && chunk.children.empty(), ErrorCode::Schema, "Invalid GKS leaf");
        for (const auto& entry : chunk.entries) {
            require(entry.ordinal < maximum_items, ErrorCode::Schema, "GKS object ordinal out of range");
        }
        for (std::size_t i = 1; i < chunk.entries.size(); ++i) {
            require(chunk.entries[i - 1].position < chunk.entries[i].position,
                    ErrorCode::Schema, "GKS index entries are not strictly increasing");
        }
    } else {
        require(chunk.entries.empty() && !chunk.children.empty(), ErrorCode::Schema, "Invalid GKS index branch");
        for (std::size_t i = 0; i < chunk.children.size(); ++i) {
            require(chunk.children[i].first <= chunk.children[i].last
                && (!i || chunk.children[i - 1].last < chunk.children[i].first),
                ErrorCode::Schema, "Overlapping or invalid GKS child ranges");
        }
    }
}
void manifest_valid(const GksManifest& manifest, GksLimits limits) {
    validate_text(manifest.name, 128); validate_text(manifest.revision, 128);
    require(!manifest.name.empty() && !manifest.revision.empty() && manifest.object_schema == schema_version
        && manifest.indexes.size() <= 7 && manifest.parents.size() <= 16,
        ErrorCode::Schema, "Invalid GKS manifest metadata");
    require(manifest.object_count ? (!manifest.indexes.empty()
        && manifest.indexes.front().kind == IndexKind::Objects) : manifest.indexes.empty(),
        ErrorCode::Schema, "GKS object count/index mismatch");
    for (std::size_t i = 0; i < manifest.indexes.size(); ++i) {
        const auto& root = manifest.indexes[i];
        validate_text(root.key_format, 128);
        require(root.kind >= IndexKind::Objects && root.kind <= IndexKind::Prerequisite
            && !root.key_format.empty() && root.level <= 16
            && (!i || manifest.indexes[i - 1].kind < root.kind),
            ErrorCode::Schema, "Invalid or duplicate GKS index root");
        require(root.level <= limits.index_depth, ErrorCode::Budget, "GKS root depth exceeds bounds");
    }
    for (std::size_t i = 1; i < manifest.parents.size(); ++i) {
        require(manifest.parents[i - 1] < manifest.parents[i], ErrorCode::Schema, "Noncanonical GKS parents");
    }
}
}

BlockId identify_block(std::span<const std::uint8_t> wire) { return {digest(wire)}; }
std::string hex(const BlockId& id) { return hex(ObjectId{id.digest}); }
BlockId parse_block_id(std::string_view value) { return {parse_id(value).digest}; }

Bytes encode_object_chunk(const ObjectChunk& chunk, GksLimits limits) {
    bounds(limits);
    require(!chunk.records.empty(), ErrorCode::Schema, "Empty GKS object chunk");
    Writer writer{limits, {}};
    writer.count(chunk.records.size());
    std::optional<ObjectId> previous;
    for (const auto& record : chunk.records) {
        const auto wire = encode_object(record.object);
        const ObjectId id{digest(wire)};
        require(!previous || *previous < id, ErrorCode::Schema, "GKS objects are not strictly increasing");
        previous = id;
        writer.append(id.digest);
        writer.number(wire.size(), 4);
        writer.append(wire);
        writer.number(record.assessment.has_value(), 1);
        if (record.assessment) assessment(writer, *record.assessment);
    }
    return pack(BlockKind::Objects, writer);
}
ObjectChunk decode_object_chunk(const BlockId& expected, std::span<const std::uint8_t> wire, GksLimits limits) {
    const auto body = unpack(expected, wire, BlockKind::Objects, limits);
    Reader reader{limits, body};
    ObjectChunk result;
    const auto count = reader.count();
    require(count > 0, ErrorCode::Schema, "Empty GKS object chunk");
    std::optional<ObjectId> previous;
    for (std::size_t i = 0; i < count; ++i) {
        const ObjectId id{reader.hash()};
        require(!previous || *previous < id, ErrorCode::Schema, "GKS object order mismatch");
        previous = id;
        const auto size = reader.number(4);
        require(size <= maximum_object_bytes, ErrorCode::Budget, "GKS object extent exceeds bounds");
        KnowledgeRecord record{decode_object(id, reader.take(size)), {}};
        const auto has_assessment = reader.number(1);
        require(has_assessment <= 1, ErrorCode::Schema, "Invalid GKS optional assessment flag");
        if (has_assessment) record.assessment = assessment(reader);
        result.records.push_back(std::move(record));
    }
    reader.end();
    return result;
}
Bytes encode_index_chunk(const IndexChunk& chunk, GksLimits limits) {
    bounds(limits); index_valid(chunk, limits);
    Writer writer{limits, {}};
    writer.text(chunk.key_format); writer.number(chunk.level, 4);
    writer.count(chunk.level ? chunk.children.size() : chunk.entries.size());
    for (const auto& entry : chunk.entries) {
        writer.position(entry.position); writer.append(entry.block.digest); writer.number(entry.ordinal, 4);
    }
    for (const auto& child : chunk.children) {
        writer.position(child.first); writer.position(child.last); writer.append(child.block.digest);
    }
    return pack(BlockKind::Index, writer);
}
IndexChunk decode_index_chunk(const BlockId& expected, std::span<const std::uint8_t> wire, GksLimits limits) {
    const auto body = unpack(expected, wire, BlockKind::Index, limits);
    Reader reader{limits, body};
    IndexChunk result;
    result.key_format = reader.text(); result.level = reader.number(4);
    require(result.level <= limits.index_depth, ErrorCode::Budget, "GKS index depth exceeds bounds");
    const auto count = reader.count();
    for (std::size_t i = 0; i < count; ++i) {
        if (!result.level) {
            auto position = reader.position();
            const BlockId block{reader.hash()};
            result.entries.push_back({std::move(position), block, static_cast<std::uint32_t>(reader.number(4))});
        } else {
            auto first = reader.position(), last = reader.position();
            result.children.push_back({std::move(first), std::move(last), BlockId{reader.hash()}});
        }
    }
    reader.end(); index_valid(result, limits);
    return result;
}
Bytes encode_gks_manifest(const GksManifest& manifest, GksLimits limits) {
    bounds(limits); manifest_valid(manifest, limits);
    Writer writer{limits, {}};
    writer.text(manifest.name); writer.text(manifest.revision);
    writer.number(manifest.object_schema, 2); writer.number(manifest.object_count, 8);
    writer.count(manifest.indexes.size());
    for (const auto& root : manifest.indexes) {
        writer.number(static_cast<std::uint8_t>(root.kind), 1);
        writer.text(root.key_format); writer.number(root.level, 4); writer.append(root.block.digest);
    }
    writer.count(manifest.parents.size());
    for (const auto& parent : manifest.parents) writer.append(parent.digest);
    return pack(BlockKind::Manifest, writer);
}
GksManifest decode_gks_manifest(const BlockId& expected, std::span<const std::uint8_t> wire, GksLimits limits) {
    const auto body = unpack(expected, wire, BlockKind::Manifest, limits);
    Reader reader{limits, body};
    GksManifest result;
    result.name = reader.text(); result.revision = reader.text();
    result.object_schema = reader.number(2); result.object_count = reader.number(8);
    auto count = reader.count();
    require(count <= 7, ErrorCode::Schema, "Too many GKS index roots");
    for (std::size_t i = 0; i < count; ++i) {
        const auto kind = static_cast<IndexKind>(reader.number(1));
        auto format = reader.text();
        const auto level = static_cast<std::uint32_t>(reader.number(4));
        result.indexes.push_back({kind, std::move(format), level, BlockId{reader.hash()}});
    }
    count = reader.count();
    require(count <= 16, ErrorCode::Schema, "Too many GKS parent manifests");
    for (std::size_t i = 0; i < count; ++i) result.parents.push_back(BlockId{reader.hash()});
    reader.end(); manifest_valid(result, limits);
    return result;
}

} // namespace axiom::knowledge
