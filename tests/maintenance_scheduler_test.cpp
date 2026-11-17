#include "maintenance_scheduler.hpp"
#include "loopback_http_fixture.hpp"

#include <iostream>
#include <map>
#include <unistd.h>

namespace {
using namespace axiom::knowledge;
using namespace axiom::test;
std::size_t assertions = 0;
void check(bool value) {
    ++assertions;
    if (!value) throw std::runtime_error("Maintenance scheduler assertion " + std::to_string(assertions));
}
template<class Function> void rejects(ErrorCode expected, Function function) {
    try { function(); }
    catch (const Error& error) { check(error.code == expected); return; }
    throw std::runtime_error("Expected maintenance scheduler rejection");
}
struct Directory {
    std::filesystem::path path;
    Directory() {
        auto pattern = (std::filesystem::temp_directory_path() / "gammathink-schedule-XXXXXX").string();
        const auto* made = mkdtemp(pattern.data());
        if (!made) throw std::runtime_error("Cannot create scheduler fixture");
        path = made;
    }
    ~Directory() { std::filesystem::remove_all(path); }
};
class Blocks final : public BlockSource {
public:
    std::map<BlockId, Bytes> blocks;
    std::size_t reads = 0;
    Bytes read(const BlockId& id, std::size_t maximum) override {
        ++reads;
        const auto& value = blocks.at(id);
        if (value.size() > maximum) throw Error(ErrorCode::Budget, "Synthetic source allowance");
        return value;
    }
};
}
int main() {
    using namespace std::chrono_literals;
    try {
        Directory directory;
        const auto seed = parse_publisher_key("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60");
        const auto key = publisher_key_from_seed(seed);
        const auto initial_wire = sign_publisher_feed({"stable", 1, {}, {}}, seed);
        const auto initial = verify_publisher_feed(initial_wire, key);
        const auto update_wire = sign_publisher_feed({"stable", 2, initial.id(), {}}, seed);
        const auto update = verify_publisher_feed(update_wire, key);
        auto invalid_wire = update_wire;
        invalid_wire[invalid_wire.size() - 3] = invalid_wire[invalid_wire.size() - 3] == '0' ? '1' : '0';
        std::atomic<int> mode = 0;
        HttpFixture server([&](const HttpRequest& request) {
            if (request.method != "GET" || request.target != "/publisher/stable.json")
                throw std::runtime_error("Unexpected scheduled disclosure");
            switch (mode.load()) {
            case 0: return HttpResponse{initial_wire};
            case 1: return HttpResponse{update_wire};
            case 2: return HttpResponse{invalid_wire};
            default: return HttpResponse{{}, 503};
            }
        });
        auto upstream = std::make_shared<Blocks>();
        for (std::uint8_t value = 1; value <= 3; ++value) {
            Bytes bytes(1024, value); upstream->blocks.emplace(BlockId{digest(bytes)}, std::move(bytes));
        }
        auto cache = std::make_shared<BlockCache>(directory.path / "cache", upstream);
        std::vector<BlockId> blocks;
        for (const auto& [id, bytes] : upstream->blocks) { blocks.push_back(id); cache->read(id, bytes.size()); }
        cache->pin(std::span(blocks).first(1));
        auto lease = cache->lease(std::span(blocks).subspan(1, 1));
        const auto source_reads = upstream->reads;
        PublisherCatalogue catalogue(directory.path / "catalogue.db");
        axiom::neural::NativeModelManager model;
        const auto start = MaintenanceScheduler::Clock::time_point{};
        MaintenanceScheduler scheduler(catalogue, model, {4, 1024 * 1024, 0}, start);
        ReadHttpOptions options; options.origin = server.origin();
        options.network = true; options.allow_insecure_loopback = true;
        scheduler.add_feed("updates", options, "/publisher/stable.json", key, "stable", 10s);
        CachePruneLimits prune; prune.apply = true; prune.examined = 1; prune.candidates = 1;
        scheduler.add_cache("cleanup", cache, prune, 60s);
        check(scheduler.next_due(false) == start && !scheduler.next_due(true));
        rejects(ErrorCode::Schema, [&] { scheduler.add_cache("updates", cache, prune, 60s); });
        rejects(ErrorCode::Budget, [&] { scheduler.add_cache("invalid", cache, prune, 0s); });
        auto invalid_limits = prune; invalid_limits.candidates = 2;
        rejects(ErrorCode::Budget, [&] { scheduler.add_cache("invalid", cache, invalid_limits, 60s); });
        check(scheduler.tick(start, true).busy);
        check(server.requests().empty() && cache->stats().ssd_entries == 3);
        const auto offline = scheduler.tick(start, false);
        check(!offline.busy && offline.results.size() == 1 && offline.results.front().cache->examined == 1);
        check(server.requests().empty() && catalogue.feeds().feeds.empty());
        scheduler.set_network(true);
        const auto first = scheduler.tick(start, false);
        check(first.results.size() == 1 && first.results.front().discovered == initial.id());
        check(catalogue.mounts().empty() && catalogue.feeds().feeds.size() == 1);
        check(scheduler.tick(start, false).results.empty());
        check(scheduler.tick(start + 9s, false).results.empty());
        check(scheduler.next_due(false) == start + 10s);
        mode.store(1);
        const auto next = scheduler.tick(start + 10s, false);
        check(next.results.size() == 1 && next.results.front().discovered == update.id());
        check(catalogue.chain_complete(update.id(), key) && catalogue.mounts().empty());
        mode.store(2);
        const auto invalid = scheduler.tick(start + 20s, false);
        check(invalid.results.size() == 1 && invalid.results.front().error == ErrorCode::Integrity);
        check(!invalid.results.front().discovered && catalogue.feeds().feeds.size() == 2);
        check(scheduler.tick(start + 20s, false).results.empty()); // No immediate failed-job retry.
        axiom::neural::ModelSpec absent{"synthetic-missing", directory.path / "absent.gguf", digest(Bytes{1})};
        rejects(ErrorCode::Storage, [&] { model.load(absent); });
        check(model.state() == axiom::neural::ModelState::Failed);
        check(!scheduler.next_due(false));
        check(scheduler.tick(start + 30s, false).busy && server.requests().size() == 3);
        model.unload();
        mode.store(3);
        const auto unavailable = scheduler.tick(start + 30s, false);
        check(unavailable.results.front().error == ErrorCode::Unavailable && server.requests().size() == 4);
        const auto exhausted = scheduler.tick(start + 40s, false);
        check(exhausted.results.front().error == ErrorCode::Budget && server.requests().size() == 4);
        scheduler.revoke_network();
        check(scheduler.tick(start + 50s, false).results.empty());
        const auto middle = scheduler.tick(start + 60s, false);
        const auto last = scheduler.tick(start + 120s, false);
        check(middle.results.front().cache->examined == 1 && last.results.front().cache->examined == 1);
        check(cache->stats().ssd_entries == 2 && upstream->reads == source_reads);
        check(cache->pinned() == std::vector<BlockId>{blocks.front()} && lease.active());
        lease.release();
        scheduler.tick(start + 180s, false);
        scheduler.tick(start + 240s, false);
        check(cache->stats().ssd_entries == 1 && cache->pinned().size() == 1);
        rejects(ErrorCode::Schema, [&] { scheduler.tick(start + 239s, false); });
        check(scheduler.network_stats().requests == 4 && catalogue.mounts().empty());
        check(model.state() == axiom::neural::ModelState::Unloaded);
        server.finish();
        std::cout << "Maintenance scheduling passed: " << assertions << " deterministic assertions\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
