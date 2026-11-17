#include "drive_network.hpp"
#include "block_cid.hpp"
#include "loopback_http_fixture.hpp"

#include <filesystem>
#include <iostream>
#include <map>
#include <source_location>
#include <stdlib.h>
#include <unistd.h>

namespace {
using namespace axiom::knowledge;
using namespace axiom::test;
std::size_t checks = 0;
void check(bool condition, const std::source_location where = std::source_location::current()) {
    ++checks;
    if (!condition) throw std::runtime_error("Drive network assertion at " + std::to_string(where.line()));
}
template<class F> void rejects(ErrorCode code, F&& operation) {
    ++checks;
    try { operation(); }
    catch (const Error& error) { if (error.code == code) return; throw; }
    throw std::runtime_error("Expected drive network rejection");
}
struct Directory {
    std::filesystem::path path;
    Directory() {
        auto pattern = (std::filesystem::temp_directory_path() / "drive-network-XXXXXX").string();
        const auto made = mkdtemp(pattern.data());
        if (!made) throw std::runtime_error("Cannot create network fixture directory");
        path = made;
    }
    ~Directory() { std::filesystem::remove_all(path); }
};
GatewayOptions gateway(const HttpFixture& server, bool permitted = true) {
    GatewayOptions options;
    options.origin = server.origin();
    options.network = permitted;
    options.allow_insecure_loopback = true;
    return options;
}
RemoteQueryOptions query_options(const HttpFixture& server, bool disclose = false) {
    RemoteQueryOptions options;
    options.origin = server.origin();
    options.network = true;
    options.allow_insecure_loopback = true;
    options.allow_query_disclosure = disclose;
    return options;
}
}

int main() {
    try {
        Directory directory;
        Journal data(":memory:");
        Object object;
        object.label = "Synthetic network record";
        object.concepts = {"public-fixture"};
        object.format = "text/plain";
        object.payload = {'d', 'a', 't', 'a'};
        const auto id = data.put(object);
        const auto snapshot_path = directory.path / "snapshot";
        const auto snapshot = compile_snapshot(data, snapshot_path, "network", "1");
        GksSnapshot stored(snapshot_path, snapshot.root);
        std::map<std::string, Bytes> blocks;
        for (const auto& file : std::filesystem::directory_iterator(snapshot_path)) {
            if (file.path().extension() != ".gksb") continue;
            const auto block = parse_block_id(file.path().stem().string());
            blocks.emplace("/ipfs/" + cid_text(block_cid(block)) + "?format=raw", stored.block(block));
        }
        HttpFixture server([&](const HttpRequest& request) {
            if (request.method == "GET") {
                const auto found = blocks.find(request.target);
                return found == blocks.end() ? HttpResponse{{'n', 'o'}, 404} : HttpResponse{found->second};
            }
            if (request.method != "POST" || request.target != "/gammathink/query/v1") {
                throw std::runtime_error("Unexpected fixture query route");
            }
            const auto decoded = decode_remote_query(request.body);
            if (decoded.root != snapshot.root) throw std::runtime_error("Unexpected fixture query root");
            GksArchive verifier(snapshot_path, snapshot.root);
            const auto page = verifier.query(decoded.query, decoded.after, decoded.limit);
            return HttpResponse{encode_remote_query_page({identify_block(request.body).digest, page})};
        });
        {
            DriveNetwork policy(directory.path / "cache");
            check(!policy.network_enabled());
            policy.add("library", gateway(server), query_options(server));
            const auto capabilities = policy.access();
            const auto source = capabilities.blocks("library");
            rejects(ErrorCode::Unavailable, [&] { source->read(snapshot.root, 2 * 1024 * 1024); });
            check(policy.network_stats().requests == 0 && server.requests().empty());
            rejects(ErrorCode::Permission, [&] { capabilities.blocks("https://not-a-capability"); });
            rejects(ErrorCode::Schema, [&] { policy.add("../escape", gateway(server)); });
            rejects(ErrorCode::Schema, [&] { policy.add("library", gateway(server)); });
            rejects(ErrorCode::Permission, [&] { capabilities.queries("library"); });
            policy.set_network(true);
            policy.begin_retrieval();
            Journal overlay(":memory:");
            ArchiveRegistry registry(directory.path / "catalogue.db");
            registry.register_snapshot(source, snapshot.root);
            registry.mount_remote("library", "network", "1", MountBackend::RemoteBlocks,
                "library", source, 0);
            VirtualDrive drive(overlay, registry, {}, capabilities);
            auto private_view = drive.open_view();
            KnowledgeQuery all;
            const auto first = private_view->knowledge().query(all);
            check(first.complete && first.objects == std::vector<ObjectId>{id});
            check(private_view->knowledge().get(id)->record.object.payload == object.payload);
            check(policy.network_stats().requests > 0 && policy.network_stats().query_bytes == 0);
            for (const auto& request : server.requests()) check(request.method == "GET");

            policy.add("sharing", gateway(server), query_options(server, true));
            const auto sharing = capabilities.blocks("sharing");
            registry.mount_remote("sharing", "network", "1", MountBackend::RemoteQuery,
                "sharing", sharing, 0);
            auto online = drive.open_view();
            check(online->knowledge().query(all).objects == std::vector<ObjectId>{id});
            check(policy.network_stats().query_bytes > 0);
            const auto requests_before_offline = policy.network_stats().requests;
            policy.set_network(false);
            rejects(ErrorCode::Unavailable, [&] { online->knowledge().query(all); });
            check(policy.network_stats().requests == requests_before_offline);
            rejects(ErrorCode::Unavailable, [&] { drive.open_view(); });
            // Choose cache-only block/index evaluation explicitly. A remote
            // query binding never silently downgrades its disclosure contract.
            registry.mount_remote("sharing", "network", "1", MountBackend::RemoteBlocks,
                "sharing", sharing, 1);
            auto offline = drive.open_view();
            check(offline->knowledge().query(all).objects == std::vector<ObjectId>{id});
            check(offline->knowledge().get(id)->record.object.payload == object.payload);
            check(policy.network_stats().requests == requests_before_offline);
            check(policy.cache_stats("library").ram_entries > 0);
            check(overlay.objects().objects.empty());

            policy.add("blocks-only", gateway(server));
            rejects(ErrorCode::Permission, [&] { capabilities.queries("blocks-only"); });
            auto foreign_authority = gateway(server);
            foreign_authority.network_gate = std::make_shared<std::atomic<bool>>(true);
            rejects(ErrorCode::Schema, [&] { policy.add("foreign", foreign_authority); });
        }
        {
            DriveNetworkLimits tiny;
            tiny.sources = 2;
            tiny.network.requests = 1;
            DriveNetwork policy(directory.path / "limited", tiny);
            policy.add("first", gateway(server));
            policy.add("second", gateway(server));
            rejects(ErrorCode::Budget, [&] { policy.add("third", gateway(server)); });
            policy.set_network(true);
            const auto access = policy.access();
            check(!access.blocks("first")->read(snapshot.root, 2 * 1024 * 1024).empty());
            const auto used = policy.network_stats().requests;
            rejects(ErrorCode::Budget, [&] { access.blocks("second")->read(snapshot.root, 2 * 1024 * 1024); });
            check(used == 1 && policy.network_stats().requests == 1);
            // A new capability view still uses the same source/network ledger.
            rejects(ErrorCode::Budget, [&] { policy.access().blocks("second")->read(snapshot.root, 2 * 1024 * 1024); });
        }
        {
            DriveNetwork policy(directory.path / "denied");
            policy.add("source", gateway(server, false));
            policy.set_network(true);
            rejects(ErrorCode::Unavailable, [&] {
                policy.access().blocks("source")->read(snapshot.root, 2 * 1024 * 1024);
            });
            check(policy.network_stats().requests == 0);
        }
        {
            DriveNetwork policy(directory.path / "active-retention");
            policy.add("source", gateway(server));
            policy.set_network(true);
            auto source = policy.access().blocks("source");
            ArchiveRegistry registry(directory.path / "retention-registry.db");
            registry.register_snapshot(source, snapshot.root);
            registry.mount_remote("retained", "network", "1", MountBackend::RemoteBlocks, "source", source, 0);
            source.reset();
            Journal overlay(":memory:");
            VirtualDrive drive(overlay, registry, {}, policy.access());
            auto view = drive.open_view();
            check(view->knowledge().get(id)->record.object.payload == object.payload);
            const auto retained = policy.cache_stats("source").leased_blocks;
            check(retained > 0);
            policy.set_network(false);
            const auto requests = policy.network_stats().requests;
            CachePruneLimits apply; apply.apply = true;
            check(policy.prune("source", {}, apply).evicted == 0);
            check(view->knowledge().get(id)->record.object.payload == object.payload);
            view.reset();
            check(policy.cache_stats("source").leased_blocks == 0);
            check(policy.prune("source", {}, apply).evicted == retained);
            check(policy.network_stats().requests == requests && overlay.objects().objects.empty());
        }
        RemoteQueryOptions held;
        {
            DriveNetwork owner(directory.path / "owner");
            owner.add("source", gateway(server), query_options(server, true));
            owner.set_network(true);
            held = owner.access().queries("source");
            check(held.network_gate->load());
        }
        check(!held.network_gate->load());
        ReadHttp orphan(held);
        rejects(ErrorCode::Unavailable, [&] { orphan.raw_get("/after-owner-exit", 16); });
        server.finish();
        std::cout << "drive network policy passed: " << checks << " deterministic assertions\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
