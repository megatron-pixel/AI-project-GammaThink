#include "remote_query.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <atomic>
#include <iostream>
#include <map>
#include <mutex>
#include <netinet/in.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace {
using namespace axiom::knowledge;
std::size_t assertions = 0;
void check(bool condition) {
    ++assertions;
    if (!condition) throw std::runtime_error("Remote query assertion " + std::to_string(assertions));
}
template<class Function> void rejects(ErrorCode expected, Function function) {
    try { function(); }
    catch (const Error& error) { check(error.code == expected); return; }
    throw std::runtime_error("Expected remote query rejection");
}
struct Directory {
    std::filesystem::path path;
    Directory() {
        auto pattern = (std::filesystem::temp_directory_path() / "gammathink-query-XXXXXX").string();
        std::vector<char> value(pattern.begin(), pattern.end()); value.push_back(0);
        const auto* made = mkdtemp(value.data());
        if (!made) throw std::runtime_error("Cannot create query fixture");
        path = made;
    }
    ~Directory() { std::filesystem::remove_all(path); }
};
struct Blocks final : BlockSource {
    std::map<BlockId, Bytes> values;
    Bytes read(const BlockId& id, std::size_t maximum) override {
        const auto found = values.find(id);
        if (found == values.end()) throw Error(ErrorCode::Unavailable, "Missing verification block");
        if (found->second.size() > maximum) throw Error(ErrorCode::Budget, "Fixture block read cap");
        return found->second;
    }
};
struct Socket {
    int fd;
    explicit Socket(int value) : fd(value) {}
    ~Socket() { if (fd >= 0) close(fd); }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
};
void require(bool condition) { if (!condition) throw std::runtime_error("Query server fixture failed"); }
void send_bytes(int fd, std::span<const std::uint8_t> bytes) {
    int flags = 0;
#ifdef MSG_NOSIGNAL
    flags = MSG_NOSIGNAL;
#endif
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto size = send(fd, bytes.data() + offset, bytes.size() - offset, flags);
        if (size <= 0) return;
        offset += static_cast<std::size_t>(size);
    }
}
class Server {
public:
    enum class Mode { Normal, WrongDigest, Omit, PrematureEnd, WrongIds, Trailing, Missing, Redirect, Huge };
    explicit Server(std::map<Bytes, ObjectPage> pages)
        : pages_(std::move(pages)), listener_(socket(AF_INET, SOCK_STREAM, 0)) {
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
        Socket wake(socket(AF_INET, SOCK_STREAM, 0));
        sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(port_);
        if (wake.fd < 0 || connect(wake.fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) std::terminate();
        if (thread_.joinable()) thread_.join();
    }
    std::string origin() const { return "http://127.0.0.1:" + std::to_string(port_); }
    void mode(Mode value) { mode_ = value; }
    std::vector<Bytes> requests() {
        std::lock_guard lock(mutex_); if (failure_) std::rethrow_exception(failure_); return requests_;
    }
private:
    std::map<Bytes, ObjectPage> pages_;
    Socket listener_;
    std::uint16_t port_ = 0;
    std::atomic<bool> stopping_{false};
    std::atomic<Mode> mode_{Mode::Normal};
    std::mutex mutex_;
    std::exception_ptr failure_;
    std::vector<Bytes> requests_;
    std::thread thread_;
    void run() {
        for (;;) {
            Socket client(accept(listener_.fd, nullptr, nullptr));
            require(client.fd >= 0); if (stopping_) return;
            const timeval timeout{5, 0};
            require(setsockopt(client.fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
            require(setsockopt(client.fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == 0);
#ifdef SO_NOSIGPIPE
            const int enabled = 1;
            require(setsockopt(client.fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) == 0);
#endif
            std::string input;
            while (input.find("\r\n\r\n") == std::string::npos) {
                char buffer[1024]; const auto size = recv(client.fd, buffer, sizeof(buffer), 0);
                require(size > 0 && input.size() + static_cast<std::size_t>(size) <= 8192);
                input.append(buffer, static_cast<std::size_t>(size));
            }
            const auto end = input.find("\r\n\r\n") + 4;
            const auto headers = input.substr(0, end);
            require(headers.starts_with("POST /gammathink/query/v1 HTTP/"));
            require(headers.find("Accept: application/vnd.gammathink.query-page\r\n") != std::string::npos);
            require(headers.find("Content-Type: application/vnd.gammathink.query-request\r\n") != std::string::npos);
            require(headers.find("Authorization:") == std::string::npos && headers.find("Cookie:") == std::string::npos);
            const auto at = headers.find("Content-Length: "); require(at != std::string::npos);
            const auto length = std::stoull(headers.substr(at + 16));
            require(length <= remote_query_request_bytes);
            Bytes request(input.begin() + static_cast<std::ptrdiff_t>(end), input.end());
            require(request.size() <= length);
            while (request.size() < length) {
                std::uint8_t buffer[1024];
                const auto size = recv(client.fd, buffer, std::min<std::size_t>(sizeof(buffer), length - request.size()), 0);
                require(size > 0); request.insert(request.end(), buffer, buffer + size);
            }
            decode_remote_query(request);
            const auto found = pages_.find(request); require(found != pages_.end());
            {
                std::lock_guard lock(mutex_); requests_.push_back(request);
            }
            RemoteQueryPage response{identify_block(request).digest, found->second};
            const auto mode = mode_.load();
            if (mode == Mode::WrongDigest) response.request_digest.front() ^= 1;
            if (mode == Mode::Omit && !response.page.objects.empty()) {
                response.page.objects.pop_back();
                response.page.complete = true; response.page.next.reset();
            }
            if (mode == Mode::PrematureEnd) { response.page.complete = true; response.page.next.reset(); }
            if (mode == Mode::WrongIds) { response.page.objects = {ObjectId{}}; response.page.complete = true; response.page.next.reset(); }
            auto body = encode_remote_query_page(response);
            if (mode == Mode::Trailing) body.push_back(0);
            std::string status = "200 OK", extra;
            if (mode == Mode::Missing) status = "404 Not Found";
            if (mode == Mode::Redirect) { status = "302 Found"; extra = "Location: " + origin() + "/gammathink/query/v1\r\n"; }
            const auto header = "HTTP/1.1 " + status + "\r\nContent-Type: application/vnd.gammathink.query-page\r\nConnection: close\r\n"
                + extra + "Content-Length: " + (mode == Mode::Huge ? std::string("1000000000") : std::to_string(body.size())) + "\r\n\r\n";
            send_bytes(client.fd, {reinterpret_cast<const std::uint8_t*>(header.data()), header.size()});
            if (mode != Mode::Huge) send_bytes(client.fd, body);
        }
    }
};
}
int main() {
    try {
        Directory directory; Journal journal(":memory:");
        std::vector<ObjectId> ids;
        for (unsigned i = 0; i < 18; ++i) {
            Object object; object.label = "alpha beta " + std::to_string(i); object.format = "text/plain";
            object.concepts = {i % 2 ? "heat" : "cold"};
            SourceReference source; source.archive_id = "library"; source.archive_version = "v1";
            source.entry_path = "entry"; source.extractor_version = "fixture"; object.sources = {source};
            if (!ids.empty()) object.links.push_back({Relationship::Requires, ids.front()});
            ids.push_back(journal.put(object));
        }
        SnapshotLimits limits; limits.chunk_records = 2; limits.chunk_bytes = 1024;
        const auto snapshot_path = directory.path / "snapshot";
        const auto root = compile_snapshot(journal, snapshot_path, "query", "r1", limits).root;
        auto blocks = std::make_shared<Blocks>(); GksSnapshot local(snapshot_path, root);
        for (const auto& file : std::filesystem::directory_iterator(snapshot_path)) {
            if (file.path().extension() != ".gksb") continue;
            const auto id = parse_block_id(file.path().stem().string());
            blocks->values.emplace(id, local.block(id));
        }
        JournalArchive expected(journal);
        std::vector<KnowledgeQuery> queries;
        for (const auto kind : {QueryKind::All,QueryKind::Kind,QueryKind::Concept,QueryKind::Text,QueryKind::Source,QueryKind::Related,QueryKind::RequiredBy}) {
            KnowledgeQuery query; query.kind = kind; query.target = ids.front(); query.relationship = Relationship::Requires;
            if (kind == QueryKind::Concept) query.value = "heat";
            if (kind == QueryKind::Text) query.value = "alpha beta";
            if (kind == QueryKind::Source) { query.value = "library"; query.source_version = "v1"; }
            queries.push_back(query);
        }
        std::map<Bytes, ObjectPage> pages;
        for (const auto& query : queries) {
            std::optional<ObjectId> after;
            do {
                auto page = expected.query(query, after, 2);
                const auto wire = encode_remote_query({root,query,after,2});
                const auto decoded = decode_remote_query(wire);
                check(encode_remote_query(decoded) == wire);
                pages.emplace(wire, page);
                if (page.complete) break;
                after = page.next;
            } while (true);
        }
        KnowledgeQuery all;
        const auto complete_wire = encode_remote_query({root,all,{},256});
        pages.emplace(complete_wire, expected.query(all, {}, 256));
        Server server(pages);
        RemoteQueryOptions options; options.origin = server.origin(); options.allow_insecure_loopback = true;
        {
            RemoteQueryArchive disabled(blocks, root, options);
            rejects(ErrorCode::Permission, [&] { disabled.query(all, {}, 2); });
            check(disabled.get(ids.front())->object == *journal.get(ids.front()));
            check(server.requests().empty());
        }
        options.allow_query_disclosure = true;
        {
            RemoteQueryArchive offline(blocks, root, options);
            rejects(ErrorCode::Unavailable, [&] { offline.query(all, {}, 2); });
            check(server.requests().empty());
        }
        options.network = true;
        RemoteQueryArchive remote(blocks, root, options);
        for (const auto& [wire,page] : pages) {
            const auto request = decode_remote_query(wire);
            const auto got = remote.query(request.query, request.after, request.limit);
            check(got.objects == page.objects && got.complete == page.complete && got.next == page.next);
        }
        UnifiedKnowledge knowledge({&remote});
        check(knowledge.get(ids[7])->record.object == *journal.get(ids[7]));
        check(knowledge.query(all, {}, 2).objects == expected.query(all, {}, 2).objects);
        const auto disclosure_count = server.requests().size();
        check(remote.stats().query_bytes > 0 && remote.stats().requests == disclosure_count);
        {
            Journal overlay(":memory:");
            const auto registry_path = directory.path / "query-registry.sqlite";
            {
                ArchiveRegistry registry(registry_path);
                registry.register_snapshot(blocks, root);
                registry.mount_remote("service", "query", "r1", MountBackend::RemoteQuery, "approved", blocks, 0);
            }
            ArchiveRegistry registry(registry_path);
            DriveAccess access;
            access.blocks = [blocks](std::string_view key) -> std::shared_ptr<BlockSource> {
                return key == "approved" ? blocks : nullptr;
            };
            VirtualDrive no_query_capability(overlay, registry, {}, access);
            rejects(ErrorCode::Permission, [&] { no_query_capability.open_view(); });
            access.queries = [options](std::string_view) { return options; };
            VirtualDrive drive(overlay, registry, {}, access);
            auto view = drive.open_view();
            check(view->knowledge().query(all, {}, 2).objects == expected.query(all, {}, 2).objects);
            auto private_options = options; private_options.allow_query_disclosure = false;
            access.queries = [private_options](std::string_view) { return private_options; };
            VirtualDrive private_drive(overlay, registry, {}, access);
            auto private_view = private_drive.open_view();
            const auto before = server.requests().size();
            rejects(ErrorCode::Permission, [&] { private_view->knowledge().query(all, {}, 2); });
            check(server.requests().size() == before);
            registry.unmount("service", 1);
            check(!drive.open_view()->knowledge().get(ids.front()) && view->knowledge().get(ids.front()).has_value());
        }
        for (const auto mode : {Server::Mode::WrongDigest,Server::Mode::Omit,Server::Mode::PrematureEnd,Server::Mode::WrongIds}) {
            server.mode(mode);
            rejects(ErrorCode::Integrity, [&] { remote.query(all, {}, 2); });
        }
        server.mode(Server::Mode::Omit);
        rejects(ErrorCode::Integrity, [&] { remote.query(all, {}, 256); }); // Valid-looking incomplete search.
        server.mode(Server::Mode::Trailing);
        rejects(ErrorCode::Schema, [&] { remote.query(all, {}, 2); });
        server.mode(Server::Mode::Missing);
        rejects(ErrorCode::Unavailable, [&] { remote.query(all, {}, 2); });
        server.mode(Server::Mode::Redirect);
        const auto redirects = server.requests().size();
        rejects(ErrorCode::Unavailable, [&] { remote.query(all, {}, 2); });
        check(server.requests().size() == redirects + 1);
        server.mode(Server::Mode::Huge);
        rejects(ErrorCode::Budget, [&] { remote.query(all, {}, 2); });
        server.mode(Server::Mode::Normal);
        {
            auto quota = options; quota.query_bytes = 1;
            RemoteQueryArchive capped(blocks, root, quota);
            const auto before = server.requests().size();
            rejects(ErrorCode::Budget, [&] { capped.query(all, {}, 2); });
            check(server.requests().size() == before);
            quota = options; quota.requests = 1;
            RemoteQueryArchive once(blocks, root, quota);
            once.query(all, {}, 2);
            rejects(ErrorCode::Budget, [&] { once.query(all, {}, 2); });
        }
        {
            auto unavailable = std::make_shared<Blocks>(); unavailable->values.emplace(root, local.block(root));
            RemoteQueryArchive denied(unavailable, root, options);
            rejects(ErrorCode::Unavailable, [&] { denied.query(all, {}, 2); });
        }
        auto wire = encode_remote_query({root, queries[4], ids.front(), 2});
        for (std::size_t n = 0; n < wire.size(); ++n) rejects(ErrorCode::Schema, [&] { decode_remote_query(std::span(wire).first(n)); });
        auto changed = wire; changed.push_back(0);
        rejects(ErrorCode::Schema, [&] { decode_remote_query(changed); });
        changed = wire; changed[40] = 2;
        rejects(ErrorCode::Schema, [&] { decode_remote_query(changed); });
        auto response_wire = encode_remote_query_page({identify_block(wire).digest, expected.query(all, {}, 2)});
        for (std::size_t n = 0; n < response_wire.size(); ++n) rejects(ErrorCode::Schema, [&] { decode_remote_query_page(std::span(response_wire).first(n)); });
        changed = response_wire; changed[40] = 2;
        rejects(ErrorCode::Schema, [&] { decode_remote_query_page(changed); });
        changed = response_wire; changed[41] = 1; changed[42] = 1;
        rejects(ErrorCode::Budget, [&] { decode_remote_query_page(changed); });
        changed = response_wire; std::copy_n(changed.begin() + 43, 32, changed.begin() + 75);
        rejects(ErrorCode::Schema, [&] { decode_remote_query_page(changed); });
        KnowledgeQuery huge; huge.kind = QueryKind::Source; huge.value.assign(maximum_text_bytes, 'a');
        huge.source_version = std::string(maximum_text_bytes, 'b');
        wire = encode_remote_query({root,huge,ids.front(),256});
        check(wire.size() <= remote_query_request_bytes && encode_remote_query(decode_remote_query(wire)) == wire);
        rejects(ErrorCode::Budget, [&] { encode_remote_query({root,all,{},0}); });
        std::cout << "remote query tests passed: " << assertions << " deterministic assertions\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
