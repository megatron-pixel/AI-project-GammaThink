#include "remote_query.hpp"

#include <algorithm>

namespace axiom::knowledge {
namespace {
void require(bool value, ErrorCode code, const char* message) {
    if (!value) throw Error(code, message);
}
constexpr std::array<QueryKind, 7> kinds{QueryKind::All, QueryKind::Kind, QueryKind::Concept,
    QueryKind::Text, QueryKind::Source, QueryKind::Related, QueryKind::RequiredBy};
void integer(Bytes& bytes, std::uint32_t value, unsigned width) {
    for (unsigned i = 0; i < width; ++i) bytes.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
}
void text(Bytes& bytes, std::string_view value) {
    validate_text(value);
    integer(bytes, static_cast<std::uint32_t>(value.size()), 4);
    bytes.insert(bytes.end(), value.begin(), value.end());
}
void hash(Bytes& bytes, const Digest& value) { bytes.insert(bytes.end(), value.begin(), value.end()); }
struct Reader {
    std::span<const std::uint8_t> bytes;
    std::size_t offset = 0;
    std::span<const std::uint8_t> take(std::size_t count) {
        require(count <= bytes.size() - offset, ErrorCode::Schema, "Truncated remote query wire");
        const auto result = bytes.subspan(offset, count); offset += count; return result;
    }
    std::uint32_t number(unsigned width) {
        const auto value = take(width);
        std::uint32_t result = 0;
        for (unsigned i = 0; i < width; ++i) result |= static_cast<std::uint32_t>(value[i]) << (8 * i);
        return result;
    }
    bool boolean() {
        const auto value = number(1);
        require(value <= 1, ErrorCode::Schema, "Noncanonical remote query boolean"); return value == 1;
    }
    Digest digest() {
        Digest result; const auto value = take(32); std::copy(value.begin(), value.end(), result.begin()); return result;
    }
    std::string string() {
        const auto size = number(4);
        require(size <= maximum_text_bytes, ErrorCode::Budget, "Remote query text exceeds bounds");
        const auto value = take(size);
        std::string result(value.begin(), value.end()); validate_text(result); return result;
    }
    void prefix(const Bytes& expected) {
        const auto value = take(expected.size());
        require(std::equal(value.begin(), value.end(), expected.begin()), ErrorCode::Schema, "Unsupported remote query profile");
    }
    void end() const { require(offset == bytes.size(), ErrorCode::Schema, "Trailing remote query wire"); }
};
void page_shape(const ObjectPage& page) {
    require(page.objects.size() <= 256, ErrorCode::Budget, "Remote query page exceeds bounds");
    require((page.complete && !page.next) || (!page.complete && !page.objects.empty() && page.next == page.objects.back()),
            ErrorCode::Schema, "Invalid remote query continuation");
    std::optional<ObjectId> previous;
    for (const auto& id : page.objects) {
        require(!previous || id > *previous, ErrorCode::Schema, "Remote query IDs not strictly ordered");
        previous = id;
    }
}
}
Bytes encode_remote_query(const RemoteQueryRequest& request) {
    validate_knowledge_query(request.query, request.limit);
    Bytes bytes{'G','K','Q','R',1,0,0,0};
    hash(bytes, request.root.digest);
    bytes.push_back(request.after.has_value());
    if (request.after) hash(bytes, request.after->digest);
    integer(bytes, static_cast<std::uint32_t>(request.limit), 2);
    const auto kind = std::find(kinds.begin(), kinds.end(), request.query.kind);
    bytes.push_back(static_cast<std::uint8_t>(kind - kinds.begin() + 1));
    switch (request.query.kind) {
    case QueryKind::All: break;
    case QueryKind::Kind: bytes.push_back(static_cast<std::uint8_t>(request.query.object_kind)); break;
    case QueryKind::Concept: case QueryKind::Text: text(bytes, request.query.value); break;
    case QueryKind::Source:
        text(bytes, request.query.value);
        bytes.push_back(request.query.source_version.has_value());
        if (request.query.source_version) text(bytes, *request.query.source_version);
        break;
    case QueryKind::Related:
        hash(bytes, request.query.target.digest);
        bytes.push_back(static_cast<std::uint8_t>(request.query.relationship)); break;
    case QueryKind::RequiredBy: hash(bytes, request.query.target.digest); break;
    }
    require(bytes.size() <= remote_query_request_bytes, ErrorCode::Budget, "Remote query request exceeds bounds");
    return bytes;
}
RemoteQueryRequest decode_remote_query(std::span<const std::uint8_t> bytes) {
    require(bytes.size() <= remote_query_request_bytes, ErrorCode::Budget, "Remote query request exceeds bounds");
    Reader reader{bytes}; reader.prefix({'G','K','Q','R',1,0,0,0});
    RemoteQueryRequest request; request.root.digest = reader.digest();
    if (reader.boolean()) request.after = ObjectId{reader.digest()};
    request.limit = reader.number(2);
    const auto kind = reader.number(1);
    require(kind >= 1 && kind <= kinds.size(), ErrorCode::Schema, "Unknown remote query kind");
    request.query.kind = kinds[kind - 1];
    switch (request.query.kind) {
    case QueryKind::All: break;
    case QueryKind::Kind: request.query.object_kind = static_cast<ObjectKind>(reader.number(1)); break;
    case QueryKind::Concept: case QueryKind::Text: request.query.value = reader.string(); break;
    case QueryKind::Source:
        request.query.value = reader.string();
        if (reader.boolean()) request.query.source_version = reader.string();
        break;
    case QueryKind::Related:
        request.query.target.digest = reader.digest();
        request.query.relationship = static_cast<Relationship>(reader.number(1)); break;
    case QueryKind::RequiredBy: request.query.target.digest = reader.digest(); break;
    }
    reader.end(); validate_knowledge_query(request.query, request.limit);
    return request;
}
Bytes encode_remote_query_page(const RemoteQueryPage& response) {
    page_shape(response.page);
    Bytes bytes{'G','K','Q','P',1,0,0,0}; hash(bytes, response.request_digest);
    bytes.push_back(response.page.complete);
    integer(bytes, static_cast<std::uint32_t>(response.page.objects.size()), 2);
    for (const auto& id : response.page.objects) hash(bytes, id.digest);
    return bytes;
}
RemoteQueryPage decode_remote_query_page(std::span<const std::uint8_t> bytes) {
    require(bytes.size() <= remote_query_page_bytes, ErrorCode::Budget, "Remote query response exceeds bounds");
    Reader reader{bytes}; reader.prefix({'G','K','Q','P',1,0,0,0});
    RemoteQueryPage result; result.request_digest = reader.digest();
    result.page.complete = reader.boolean();
    const auto count = reader.number(2);
    require(count <= 256, ErrorCode::Budget, "Remote query count exceeds bounds");
    for (std::uint32_t i = 0; i < count; ++i) result.page.objects.push_back(ObjectId{reader.digest()});
    reader.end();
    if (!result.page.complete && !result.page.objects.empty()) result.page.next = result.page.objects.back();
    page_shape(result.page); return result;
}
RemoteQueryArchive::RemoteQueryArchive(std::shared_ptr<BlockSource> source, const BlockId& root,
                                       RemoteQueryOptions options, IndexQueryLimits limits)
    : root_(root), http_(options), archive_(std::move(source), root, limits),
      disclose_(options.allow_query_disclosure) {}
std::optional<KnowledgeRecord> RemoteQueryArchive::get(const ObjectId& id) { return archive_.get(id); }
ObjectPage RemoteQueryArchive::query(const KnowledgeQuery& query, std::optional<ObjectId> after, std::size_t limit) {
    validate_knowledge_query(query, limit);
    require(disclose_, ErrorCode::Permission, "Remote query disclosure is not enabled");
    const auto request = encode_remote_query({root_, query, after, limit});
    const auto response = decode_remote_query_page(http_.query_post("/gammathink/query/v1", request, remote_query_page_bytes));
    require(response.request_digest == identify_block(request).digest, ErrorCode::Integrity, "Remote response belongs to another request");
    const auto verified = archive_.query(query, after, limit);
    require(response.page.objects == verified.objects && response.page.complete == verified.complete
        && response.page.next == verified.next, ErrorCode::Integrity, "Remote query page differs from selected snapshot");
    return verified;
}

} // namespace axiom::knowledge
