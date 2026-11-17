#include "knowledge.hpp"

#include <algorithm>
#include <limits>
#include <openssl/evp.h>

namespace axiom::knowledge {
namespace {

void require(bool condition, ErrorCode code, const char* message) {
    if (!condition) {
        throw Error(code, message);
    }
}

void text(std::string_view value, std::size_t maximum = maximum_text_bytes) {
    require(maximum <= maximum_object_bytes && value.size() <= maximum,
            ErrorCode::Budget, "Knowledge text limit exceeded");
    for (std::size_t i = 0; i < value.size();) {
        const auto lead = static_cast<std::uint8_t>(value[i++]);
        require(lead != 0, ErrorCode::Schema, "NUL in knowledge metadata");
        if (lead < 0x80) {
            continue;
        }
        unsigned continuation = 0;
        std::uint32_t point = 0, minimum = 0;
        if (lead >= 0xc2 && lead <= 0xdf) {
            continuation = 1;
            point = lead & 0x1fU;
            minimum = 0x80;
        } else if (lead >= 0xe0 && lead <= 0xef) {
            continuation = 2;
            point = lead & 0x0fU;
            minimum = 0x800;
        } else if (lead >= 0xf0 && lead <= 0xf4) {
            continuation = 3;
            point = lead & 0x07U;
            minimum = 0x10000;
        } else {
            throw Error(ErrorCode::Schema, "Invalid UTF-8 metadata lead");
        }
        require(continuation <= value.size() - i, ErrorCode::Schema, "Truncated UTF-8 metadata");
        for (unsigned j = 0; j < continuation; ++j) {
            const auto next = static_cast<std::uint8_t>(value[i++]);
            require((next & 0xc0U) == 0x80U, ErrorCode::Schema, "Invalid UTF-8 continuation");
            point = (point << 6U) | (next & 0x3fU);
        }
        require(point >= minimum && point <= 0x10ffffU &&
                    !(point >= 0xd800U && point <= 0xdfffU),
                ErrorCode::Schema, "Invalid UTF-8 scalar");
    }
}

void validate(const Object& object) {
    require(object.version == schema_version, ErrorCode::Schema, "Unsupported CoK schema");
    require(object.kind >= ObjectKind::Claim && object.kind <= ObjectKind::Contradiction,
            ErrorCode::Schema, "Unknown knowledge object kind");
    require(object.visibility == Visibility::Private || object.visibility == Visibility::Public,
            ErrorCode::Schema, "Unknown knowledge visibility");
    require(!object.format.empty(), ErrorCode::Schema, "Missing payload contract");
    require((object.kind == ObjectKind::Derivation) == object.derivation.has_value(),
            ErrorCode::Schema, "Derivation object/record mismatch");
}

void extent(const SourceReference& source) {
    require(source.offset <= std::numeric_limits<std::uint64_t>::max() - source.length,
            ErrorCode::Schema, "Source extent overflow");
}

struct Writer {
    Bytes bytes;

    void append(std::span<const std::uint8_t> value) {
        require(value.size() <= maximum_object_bytes - bytes.size(),
                ErrorCode::Budget, "Knowledge object byte limit exceeded");
        bytes.insert(bytes.end(), value.begin(), value.end());
    }
    void number(std::uint64_t value, unsigned width) {
        for (unsigned i = 0; i < width; ++i) {
            const auto byte = static_cast<std::uint8_t>(value >> (8 * i));
            append(std::span<const std::uint8_t>(&byte, 1));
        }
    }
    void count(std::size_t size) {
        require(size <= maximum_items, ErrorCode::Budget, "Knowledge collection limit exceeded");
        number(size, 4);
    }
    void string(std::string_view value) {
        text(value);
        number(value.size(), 4);
        append({reinterpret_cast<const std::uint8_t*>(value.data()), value.size()});
    }
    void id(const ObjectId& value) { append(value.digest); }
    void ids(const std::vector<ObjectId>& values) {
        count(values.size());
        for (const auto& value : values) {
            id(value);
        }
    }
};

struct Reader {
    std::span<const std::uint8_t> bytes;
    std::size_t position = 0;

    std::span<const std::uint8_t> take(std::size_t size) {
        require(size <= bytes.size() - position, ErrorCode::Schema, "Truncated knowledge object");
        const auto result = bytes.subspan(position, size);
        position += size;
        return result;
    }
    std::uint64_t number(unsigned width) {
        const auto value = take(width);
        std::uint64_t result = 0;
        for (unsigned i = 0; i < width; ++i) {
            result |= static_cast<std::uint64_t>(value[i]) << (8 * i);
        }
        return result;
    }
    std::size_t count() {
        const auto size = number(4);
        require(size <= maximum_items, ErrorCode::Budget, "Knowledge collection limit exceeded");
        return static_cast<std::size_t>(size);
    }
    std::string string() {
        const auto size = number(4);
        require(size <= maximum_text_bytes, ErrorCode::Budget, "Knowledge text limit exceeded");
        const auto value = take(static_cast<std::size_t>(size));
        std::string result;
        if (!value.empty()) {
            result.assign(reinterpret_cast<const char*>(value.data()), value.size());
        }
        text(result);
        return result;
    }
    ObjectId id() {
        ObjectId result;
        const auto value = take(result.digest.size());
        std::copy(value.begin(), value.end(), result.digest.begin());
        return result;
    }
    std::vector<ObjectId> ids() {
        const auto size = count();
        require(size <= (bytes.size() - position) / Digest{}.size(),
                ErrorCode::Schema, "Truncated knowledge identities");
        std::vector<ObjectId> result;
        result.reserve(size);
        for (std::size_t i = 0; i < size; ++i) {
            result.push_back(id());
        }
        return result;
    }
};

void unique(std::vector<ObjectId>& values) {
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
}

} // namespace

void validate_text(std::string_view value, std::size_t maximum) {
    text(value, maximum);
}

Digest digest(std::span<const std::uint8_t> bytes) {
    Digest result;
    unsigned length = 0;
    require(EVP_Digest(bytes.data(), bytes.size(), result.data(), &length, EVP_sha256(), nullptr) == 1 &&
                length == result.size(),
            ErrorCode::Integrity, "Cannot compute knowledge digest");
    return result;
}

std::string hex(const ObjectId& id) {
    constexpr std::string_view digits = "0123456789abcdef";
    std::string result;
    result.reserve(id.digest.size() * 2);
    for (const auto byte : id.digest) {
        result.push_back(digits[byte >> 4U]);
        result.push_back(digits[byte & 0x0fU]);
    }
    return result;
}

ObjectId parse_id(std::string_view value) {
    ObjectId result;
    require(value.size() == result.digest.size() * 2, ErrorCode::Schema, "Invalid object identity size");
    constexpr std::string_view digits = "0123456789abcdef";
    for (std::size_t i = 0; i < result.digest.size(); ++i) {
        const auto high = digits.find(value[2 * i]), low = digits.find(value[2 * i + 1]);
        require(high != std::string_view::npos && low != std::string_view::npos,
                ErrorCode::Schema, "Noncanonical object identity");
        result.digest[i] = static_cast<std::uint8_t>((high << 4U) | low);
    }
    return result;
}

Bytes encode_object(const Object& object) {
    validate(object);
    Writer out;
    constexpr std::array<std::uint8_t, 4> magic{'G', 'K', 'O', 'B'};
    out.append(magic);
    out.number(object.version, 2);
    out.number(static_cast<std::uint8_t>(object.kind), 1);
    out.number(static_cast<std::uint8_t>(object.visibility), 1);
    out.string(object.label);
    out.string(object.format);
    require(object.payload.size() <= maximum_object_bytes, ErrorCode::Budget, "Payload limit exceeded");
    out.number(object.payload.size(), 4);
    out.append(object.payload);
    out.count(object.concepts.size());
    for (const auto& tag : object.concepts) {
        out.string(tag);
    }
    out.count(object.sources.size());
    for (const auto& source : object.sources) {
        extent(source);
        out.string(source.archive_id);
        out.string(source.archive_version);
        out.string(source.entry_path);
        out.string(source.extractor_version);
        out.append(source.source_digest);
        out.number(source.offset, 8);
        out.number(source.length, 8);
    }
    out.count(object.links.size());
    for (const auto& link : object.links) {
        require(link.relationship >= Relationship::Requires &&
                    link.relationship <= Relationship::AlternativeTo,
                ErrorCode::Schema, "Unknown knowledge relationship");
        out.number(static_cast<std::uint8_t>(link.relationship), 1);
        out.id(link.target);
    }
    out.number(object.derivation.has_value(), 1);
    if (object.derivation) {
        const auto& value = *object.derivation;
        out.id(value.conclusion);
        out.id(value.rule);
        out.ids(value.premises);
        out.ids(value.assumptions);
        out.ids(value.evidence);
    }
    return std::move(out.bytes);
}

Object decode_object(std::span<const std::uint8_t> bytes) {
    require(bytes.size() <= maximum_object_bytes, ErrorCode::Budget, "Knowledge object byte limit exceeded");
    Reader in{bytes};
    const auto magic = in.take(4);
    require(magic[0] == 'G' && magic[1] == 'K' && magic[2] == 'O' && magic[3] == 'B',
            ErrorCode::Schema, "Invalid knowledge object magic");
    Object object;
    object.version = static_cast<std::uint16_t>(in.number(2));
    require(object.version == schema_version, ErrorCode::Schema, "Unsupported CoK schema");
    object.kind = static_cast<ObjectKind>(in.number(1));
    object.visibility = static_cast<Visibility>(in.number(1));
    object.label = in.string();
    object.format = in.string();
    const auto payload_size = in.number(4);
    require(payload_size <= maximum_object_bytes, ErrorCode::Budget, "Payload limit exceeded");
    const auto payload = in.take(static_cast<std::size_t>(payload_size));
    object.payload.assign(payload.begin(), payload.end());
    const auto concepts = in.count();
    for (std::size_t i = 0; i < concepts; ++i) {
        object.concepts.push_back(in.string());
    }
    const auto sources = in.count();
    for (std::size_t i = 0; i < sources; ++i) {
        SourceReference source;
        source.archive_id = in.string();
        source.archive_version = in.string();
        source.entry_path = in.string();
        source.extractor_version = in.string();
        const auto value = in.take(source.source_digest.size());
        std::copy(value.begin(), value.end(), source.source_digest.begin());
        source.offset = in.number(8);
        source.length = in.number(8);
        extent(source);
        object.sources.push_back(std::move(source));
    }
    const auto links = in.count();
    for (std::size_t i = 0; i < links; ++i) {
        const auto kind = static_cast<Relationship>(in.number(1));
        require(kind >= Relationship::Requires && kind <= Relationship::AlternativeTo,
                ErrorCode::Schema, "Unknown knowledge relationship");
        object.links.push_back({kind, in.id()});
    }
    const auto has_derivation = in.number(1);
    require(has_derivation <= 1, ErrorCode::Schema, "Invalid derivation marker");
    if (has_derivation) {
        Derivation value;
        value.conclusion = in.id();
        value.rule = in.id();
        value.premises = in.ids();
        value.assumptions = in.ids();
        value.evidence = in.ids();
        object.derivation = std::move(value);
    }
    require(in.position == bytes.size(), ErrorCode::Schema, "Trailing knowledge bytes");
    validate(object);
    return object;
}

ObjectId identify(const Object& object) {
    return {digest(encode_object(object))};
}

std::string indexed_text(const Object& object) {
    std::string result = object.label;
    for (const auto& tag : object.concepts) {
        result += '\n';
        result += tag;
    }
    for (const auto& source : object.sources) {
        result += '\n';
        result += source.archive_id;
        result += '\n';
        result += source.entry_path;
    }
    if (object.format == "text/plain; charset=utf-8") {
        result += '\n';
        if (!object.payload.empty()) {
            result.append(reinterpret_cast<const char*>(object.payload.data()), object.payload.size());
        }
    }
    validate_text(result, maximum_object_bytes);
    return result;
}

Object decode_object(const ObjectId& expected, std::span<const std::uint8_t> bytes) {
    require(bytes.size() <= maximum_object_bytes, ErrorCode::Budget, "Knowledge object byte limit exceeded");
    require(digest(bytes) == expected.digest, ErrorCode::Integrity, "Knowledge object digest mismatch");
    return decode_object(bytes);
}

std::vector<ObjectId> prerequisites(const Object& object) {
    std::vector<ObjectId> result;
    for (const auto& link : object.links) {
        if (link.relationship == Relationship::Requires || link.relationship == Relationship::DependsOn) {
            result.push_back(link.target);
        }
    }
    if (object.derivation) {
        const auto& value = *object.derivation;
        result.push_back(value.rule);
        for (const auto* ids : {&value.premises, &value.assumptions, &value.evidence}) {
            result.insert(result.end(), ids->begin(), ids->end());
        }
    }
    unique(result);
    return result;
}

std::vector<ObjectId> references(const Object& object) {
    auto result = prerequisites(object);
    for (const auto& link : object.links) {
        result.push_back(link.target);
    }
    if (object.derivation) {
        result.push_back(object.derivation->conclusion);
    }
    unique(result);
    return result;
}

std::vector<Link> relationships(const Object& object) {
    auto result = object.links;
    if (object.derivation) {
        const auto& value = *object.derivation;
        result.push_back({Relationship::Produces, value.conclusion});
        result.push_back({Relationship::DependsOn, value.rule});
        for (const auto* ids : {&value.premises, &value.assumptions}) {
            for (const auto& id : *ids) {
                result.push_back({Relationship::Requires, id});
            }
        }
        for (const auto& id : value.evidence) {
            result.push_back({Relationship::Supports, id});
        }
    }
    return result;
}

} // namespace axiom::knowledge
