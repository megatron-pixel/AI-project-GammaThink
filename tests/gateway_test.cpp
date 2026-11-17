#include "gateway.hpp"
#include "block_cache.hpp"
#include "block_cid.hpp"
#include "virtual_drive.hpp"

#include <arpa/inet.h>
#include <atomic>
#include <exception>
#include <iostream>
#include <map>
#include <mutex>
#include <netinet/in.h>
#include <set>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace {
using namespace axiom::knowledge;
std::size_t assertions = 0;
void check(bool condition) {
    ++assertions;
    if (!condition) throw std::runtime_error("Gateway assertion " + std::to_string(assertions));
}
template<class Function> void rejects(ErrorCode expected, Function function) {
    try { function(); }
    catch (const Error& error) { check(error.code == expected); return; }
    throw std::runtime_error("Expected gateway rejection");
}
struct Directory {
    std::filesystem::path path;
    Directory() {
        auto pattern = (std::filesystem::temp_directory_path() / "gammathink-gateway-XXXXXX").string();
        std::vector<char> value(pattern.begin(), pattern.end()); value.push_back(0);
        const auto* made = mkdtemp(value.data());
        if (!made) throw std::runtime_error("Cannot create gateway fixture");
        path = made;
    }
    ~Directory() { std::filesystem::remove_all(path); }
};
struct Socket {
    int fd;
    explicit Socket(int value) : fd(value) {}
    ~Socket() { if (fd >= 0) close(fd); }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
};
void require(bool condition) {
    if (!condition) throw std::runtime_error("Loopback gateway fixture failure");
}
void send_bytes(int fd, std::span<const std::uint8_t> bytes) {
    std::size_t offset = 0;
    int flags = 0;
#ifdef MSG_NOSIGNAL
    flags = MSG_NOSIGNAL;
#endif
    while (offset < bytes.size()) {
        const auto size = send(fd, bytes.data() + offset, bytes.size() - offset, flags);
        if (size <= 0) return; // Expected when a quota-aborting client disconnects.
        offset += static_cast<std::size_t>(size);
    }
}
class Server {
public:
    enum class Mode { Normal, Corrupt, Missing, Redirect, DeclaredHuge, NoLength, HugeHeader };
    explicit Server(std::map<std::string, Bytes> blocks)
        : blocks_(std::move(blocks)), listener_(socket(AF_INET, SOCK_STREAM, 0)) {
        require(listener_.fd >= 0);
        sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        require(bind(listener_.fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
        require(listen(listener_.fd, 16) == 0);
        socklen_t length = sizeof(address);
        require(getsockname(listener_.fd, reinterpret_cast<sockaddr*>(&address), &length) == 0);
        port_ = ntohs(address.sin_port);
        thread_ = std::thread([this] {
            try { run(); } catch (...) { std::lock_guard lock(mutex_); failure_ = std::current_exception(); }
        });
    }
    ~Server() {
        stopping_ = true;
        // An explicit connection wakes accept; no sleep/poll or cross-thread
        // close assumption is needed on either Darwin or Linux.
        Socket wake(socket(AF_INET, SOCK_STREAM, 0));
        sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(port_);
        if (wake.fd < 0 || connect(wake.fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
            std::terminate();
        if (thread_.joinable()) thread_.join();
    }
    std::string origin() const { return "http://127.0.0.1:" + std::to_string(port_); }
    void mode(Mode value) { mode_ = value; }
    std::vector<std::string> requests() {
        std::lock_guard lock(mutex_);
        if (failure_) std::rethrow_exception(failure_);
        return requests_;
    }
private:
    std::map<std::string, Bytes> blocks_;
    Socket listener_;
    std::uint16_t port_ = 0;
    std::atomic<bool> stopping_{false};
    std::atomic<Mode> mode_{Mode::Normal};
    std::mutex mutex_;
    std::vector<std::string> requests_;
    std::exception_ptr failure_;
    std::thread thread_;
    void run() {
        for (;;) {
            Socket client(accept(listener_.fd, nullptr, nullptr));
            require(client.fd >= 0);
            if (stopping_) return;
            const timeval timeout{5, 0};
            require(setsockopt(client.fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
            require(setsockopt(client.fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == 0);
#ifdef SO_NOSIGPIPE
            const int enabled = 1;
            require(setsockopt(client.fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) == 0);
#endif
            std::string request;
            while (request.find("\r\n\r\n") == std::string::npos) {
                char buffer[1024];
                const auto size = recv(client.fd, buffer, sizeof(buffer), 0);
                require(size > 0 && request.size() + static_cast<std::size_t>(size) <= 8192);
                request.append(buffer, static_cast<std::size_t>(size));
            }
            require(request.starts_with("GET /ipfs/"));
            require(request.find("?format=raw HTTP/") == 69);
            const auto cid = request.substr(10, 59);
            parse_block_cid(cid);
            require(request.find("Accept: application/vnd.ipld.raw\r\n") != std::string::npos);
            require(request.find("Authorization:") == std::string::npos && request.find("Cookie:") == std::string::npos);
            {
                std::lock_guard lock(mutex_); requests_.push_back(cid);
            }
            const auto found = blocks_.find(cid);
            Bytes body = found == blocks_.end() ? Bytes{} : found->second;
            const auto mode = mode_.load();
            std::string status = "200 OK";
            std::string extra;
            if (mode == Mode::Missing || found == blocks_.end()) { status = "404 Not Found"; body = {'n','o'}; }
            if (mode == Mode::Corrupt && !body.empty()) body.front() ^= 1;
            if (mode == Mode::Redirect) {
                status = "302 Found";
                extra = "Location: " + origin() + "/ipfs/" + cid + "?format=raw\r\n";
                body.clear();
            }
            if (mode == Mode::HugeHeader) extra += "X-Large: " + std::string(70000, 'x') + "\r\n";
            auto headers = "HTTP/1.1 " + status + "\r\nContent-Type: application/vnd.ipld.raw\r\nConnection: close\r\n" + extra;
            if (mode != Mode::NoLength) headers += "Content-Length: "
                + (mode == Mode::DeclaredHuge ? std::string("1000000000") : std::to_string(body.size())) + "\r\n";
            headers += "\r\n";
            send_bytes(client.fd, {reinterpret_cast<const std::uint8_t*>(headers.data()), headers.size()});
            if (mode != Mode::DeclaredHuge) send_bytes(client.fd, body);
        }
    }
};
class ForgedSource final : public BlockSource {
public:
    Bytes value;
    Bytes read(const BlockId&, std::size_t) override { return value; }
};
}
int main() {
    try {
        Directory directory;
        Journal journal(":memory:"), overlay(":memory:");
        std::vector<ObjectId> ids;
        for (unsigned i = 0; i < 24; ++i) {
            Object object; object.label = "gateway document " + std::to_string(i);
            object.format = "text/plain; charset=utf-8"; object.concepts = {i % 2 ? "heat" : "cold"};
            ids.push_back(journal.put(object));
        }
        SnapshotLimits limits; limits.chunk_records = 2; limits.chunk_bytes = 1024;
        const auto path = directory.path / "snapshot";
        const auto compiled = compile_snapshot(journal, path, "remote", "r1", limits);
        GksSnapshot local(path, compiled.root);
        std::map<std::string, Bytes> blocks;
        std::set<std::string> object_chunks;
        for (const auto& file : std::filesystem::directory_iterator(path)) {
            if (file.path().extension() != ".gksb") continue;
            const auto id = parse_block_id(file.path().stem().string());
            auto wire = local.block(id);
            const auto cid = cid_text(block_cid(id));
            if (wire[6] == 1) object_chunks.insert(cid);
            blocks.emplace(cid, std::move(wire));
        }
        const auto root_size = local.block(compiled.root).size();
        const auto block_count = blocks.size();
        Server server(std::move(blocks));
        GatewayOptions options; options.origin = server.origin(); options.allow_insecure_loopback = true;
        auto disabled = std::make_shared<GatewayBlocks>(options);
        rejects(ErrorCode::Unavailable, [&] { disabled->read(compiled.root, maximum_cid_block_bytes); });
        check(disabled->stats().requests == 0 && server.requests().empty());
        options.network = true;
        auto source = std::make_shared<GatewayBlocks>(options);
        auto cache = std::make_shared<BlockCache>(directory.path / "cache", source);
        GksArchive remote(cache, compiled.root);
        check(source->stats().requests == 1 && server.requests() == std::vector<std::string>{cid_text(block_cid(compiled.root))});
        KnowledgeQuery query; query.kind = QueryKind::Concept; query.value = "heat";
        const auto page = remote.query(query, {}, 3);
        check(page.objects.size() == 3 && !page.complete);
        for (const auto& cid : server.requests()) check(!object_chunks.contains(cid));
        const auto fetched = remote.get(ids[13]);
        check(fetched && fetched->object == *journal.get(ids[13]));
        bool fetched_object = false;
        const auto requests = server.requests();
        for (const auto& cid : requests) fetched_object |= object_chunks.contains(cid);
        check(fetched_object && std::set<std::string>(requests.begin(), requests.end()).size() < block_count);
        JournalArchive journal_reader(overlay);
        UnifiedKnowledge knowledge({&journal_reader, &remote});
        check(knowledge.get(ids[13])->record.object == *journal.get(ids[13]));
        {
            ArchiveRegistry registry(directory.path / "gateway-registry.sqlite");
            registry.register_snapshot(cache, compiled.root);
            registry.mount_remote("network", "remote", "r1", MountBackend::RemoteBlocks, "gateway", cache, 0);
            DriveAccess access;
            access.blocks = [cache](std::string_view key) -> std::shared_ptr<BlockSource> {
                return key == "gateway" ? cache : nullptr;
            };
            VirtualDrive drive(overlay, registry, {}, access);
            auto view = drive.open_view(); auto simultaneous = drive.open_view();
            check(view->knowledge().get(ids[7])->record.object == *journal.get(ids[7]));
            check(simultaneous->knowledge().get(ids[7]).has_value());
            registry.unmount("network", 1);
            check(!drive.open_view()->knowledge().get(ids[7]) && view->knowledge().get(ids[7]).has_value());
        }
        const std::array<BlockId, 1> pinned_root{compiled.root};
        cache->pin(pinned_root);
        cache->set_offline(true);
        const auto offline_requests = source->stats().requests;
        check(knowledge.get(ids[13])->record.object == *journal.get(ids[13]));
        rejects(ErrorCode::Unavailable, [&] { cache->read(identify_block(Bytes{1}), maximum_cid_block_bytes); });
        check(source->stats().requests == offline_requests && cache->pinned() == std::vector<BlockId>{compiled.root});
        cache->set_offline(false);
        const auto before = source->stats().requests;
        server.mode(Server::Mode::Corrupt);
        rejects(ErrorCode::Integrity, [&] { source->read(compiled.root, maximum_cid_block_bytes); });
        check(source->stats().requests == before + 1);
        server.mode(Server::Mode::Missing);
        rejects(ErrorCode::Unavailable, [&] { source->read(compiled.root, maximum_cid_block_bytes); });
        server.mode(Server::Mode::Redirect);
        const auto redirect_before = server.requests().size();
        rejects(ErrorCode::Unavailable, [&] { source->read(compiled.root, maximum_cid_block_bytes); });
        check(server.requests().size() == redirect_before + 1);
        server.mode(Server::Mode::DeclaredHuge);
        rejects(ErrorCode::Budget, [&] { source->read(compiled.root, maximum_cid_block_bytes); });
        server.mode(Server::Mode::NoLength);
        rejects(ErrorCode::Budget, [&] { source->read(compiled.root, 16); });
        server.mode(Server::Mode::HugeHeader);
        rejects(ErrorCode::Budget, [&] { source->read(compiled.root, maximum_cid_block_bytes); });
        server.mode(Server::Mode::Normal);
        check(source->read(compiled.root, maximum_cid_block_bytes) == local.block(compiled.root));
        auto quota = options; quota.requests = 1;
        GatewayBlocks request_limited(quota);
        request_limited.read(compiled.root, maximum_cid_block_bytes);
        rejects(ErrorCode::Budget, [&] { request_limited.read(compiled.root, maximum_cid_block_bytes); });
        quota = options; quota.total_bytes = root_size;
        GatewayBlocks byte_limited(quota);
        byte_limited.read(compiled.root, maximum_cid_block_bytes);
        check(byte_limited.stats().accepted_bytes == root_size);
        rejects(ErrorCode::Budget, [&] { byte_limited.read(compiled.root, maximum_cid_block_bytes); });
        quota = options; quota.block_bytes = root_size - 1;
        GatewayBlocks block_limited(quota);
        rejects(ErrorCode::Budget, [&] { block_limited.read(compiled.root, maximum_cid_block_bytes); });
        const auto no_request = server.requests().size();
        quota = options; quota.allow_insecure_loopback = false;
        rejects(ErrorCode::Permission, [&] { GatewayBlocks rejected(quota); });
        quota = options; quota.origin = "http://example.com";
        rejects(ErrorCode::Permission, [&] { GatewayBlocks rejected(quota); });
        for (const auto* url : {"https://user:secret@example.com", "https://example.com/ipfs",
                "https://example.com?x=1", "https://example.com/#fragment"}) {
            quota = options; quota.origin = url;
            rejects(ErrorCode::Schema, [&] { GatewayBlocks rejected(quota); });
        }
        quota = options; quota.timeout_ms = 0;
        rejects(ErrorCode::Budget, [&] { GatewayBlocks rejected(quota); });
        check(server.requests().size() == no_request);
        rejects(ErrorCode::Schema, [&] { GksSnapshot rejected(std::shared_ptr<BlockSource>{}, compiled.root); });
        auto forged = std::make_shared<ForgedSource>(); forged->value = local.block(compiled.root);
        forged->value.front() ^= 1;
        rejects(ErrorCode::Integrity, [&] { GksSnapshot rejected(forged, compiled.root); });
        forged->value.resize(maximum_cid_block_bytes + 1);
        rejects(ErrorCode::Budget, [&] { GksSnapshot rejected(forged, compiled.root); });
        std::cout << "gateway tests passed: " << assertions << " deterministic assertions\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
