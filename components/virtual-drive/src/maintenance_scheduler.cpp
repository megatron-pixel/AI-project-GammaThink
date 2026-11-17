#include "maintenance_scheduler.hpp"

#include <thread>

namespace axiom::knowledge {
namespace {
void require(bool value, ErrorCode code, const char* message) {
    if (!value) throw Error(code, message);
}
}
struct MaintenanceScheduler::Impl {
    struct Feed {
        std::string job, target, channel;
        PublisherKey key;
        std::unique_ptr<ReadHttp> transport;
        std::chrono::seconds interval;
        Clock::time_point due;
    };
    struct Cache {
        std::string job;
        std::shared_ptr<BlockCache> cache;
        CachePruneLimits limits;
        std::optional<BlockId> after;
        std::chrono::seconds interval;
        Clock::time_point due;
    };
    PublisherCatalogue& catalogue;
    neural::NativeModelManager& model;
    std::shared_ptr<std::atomic<bool>> network = std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<ReadHttpBudget> budget;
    Clock::time_point last;
    std::thread::id owner = std::this_thread::get_id();
    std::vector<Feed> feeds;
    std::vector<Cache> caches;
    std::size_t next_feed = 0, next_cache = 0;
    Impl(PublisherCatalogue& source, neural::NativeModelManager& manager,
         ReadHttpBudgetLimits limits, Clock::time_point start)
        : catalogue(source), model(manager), budget(std::make_shared<ReadHttpBudget>(limits)), last(start) {}
    void check_owner() const {
        require(std::this_thread::get_id() == owner, ErrorCode::Permission, "Maintenance requires its owner thread");
    }
    void job(std::string_view name, std::chrono::seconds interval) const {
        check_owner(); validate_text(name, 64);
        require(!name.empty(), ErrorCode::Schema, "Missing maintenance job name");
        require(interval >= std::chrono::seconds(1) && interval <= std::chrono::hours(24 * 7),
            ErrorCode::Budget, "Invalid maintenance interval");
        require(feeds.size() + caches.size() < 16, ErrorCode::Budget, "Maintenance job quota exceeded");
        for (const auto& feed : feeds) require(feed.job != name, ErrorCode::Schema, "Duplicate maintenance job");
        for (const auto& cache : caches) require(cache.job != name, ErrorCode::Schema, "Duplicate maintenance job");
    }
};
MaintenanceScheduler::MaintenanceScheduler(PublisherCatalogue& catalogue, neural::NativeModelManager& model,
    ReadHttpBudgetLimits network, Clock::time_point start) : impl_(std::make_unique<Impl>(catalogue, model, network, start)) {}
MaintenanceScheduler::~MaintenanceScheduler() { revoke_network(); }
void MaintenanceScheduler::add_feed(std::string job, ReadHttpOptions options, std::string target,
    PublisherKey key, std::string channel, std::chrono::seconds interval) {
    impl_->job(job, interval);
    validate_text(target, 2048); validate_text(channel, 128);
    require(!target.empty() && target.front() == '/' && !channel.empty(), ErrorCode::Schema, "Invalid feed watch target or channel");
    require(!options.network_gate && !options.budget, ErrorCode::Schema, "Scheduler owns feed transport authority");
    options.network_gate = impl_->network; options.budget = impl_->budget;
    auto transport = std::make_unique<ReadHttp>(std::move(options));
    impl_->feeds.push_back({std::move(job), std::move(target), std::move(channel), key,
        std::move(transport), interval, impl_->last});
}
void MaintenanceScheduler::add_cache(std::string job, std::shared_ptr<BlockCache> cache, CachePruneLimits limits,
    std::chrono::seconds interval) {
    impl_->job(job, interval);
    require(bool(cache), ErrorCode::Permission, "Maintenance requires a host cache capability");
    require(limits.examined > 0 && limits.examined <= 4096 && limits.candidates > 0 && limits.candidates <= limits.examined,
        ErrorCode::Budget, "Invalid maintenance pruning limits");
    impl_->caches.push_back({std::move(job), std::move(cache), limits, {}, interval, impl_->last});
}
void MaintenanceScheduler::set_network(bool enabled) {
    impl_->check_owner(); impl_->network->store(enabled);
}
void MaintenanceScheduler::revoke_network() noexcept { impl_->network->store(false); }
ReadHttpStats MaintenanceScheduler::network_stats() const {
    impl_->check_owner(); return impl_->budget->stats();
}
std::optional<MaintenanceScheduler::Clock::time_point> MaintenanceScheduler::next_due(bool active) const {
    impl_->check_owner();
    if (active || impl_->model.state() != neural::ModelState::Unloaded) return {};
    std::optional<Clock::time_point> result;
    const auto consider = [&](Clock::time_point due) {
        if (!result || due < *result) result = due;
    };
    if (impl_->network->load()) for (const auto& feed : impl_->feeds) consider(feed.due);
    for (const auto& cache : impl_->caches) consider(cache.due);
    if (result && *result < impl_->last) return impl_->last;
    return result;
}
MaintenanceTick MaintenanceScheduler::tick(Clock::time_point now, bool active) {
    impl_->check_owner();
    require(now >= impl_->last, ErrorCode::Schema, "Maintenance clock moved backwards");
    impl_->last = now;
    MaintenanceTick tick;
    if (active || impl_->model.state() != neural::ModelState::Unloaded) {
        tick.busy = true; return tick;
    }
    if (impl_->network->load()) for (std::size_t i = 0; i < impl_->feeds.size(); ++i) {
        const auto index = (impl_->next_feed + i) % impl_->feeds.size();
        auto& feed = impl_->feeds[index];
        if (now < feed.due) continue;
        feed.due = now + feed.interval;
        impl_->next_feed = (index + 1) % impl_->feeds.size();
        MaintenanceResult result; result.job = feed.job;
        try {
            const auto wire = feed.transport->raw_get(feed.target, 1024 * 1024);
            const auto verified = verify_publisher_feed(wire, feed.key);
            require(verified.content().channel == feed.channel, ErrorCode::Integrity, "Watched publisher channel differs");
            impl_->catalogue.discover(verified);
            result.discovered = verified.id();
        } catch (const Error& error) {
            result.error = error.code; result.reason = error.what();
        }
        tick.results.push_back(std::move(result));
        break;
    }
    for (std::size_t i = 0; i < impl_->caches.size(); ++i) {
        const auto index = (impl_->next_cache + i) % impl_->caches.size();
        auto& cache = impl_->caches[index];
        if (now < cache.due) continue;
        cache.due = now + cache.interval;
        impl_->next_cache = (index + 1) % impl_->caches.size();
        MaintenanceResult result; result.job = cache.job;
        try {
            result.cache = cache.cache->prune(cache.after, cache.limits);
            cache.after = result.cache->next;
        } catch (const Error& error) {
            result.error = error.code; result.reason = error.what();
        }
        tick.results.push_back(std::move(result));
        break;
    }
    return tick;
}

} // namespace axiom::knowledge
