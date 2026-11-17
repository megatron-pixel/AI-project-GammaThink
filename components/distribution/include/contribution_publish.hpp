#pragma once

#include "block_cid.hpp"
#include "contribution.hpp"
#include "publisher_feed.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

namespace axiom::knowledge {

struct PublicationLimits {
    std::size_t blocks = 4096;
    std::uint64_t payload_bytes = 64 * 1024 * 1024;
    std::uint64_t record_bytes = 64 * 1024 * 1024;
    std::chrono::milliseconds operation_time{30000};
};
struct PublicationPlan {
    BlockId root;
    std::vector<BlockId> blocks; // Manifest last; all others deterministically sorted.
    std::uint64_t payload_bytes = 0;
    bool operator==(const PublicationPlan&) const = default;
};
PublicationPlan inspect_publication(const PreparedContribution& contribution, PublicationLimits limits = {});
// Same content-only checks through a host-supplied read capability. Catalogue
// text cannot construct this capability or turn observed metadata into truth.
PublicationPlan inspect_contribution(std::shared_ptr<BlockSource> source, const BlockId& snapshot,
                                    std::span<const ObjectId> roots, std::span<const ObjectId> objects,
                                    PublicationLimits limits = {});

class ContributionPublisher;
class PublicationPermit {
public:
    PublicationPermit(PublicationPermit&&) noexcept;
    PublicationPermit& operator=(PublicationPermit&&) noexcept;
    PublicationPermit(const PublicationPermit&) = delete;
    PublicationPermit& operator=(const PublicationPermit&) = delete;
    void revoke() noexcept;
private:
    friend class ContributionPublisher;
    PublicationPermit(std::weak_ptr<std::atomic<bool>> owner, PreparedContribution contribution,
                      PublicationPlan plan);
    PublicationPermit(std::weak_ptr<std::atomic<bool>> owner, Bytes signed_feed, PublicationPlan plan);
    std::weak_ptr<std::atomic<bool>> owner_;
    std::shared_ptr<std::atomic<bool>> active_;
    PreparedContribution contribution_;
    PublicationPlan plan_;
    Bytes signed_feed_;
    bool used_ = false;
};
struct PublicationReceipt {
    BlockId root;
    std::vector<BlockCid> confirmed;
    std::size_t attempted_requests = 0;
    std::uint64_t confirmed_payload_bytes = 0, uploaded_http_body_bytes = 0;
    bool root_stored = false;
};

// Only a trusted host can call approve AFTER explicit user approval of this
// exact review artifact and this fixed loopback Kubo node. Public visibility,
// a model proposal, imported records and catalogue discovery cannot mint it.
// No network operation occurs in the constructor, inspection or approval.
// A permit is single-attempt, revocable and not serializable.
class ContributionPublisher {
public:
    explicit ContributionPublisher(std::string loopback_kubo_origin, PublicationLimits limits = {});
    ~ContributionPublisher();
    ContributionPublisher(const ContributionPublisher&) = delete;
    ContributionPublisher& operator=(const ContributionPublisher&) = delete;
    PublicationPermit approve(const PreparedContribution& contribution);
    // Separate explicit host approval of this exact signed metadata block.
    // Does not publish the snapshots it names or imply their availability.
    PublicationPermit approve_feed(const VerifiedPublisherFeed& feed);
    PublicationReceipt publish(PublicationPermit& permit);
    void revoke_all() noexcept;
    const PublicationReceipt& last_receipt() const noexcept { return last_; }
private:
    std::string endpoint_;
    PublicationLimits limits_;
    std::thread::id thread_ = std::this_thread::get_id();
    std::shared_ptr<std::atomic<bool>> active_ = std::make_shared<std::atomic<bool>>(true);
    PublicationReceipt last_;
};

} // namespace axiom::knowledge
