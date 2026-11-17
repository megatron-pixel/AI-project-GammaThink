#include "contribution_publish.hpp"
#include "loopback_http_fixture.hpp"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <unistd.h>

namespace {
using namespace axiom::knowledge;
using namespace axiom::test;
std::size_t assertions = 0;
void check(bool value) {
    ++assertions;
    if (!value) throw std::runtime_error("Publication assertion " + std::to_string(assertions));
}
template<class Function> void rejects(ErrorCode expected, Function function) {
    try { function(); }
    catch (const Error& error) { check(error.code == expected); return; }
    throw std::runtime_error("Expected publication rejection");
}
struct Directory {
    std::filesystem::path path;
    Directory() {
        auto pattern = (std::filesystem::temp_directory_path() / "gammathink-publish-XXXXXX").string();
        std::vector<char> value(pattern.begin(), pattern.end()); value.push_back(0);
        const auto* made = mkdtemp(value.data());
        if (!made) throw std::runtime_error("Cannot create publication test directory");
        path = made;
    }
    ~Directory() { std::filesystem::remove_all(path); }
};
Object object(std::string label) {
    Object value; value.kind = ObjectKind::Claim; value.label = std::move(label);
    value.visibility = Visibility::Public; value.format = "synthetic.offline-publication-test.v1";
    value.payload = {0, 1, 2, 0, 255}; return value;
}
class SnapshotBlocks final : public BlockSource {
public:
    SnapshotBlocks(const std::filesystem::path& path, const BlockId& root) : snapshot_(path, root) {}
    Bytes read(const BlockId& id, std::size_t maximum) override {
        auto bytes = snapshot_.block(id);
        if (bytes.size() > maximum) throw Error(ErrorCode::Budget, "Test source read budget");
        return bytes;
    }
private:
    GksSnapshot snapshot_;
};
Bytes uploaded(const HttpRequest& request) {
    if (request.method != "POST" || request.target !=
        "/api/v0/block/put?cid-codec=raw&mhtype=sha2-256&mhlen=32&pin=true&allow-big-block=false")
        throw std::runtime_error("Unexpected publication request");
    const std::string_view body(reinterpret_cast<const char*>(request.body.data()), request.body.size());
    const auto start = body.find("\r\n\r\n"), end = body.rfind("\r\n--");
    if (start == std::string_view::npos || end == std::string_view::npos || end < start + 4)
        throw std::runtime_error("Invalid multipart publication");
    return {request.body.begin() + static_cast<std::ptrdiff_t>(start + 4),
            request.body.begin() + static_cast<std::ptrdiff_t>(end)};
}
HttpResponse ack(const HttpRequest& request) {
    const auto bytes = uploaded(request);
    const auto json = "{\"Key\":\"" + cid_text(block_cid(identify_block(bytes)))
        + "\",\"Size\":" + std::to_string(bytes.size()) + "}\n";
    return {{json.begin(), json.end()}, 200};
}
}
int main(int argc, char** argv) {
    try {
        Directory directory;
        Journal journal(directory.path / "journal.db");
        const auto premise = journal.put(object("synthetic source"));
        auto conclusion = object("synthetic conclusion");
        conclusion.links = {{Relationship::Requires, premise}};
        const auto root = journal.put(conclusion);
        const auto prepared = prepare_contribution(journal, std::array{root}, directory.path / "packet", "offline-qa", "r1");
        const auto seed = parse_publisher_key("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60");
        const PublisherFeed feed{"synthetic-offline-qa", 1, {},
            {{"offline-qa", "r1", prepared.snapshot.root, prepared.roots, {}}}};
        const auto signed_wire = sign_publisher_feed(feed, seed);
        const auto signed_feed = verify_publisher_feed(signed_wire, publisher_key_from_seed(seed));
        const Bytes unrelated_private{'p', 'r', 'i', 'v', 'a', 't', 'e'};
        const auto unrelated_block = identify_block(unrelated_private);
        {
            std::ofstream file(prepared.directory / (hex(unrelated_block) + ".gksb"), std::ios::binary);
            file.write(reinterpret_cast<const char*>(unrelated_private.data()), unrelated_private.size());
            check(bool(file));
        }
        const auto plan = inspect_publication(prepared);
        check(plan.root == prepared.snapshot.root && plan.blocks.back() == plan.root);
        check(std::is_sorted(plan.blocks.begin(), plan.blocks.end() - 1));
        check(plan.blocks.size() > 2 && plan.payload_bytes > 0);
        check(std::find(plan.blocks.begin(), plan.blocks.end(), unrelated_block) == plan.blocks.end());
        auto source = std::make_shared<SnapshotBlocks>(prepared.directory, prepared.snapshot.root);
        check(inspect_contribution(source, prepared.snapshot.root, prepared.roots, prepared.objects) == plan);
        PublicationLimits limits; limits.blocks = 1;
        rejects(ErrorCode::Budget, [&] { inspect_publication(prepared, limits); });
        limits = {}; limits.payload_bytes = 1;
        rejects(ErrorCode::Budget, [&] { inspect_publication(prepared, limits); });
        limits = {}; limits.record_bytes = 1;
        rejects(ErrorCode::Budget, [&] { inspect_publication(prepared, limits); });
        const auto extra = journal.put(object("unselected public data"));
        auto expanded = prepared;
        expanded.directory = directory.path / "unselected";
        expanded.objects.push_back(extra);
        expanded.snapshot = compile_selected_snapshot(journal, expanded.objects, expanded.directory, "offline-qa", "r1");
        rejects(ErrorCode::Integrity, [&] { inspect_publication(expanded); });
        for (const auto* origin : {"http://example.com:5001", "http://127.0.0.1:5001/path",
             "http://user@127.0.0.1:5001/", "http://127.0.0.1:5001/?key=secret",
             "http://127.0.0.1:5001/#fragment", "https://127.0.0.1:5001/",
             "http://127.0.0.1:5001/?", "http://127.0.0.1:5001/#", "http://@127.0.0.1:5001/"})
            rejects(ErrorCode::Permission, [&] { ContributionPublisher publisher(origin); });
        auto nul_origin = std::string("http://127.0.0.1:5001/"); nul_origin.push_back('\0');
        nul_origin += "other";
        rejects(ErrorCode::Schema, [&] { ContributionPublisher publisher(nul_origin); });
        HttpFixture server(ack);
        ContributionPublisher publisher(server.origin());
        auto permit = publisher.approve(prepared);
        check(server.requests().empty()); // Neither Public nor approval uploads.
        ContributionPublisher foreign(server.origin());
        rejects(ErrorCode::Permission, [&] { foreign.publish(permit); });
        auto moved = std::move(permit);
        rejects(ErrorCode::Permission, [&] { publisher.publish(permit); });
        const auto receipt = publisher.publish(moved);
        check(receipt.root_stored && receipt.confirmed.size() == plan.blocks.size());
        check(receipt.confirmed_payload_bytes == plan.payload_bytes);
        check(receipt.uploaded_http_body_bytes > receipt.confirmed_payload_bytes);
        check(receipt.attempted_requests == plan.blocks.size());
        const auto requests = server.requests();
        check(requests.size() == plan.blocks.size());
        for (std::size_t i = 0; i < requests.size(); ++i)
            check(identify_block(uploaded(requests[i])) == plan.blocks[i]);
        rejects(ErrorCode::Permission, [&] { publisher.publish(moved); });
        check(server.requests().size() == requests.size());
        auto revoked = publisher.approve(prepared);
        revoked.revoke();
        rejects(ErrorCode::Permission, [&] { publisher.publish(revoked); });
        check(server.requests().size() == requests.size());
        auto all = publisher.approve(prepared);
        publisher.revoke_all();
        rejects(ErrorCode::Permission, [&] { publisher.publish(all); });
        rejects(ErrorCode::Permission, [&] { publisher.approve(prepared); });
        std::optional<PublicationPermit> expired;
        {
            ContributionPublisher temporary(server.origin());
            expired.emplace(temporary.approve(prepared));
        }
        rejects(ErrorCode::Permission, [&] { foreign.publish(*expired); });
        HttpFixture failed([](const HttpRequest& request) -> HttpResponse {
            uploaded(request); return {{'n', 'o'}, 503};
        });
        ContributionPublisher unavailable(failed.origin());
        auto fail_once = unavailable.approve(prepared);
        rejects(ErrorCode::Unavailable, [&] { unavailable.publish(fail_once); });
        check(unavailable.last_receipt().attempted_requests == 1 && !unavailable.last_receipt().root_stored
            && unavailable.last_receipt().confirmed.empty());
        rejects(ErrorCode::Permission, [&] { unavailable.publish(fail_once); });
        check(failed.requests().size() == 1);
        std::atomic<std::size_t> count{0};
        HttpFixture partial([&](const HttpRequest& request) -> HttpResponse {
            if (++count == 2) return {{}, 503};
            return ack(request);
        });
        ContributionPublisher interrupted(partial.origin());
        auto partial_permit = interrupted.approve(prepared);
        rejects(ErrorCode::Unavailable, [&] { interrupted.publish(partial_permit); });
        check(interrupted.last_receipt().confirmed.size() == 1
            && interrupted.last_receipt().attempted_requests == 2 && !interrupted.last_receipt().root_stored);
        HttpFixture wrong([](const HttpRequest& request) -> HttpResponse {
            auto value = ack(request);
            // Canonical but different raw CID, not an incidental parse failure.
            const auto bytes = uploaded(request);
            const auto text = "{\"Key\":\"" + cid_text(block_cid(BlockId{}))
                + "\",\"Size\":" + std::to_string(bytes.size()) + "}";
            value.body.assign(text.begin(), text.end()); return value;
        });
        ContributionPublisher dishonest(wrong.origin());
        auto wrong_permit = dishonest.approve(prepared);
        rejects(ErrorCode::Integrity, [&] { dishonest.publish(wrong_permit); });
        check(dishonest.last_receipt().confirmed.empty() && !dishonest.last_receipt().root_stored);
        std::optional<PublicationPermit> cancelling;
        HttpFixture cancellation([&](const HttpRequest& request) {
            cancelling->revoke(); return ack(request);
        });
        ContributionPublisher cancellable(cancellation.origin());
        cancelling.emplace(cancellable.approve(prepared));
        rejects(ErrorCode::Permission, [&] { cancellable.publish(*cancelling); });
        check(cancellation.requests().size() == 1 && !cancellable.last_receipt().root_stored);
        const auto tampered = prepare_contribution(journal, std::array{root}, directory.path / "tampered", "offline-qa", "r1");
        ContributionPublisher immutable(server.origin());
        auto changed = immutable.approve(tampered);
        {
            const auto path = tampered.directory / (hex(tampered.snapshot.root) + ".gksb");
            std::filesystem::permissions(path, std::filesystem::perms::owner_write, std::filesystem::perm_options::add);
            std::fstream file(path,
                              std::ios::binary | std::ios::in | std::ios::out);
            char first = 0; file.read(&first, 1); first ^= 1;
            file.seekp(0); file.write(&first, 1); check(bool(file));
        }
        const auto before_tamper = server.requests().size();
        rejects(ErrorCode::Integrity, [&] { immutable.publish(changed); });
        check(server.requests().size() == before_tamper);
        HttpFixture metadata_server(ack);
        ContributionPublisher metadata(metadata_server.origin());
        auto metadata_permit = metadata.approve_feed(signed_feed);
        check(metadata_server.requests().empty());
        auto metadata_moved = std::move(metadata_permit);
        rejects(ErrorCode::Permission, [&] { metadata.publish(metadata_permit); });
        const auto metadata_receipt = metadata.publish(metadata_moved);
        check(metadata_receipt.confirmed.size() == 1 && metadata_receipt.root_stored
            && metadata_receipt.root == identify_block(signed_wire));
        check(metadata_receipt.confirmed_payload_bytes == signed_wire.size());
        check(metadata_server.requests().size() == 1 && uploaded(metadata_server.requests().front()) == signed_wire);
        rejects(ErrorCode::Permission, [&] { metadata.publish(metadata_moved); });
        auto metadata_revoked = metadata.approve_feed(signed_feed);
        metadata_revoked.revoke();
        rejects(ErrorCode::Permission, [&] { metadata.publish(metadata_revoked); });
        check(metadata_server.requests().size() == 1);
        limits = {}; limits.payload_bytes = 1;
        ContributionPublisher small_metadata(metadata_server.origin(), limits);
        rejects(ErrorCode::Budget, [&] { small_metadata.approve_feed(signed_feed); });
        server.finish(); failed.finish(); partial.finish(); wrong.finish(); cancellation.finish(); metadata_server.finish();
        if (argc == 2) {
            if (!std::getenv("GAMMATHINK_KUBO_QA_OFFLINE"))
                throw std::runtime_error("Real-node QA must be driven by the owned offline-daemon harness");
            ContributionPublisher kubo(argv[1]);
            auto approval = kubo.approve(prepared);
            const auto real = kubo.publish(approval);
            check(real.root_stored && real.confirmed.size() == plan.blocks.size());
            GksSnapshot snapshot(prepared.directory, prepared.snapshot.root);
            std::cout << "KUBO_PUBLICATION {\"root\":\"" << cid_text(block_cid(real.root)) << "\",\"blocks\":[";
            for (std::size_t i = 0; i < real.confirmed.size(); ++i) {
                if (i) std::cout << ',';
                const auto id = block_id(real.confirmed[i]);
                std::cout << "{\"cid\":\"" << cid_text(real.confirmed[i]) << "\",\"sha256\":\""
                    << hex(id) << "\",\"bytes\":" << snapshot.block(id).size() << '}';
            }
            std::cout << "]}\n";
            auto feed_approval = kubo.approve_feed(signed_feed);
            const auto published_feed = kubo.publish(feed_approval);
            check(published_feed.root_stored && published_feed.confirmed.size() == 1
                && published_feed.root == identify_block(signed_wire));
            std::cout << "KUBO_FEED {\"cid\":\"" << cid_text(block_cid(published_feed.root))
                << "\",\"sha256\":\"" << hex(published_feed.root) << "\",\"bytes\":"
                << signed_wire.size() << "}\n";
        } else if (argc != 1) throw std::runtime_error("Unexpected publication test arguments");
        std::cout << "Contribution publication passed: " << assertions << " deterministic assertions\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
