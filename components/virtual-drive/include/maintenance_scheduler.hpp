#pragma once

#include "block_cache.hpp"
#include "model_manager.hpp"
#include "publisher_catalogue.hpp"
#include "read_http.hpp"

namespace axiom::knowledge {

struct MaintenanceResult {
    std::string job;
    std::optional<ErrorCode> error;
    std::string reason;
    std::optional<PublisherFeedId> discovered;
    std::optional<CachePrunePage> cache;
};
struct MaintenanceTick {
    bool busy = false;
    std::vector<MaintenanceResult> results;
};

// Host-driven owner-thread scheduler. A tick performs at most one bounded feed
// GET and one cache page; no sleeping, background thread, model calls, automatic
// acceptance/publication or budget replenishment. The host's event loop supplies
// monotonic ticks and its actual interactive-task state.
class MaintenanceScheduler {
public:
    using Clock = std::chrono::steady_clock;
    MaintenanceScheduler(PublisherCatalogue& catalogue, neural::NativeModelManager& model,
                         ReadHttpBudgetLimits network = {}, Clock::time_point start = Clock::now());
    ~MaintenanceScheduler();
    void add_feed(std::string job, ReadHttpOptions host_transport, std::string host_target,
                  PublisherKey host_pinned_key, std::string channel, std::chrono::seconds interval);
    // Cache pruning is dry-run unless the host explicitly configures apply.
    void add_cache(std::string job, std::shared_ptr<BlockCache> cache, CachePruneLimits limits,
                   std::chrono::seconds interval);
    void set_network(bool enabled);
    void revoke_network() noexcept;
    MaintenanceTick tick(Clock::time_point now, bool interactive_task_active);
    // Arm one host timer, or wait for an idle/network state-change event.
    std::optional<Clock::time_point> next_due(bool interactive_task_active) const;
    ReadHttpStats network_stats() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace axiom::knowledge
