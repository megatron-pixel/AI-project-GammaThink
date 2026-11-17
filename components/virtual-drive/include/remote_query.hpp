#pragma once

#include "read_http.hpp"
#include "virtual_drive.hpp"

namespace axiom::knowledge {

inline constexpr std::size_t remote_query_request_bytes = 2 * maximum_text_bytes + 128;
inline constexpr std::size_t remote_query_page_bytes = 43 + 32 * 256;
struct RemoteQueryRequest {
    BlockId root;
    KnowledgeQuery query;
    std::optional<ObjectId> after;
    std::size_t limit = 64;
};
struct RemoteQueryPage {
    Digest request_digest{};
    ObjectPage page;
};
Bytes encode_remote_query(const RemoteQueryRequest& request);
RemoteQueryRequest decode_remote_query(std::span<const std::uint8_t> bytes);
Bytes encode_remote_query_page(const RemoteQueryPage& response);
RemoteQueryPage decode_remote_query_page(std::span<const std::uint8_t> bytes);

struct RemoteQueryOptions : ReadHttpOptions {
    // The server receives query values, root, cursor and limit only when true.
    bool allow_query_disclosure = false;
};
// A server page is a proposal, never evidence of completeness or absence.
// Every accepted page must equal an independently evaluated query over the
// caller-selected immutable GKS root. That root does not prove factual truth.
class RemoteQueryArchive final : public KnowledgeArchive {
public:
    RemoteQueryArchive(std::shared_ptr<BlockSource> source, const BlockId& root,
                       RemoteQueryOptions options, IndexQueryLimits limits = {});
    std::optional<KnowledgeRecord> get(const ObjectId& id) override;
    const GksManifest& manifest() const { return archive_.manifest(); }
    ObjectPage query(const KnowledgeQuery& query, std::optional<ObjectId> after, std::size_t limit) override;
    const ReadHttpStats& stats() const { return http_.stats(); }
private:
    BlockId root_;
    ReadHttp http_;
    GksArchive archive_;
    bool disclose_;
};

} // namespace axiom::knowledge
