#pragma once

#include "archive_registry.hpp"
#include "publisher_feed.hpp"

namespace axiom::knowledge {

struct PublisherCatalogueLimits {
    std::size_t feeds = 256, aliases = 256, history = 4096, chain = 64, active_mounts = 15;
    std::uint64_t wire_bytes = 64 * 1024 * 1024, sql_instructions = 10000000;
    std::uint32_t cache_kib = 4096;
};
struct PublisherFeedPage {
    std::vector<PublisherFeedId> feeds;
    std::optional<PublisherFeedId> next;
    bool complete = true;
};
struct CatalogueAcceptance {
    MountState state;
    std::optional<PublisherFeedId> feed;
    std::optional<std::size_t> offer;
};
struct CatalogueAcceptancePage {
    std::vector<CatalogueAcceptance> records;
    std::optional<std::uint64_t> next;
    bool complete = true;
};

// Owner-thread, local append-only discovery and explicit host acceptance.
// Independent publisher/channel chains and forks are retained, not merged into
// a global truth vote. Discovery never mounts, fetches or publishes an archive.
class PublisherCatalogue {
public:
    explicit PublisherCatalogue(const std::filesystem::path& path, PublisherCatalogueLimits limits = {});
    ~PublisherCatalogue();
    PublisherCatalogue(const PublisherCatalogue&) = delete;
    PublisherCatalogue& operator=(const PublisherCatalogue&) = delete;
    void discover(const VerifiedPublisherFeed& feed);
    VerifiedPublisherFeed get(const PublisherFeedId& id, const PublisherKey& host_pinned_key);
    PublisherFeedPage feeds(std::optional<PublisherFeedId> after = {}, std::size_t limit = 64);
    bool chain_complete(const PublisherFeedId& id, const PublisherKey& host_pinned_key);

    // Host-only decision, separate from signature validation and publication.
    // Admission checks the exact manifest identity/name/revision, lazily.
    // Declared roots/dependencies are checked when retrieved, not certified here.
    // Same-name/revision forks can coexist under distinct local aliases.
    MountState accept_local(std::string_view alias, const PublisherFeedId& feed, std::size_t offer,
                            const PublisherKey& host_pinned_key, const std::filesystem::path& directory,
                            std::uint64_t expected_generation);
    MountState accept_remote(std::string_view alias, const PublisherFeedId& feed, std::size_t offer,
                             const PublisherKey& host_pinned_key, MountBackend backend,
                             std::string_view host_source_key, std::shared_ptr<BlockSource> admission_source,
                             std::uint64_t expected_generation);
    MountState unmount(std::string_view alias, std::uint64_t expected_generation);
    MountState resolve(std::string_view alias);
    CatalogueAcceptancePage history(std::string_view alias, std::uint64_t after = 0, std::size_t limit = 64);
    // Compose with existing registry mounts and pass to DriveView. Duplicate
    // aliases or aggregate mount overflow must fail, never hide a local source.
    std::vector<MountState> mounts();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace axiom::knowledge
