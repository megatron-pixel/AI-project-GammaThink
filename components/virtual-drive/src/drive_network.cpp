#include "drive_network.hpp"
#include "retained_source.hpp"

#include <algorithm>
#include <map>

namespace axiom::knowledge {
namespace {
void require(bool condition, ErrorCode code, const char* message) {
    if (!condition) throw Error(code, message);
}
}
struct DriveNetwork::State {
    struct Source {
        std::shared_ptr<BlockCache> cache;
        std::optional<RemoteQueryOptions> queries;
    };
    std::filesystem::path directory;
    DriveNetworkLimits limits;
    BlockCacheLimits per_source;
    std::shared_ptr<std::atomic<bool>> enabled = std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<ReadHttpBudget> budget;
    std::map<std::string, Source> sources;
    std::optional<RetrievalLimits> retrieval;
    State(std::filesystem::path path, DriveNetworkLimits policy)
        : directory(std::move(path)), limits(policy), per_source(policy.cache),
          budget(std::make_shared<ReadHttpBudget>(policy.network)) {
        require(limits.sources > 0 && limits.sources <= 16, ErrorCode::Budget, "Invalid network source capacity");
        per_source.ram_bytes /= limits.sources;
        per_source.ram_entries /= limits.sources;
        per_source.ssd_bytes /= limits.sources;
        per_source.ssd_entries /= limits.sources;
        per_source.lease_references /= limits.sources;
        require(per_source.ram_bytes > 0 && per_source.ram_entries > 0 &&
            per_source.ssd_bytes > 0 && per_source.ssd_entries > 0 && per_source.lease_references > 0,
            ErrorCode::Budget, "Aggregate cache limits cannot hold the source capacity");
        std::filesystem::create_directories(directory);
    }
    Source& source(std::string_view key) {
        const auto found = sources.find(std::string(key));
        require(found != sources.end(), ErrorCode::Permission, "Unregistered read-only source capability");
        return found->second;
    }
};

DriveNetwork::DriveNetwork(const std::filesystem::path& directory, DriveNetworkLimits limits)
    : state_(std::make_shared<State>(directory, limits)) {}
DriveNetwork::~DriveNetwork() { revoke_network(); }
void DriveNetwork::add(std::string key, GatewayOptions blocks, std::optional<RemoteQueryOptions> queries) {
    require(!key.empty() && key.size() <= 64, ErrorCode::Schema, "Invalid read-only source key");
    for (const auto value : key) {
        require((value >= 'a' && value <= 'z') || (value >= '0' && value <= '9') || value == '_' || value == '-',
            ErrorCode::Schema, "Invalid read-only source key");
    }
    require(!state_->sources.contains(key), ErrorCode::Schema, "Read-only source key already registered");
    require(state_->sources.size() < state_->limits.sources, ErrorCode::Budget, "Read-only source capacity exceeded");
    require(!blocks.network_gate && !blocks.budget &&
        (!queries || (!queries->network_gate && !queries->budget)),
        ErrorCode::Schema, "Network authority is owned by the source controller");
    blocks.network_gate = state_->enabled;
    blocks.budget = state_->budget;
    auto gateway = std::make_shared<GatewayBlocks>(blocks);
    auto cache = std::make_shared<BlockCache>(state_->directory / key, gateway, state_->per_source);
    cache->set_offline(!network_enabled());
    if (state_->retrieval) cache->begin_retrieval(*state_->retrieval);
    if (queries) {
        queries->network_gate = state_->enabled;
        queries->budget = state_->budget;
        // Validate the configured endpoint/bounds now, without making a request.
        ReadHttp validate(*queries);
    }
    state_->sources.emplace(std::move(key), State::Source{std::move(cache), std::move(queries)});
}
void DriveNetwork::set_network(bool enabled) {
    if (!enabled) revoke_network();
    for (auto& [key, source] : state_->sources) {
        (void)key;
        source.cache->set_offline(!enabled);
    }
    if (enabled) state_->enabled->store(true, std::memory_order_release);
}
void DriveNetwork::revoke_network() noexcept {
    state_->enabled->store(false, std::memory_order_release);
}
bool DriveNetwork::network_enabled() const noexcept {
    return state_->enabled->load(std::memory_order_acquire);
}
void DriveNetwork::begin_retrieval(RetrievalLimits limits) {
    for (auto& [key, source] : state_->sources) {
        (void)key;
        source.cache->begin_retrieval(limits);
    }
    state_->retrieval = limits;
}
DriveAccess DriveNetwork::access() const {
    const auto state = state_;
    DriveAccess result;
    result.blocks = [state](std::string_view key) -> std::shared_ptr<BlockSource> {
        return std::make_shared<RetainedBlockSource>(state->source(key).cache,
            std::min<std::size_t>(maximum_items, state->per_source.lease_references));
    };
    result.queries = [state](std::string_view key) {
        const auto& source = state->source(key);
        require(source.queries.has_value(), ErrorCode::Permission, "Query disclosure capability is not registered");
        require(source.queries->network && source.queries->allow_query_disclosure,
            ErrorCode::Permission, "Query disclosure capability is not approved");
        require(state->enabled->load(std::memory_order_acquire), ErrorCode::Unavailable,
            "Remote query transport is offline");
        return *source.queries;
    };
    return result;
}
ReadHttpStats DriveNetwork::network_stats() const { return state_->budget->stats(); }
BlockCacheStats DriveNetwork::cache_stats(std::string_view key) const { return state_->source(key).cache->stats(); }
RetrievalStats DriveNetwork::retrieval_stats(std::string_view key) const {
    return state_->source(key).cache->retrieval_stats();
}
void DriveNetwork::pin(std::string_view key, std::span<const BlockId> ids) { state_->source(key).cache->pin(ids); }
void DriveNetwork::unpin(std::string_view key, std::span<const BlockId> ids) { state_->source(key).cache->unpin(ids); }
CachePrunePage DriveNetwork::prune(std::string_view key, std::optional<BlockId> after, CachePruneLimits limits) {
    return state_->source(key).cache->prune(after, limits);
}

} // namespace axiom::knowledge
