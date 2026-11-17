#pragma once

#include "block_cid.hpp"

namespace axiom::knowledge {

class BlockSource;
using PublisherKey = std::array<std::uint8_t, 32>;
struct PublisherFeedId {
    Digest digest{};
    auto operator<=>(const PublisherFeedId&) const = default;
};
std::string hex(const PublisherFeedId& id);
std::string publisher_key_text(const PublisherKey& key);
PublisherKey parse_publisher_key(std::string_view text);

struct SnapshotOffer {
    std::string name, revision;
    BlockId snapshot;
    std::vector<ObjectId> roots;
    // Declared prior snapshot identities, not inferred logical premises.
    std::vector<BlockId> parents;
    bool operator==(const SnapshotOffer&) const = default;
};
struct PublisherFeed {
    std::string channel;
    std::uint64_t sequence = 1;
    std::optional<PublisherFeedId> previous;
    std::vector<SnapshotOffer> offers;
    bool operator==(const PublisherFeed&) const = default;
};
struct PublisherFeedLimits {
    std::size_t wire_bytes = 1024 * 1024;
    std::size_t offers = 64, root_references = maximum_items, parent_references = maximum_items;
};

// Identity/signature verification ONLY. No truth, archive availability, chain
// acceptance, network capability, publication approval or model authority.
class VerifiedPublisherFeed {
public:
    const PublisherKey& publisher() const noexcept { return publisher_; }
    const PublisherFeed& content() const noexcept { return content_; }
    const PublisherFeedId& id() const noexcept { return id_; }
    std::span<const std::uint8_t> wire() const noexcept { return wire_; }
private:
    friend VerifiedPublisherFeed verify_publisher_feed(std::span<const std::uint8_t>,
        const PublisherKey&, PublisherFeedLimits);
    VerifiedPublisherFeed(PublisherKey publisher, PublisherFeed content, PublisherFeedId id, Bytes wire)
        : publisher_(publisher), content_(std::move(content)), id_(id), wire_(std::move(wire)) {}
    PublisherKey publisher_;
    PublisherFeed content_;
    PublisherFeedId id_;
    Bytes wire_;
};

// Explicit host key material, never a provider credential, model output or
// automatically generated/trusted identity. Signing is local and does not
// publish. Caller owns and clears its private seed; these APIs do not log it.
PublisherKey publisher_key_from_seed(std::span<const std::uint8_t> private_seed);
Bytes sign_publisher_feed(const PublisherFeed& feed, std::span<const std::uint8_t> private_seed,
                          PublisherFeedLimits limits = {});
VerifiedPublisherFeed verify_publisher_feed(std::span<const std::uint8_t> wire,
    const PublisherKey& host_pinned_key, PublisherFeedLimits limits = {});
// Explicit host capability and immutable CID. Fetches only this metadata block,
// with no inferred endpoint, retries, archive reads, acceptance or publication.
VerifiedPublisherFeed fetch_publisher_feed(BlockSource& source, const BlockCid& cid,
    const PublisherKey& host_pinned_key, PublisherFeedLimits limits = {});

} // namespace axiom::knowledge
