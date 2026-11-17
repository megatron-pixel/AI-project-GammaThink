#include "publisher_catalogue.hpp"
#include "virtual_drive.hpp"

#include <sqlite3.h>

#include <functional>
#include <iostream>
#include <unistd.h>

namespace {
using namespace axiom::knowledge;
std::size_t assertions = 0;
void check(bool value) {
    ++assertions;
    if (!value) throw std::runtime_error("Publisher catalogue assertion " + std::to_string(assertions));
}
template<class Function> void rejects(ErrorCode expected, Function function) {
    try { function(); }
    catch (const Error& error) { check(error.code == expected); return; }
    throw std::runtime_error("Expected publisher catalogue rejection");
}
struct Directory {
    std::filesystem::path path;
    Directory() {
        auto pattern = (std::filesystem::temp_directory_path() / "gammathink-catalogue-XXXXXX").string();
        const auto* made = mkdtemp(pattern.data());
        if (!made) throw std::runtime_error("Cannot create catalogue fixture");
        path = made;
    }
    ~Directory() { std::filesystem::remove_all(path); }
};
class Source final : public BlockSource {
public:
    Source(const std::filesystem::path& directory, BlockId root) : snapshot(directory, root) {}
    Bytes read(const BlockId& id, std::size_t maximum) override {
        ++reads;
        if (on_read) { auto action = std::move(on_read); action(); }
        return snapshot.block(id, maximum);
    }
    std::size_t reads = 0;
    std::function<void()> on_read;
    GksSnapshot snapshot;
};
}
int main() {
    try {
        Directory directory;
        const auto seed = parse_publisher_key("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60");
        const auto key = publisher_key_from_seed(seed);
        auto other_seed = seed; other_seed[0] ^= 1;
        const auto other_key = publisher_key_from_seed(other_seed);
        Journal first(":memory:"), second(":memory:"), overlay(":memory:");
        Object assertion; assertion.kind = ObjectKind::Claim;
        assertion.label = "Synthetic assertion"; assertion.format = "synthetic.catalogue.v1"; assertion.payload = {1};
        const auto a = first.put(assertion);
        assertion.label = "Synthetic conflicting assertion"; assertion.payload = {0};
        assertion.links = {{Relationship::Contradicts, a}};
        const auto b = second.put(assertion);
        Assessment observed; observed.logical = LogicalValidity::Valid;
        observed.state = KnowledgeState::Verified; observed.checker = "synthetic-publisher-only";
        second.record_assessment(b, observed);
        const auto snapshot_a = compile_snapshot(first, directory.path / "a", "same-name", "1");
        const auto snapshot_b = compile_snapshot(second, directory.path / "b", "same-name", "1");
        const auto signed_feed = [&](PublisherFeed value, const PublisherKey& signer) {
            return verify_publisher_feed(sign_publisher_feed(value, signer), publisher_key_from_seed(signer));
        };
        const auto initial = signed_feed({"stable", 1, {}, {{"same-name", "1", snapshot_a.root, {a}, {}}}}, seed);
        const auto update = signed_feed({"stable", 2, initial.id(),
            {{"same-name", "1", snapshot_b.root, {b}, {snapshot_a.root}}}}, seed);
        const auto fork = signed_feed({"stable", 2, initial.id(),
            {{"same-name", "1", snapshot_a.root, {a}, {}}}}, seed);
        const auto independent = signed_feed({"stable", 1, {},
            {{"same-name", "1", snapshot_b.root, {b}, {}}}}, other_seed);
        const auto path = directory.path / "catalogue.db";
        {
            PublisherCatalogue catalogue(path);
            catalogue.discover(update);
            check(catalogue.mounts().empty() && overlay.objects().objects.empty());
            check(!catalogue.chain_complete(update.id(), key));
            rejects(ErrorCode::Unavailable, [&] {
                catalogue.accept_local("update", update.id(), 0, key, directory.path / "b", 0);
            });
            catalogue.discover(initial); catalogue.discover(fork); catalogue.discover(independent);
            catalogue.discover(initial); // Exact idempotence, no duplicate or loss.
            check(catalogue.chain_complete(update.id(), key) && catalogue.chain_complete(fork.id(), key));
            check(catalogue.get(update.id(), key).content() == update.content());
            rejects(ErrorCode::Permission, [&] { catalogue.get(initial.id(), other_key); });
            rejects(ErrorCode::Missing, [&] { catalogue.get(PublisherFeedId{}, key); });
            std::vector<PublisherFeedId> all;
            std::optional<PublisherFeedId> after;
            do {
                const auto page = catalogue.feeds(after, 1);
                check(page.feeds.size() == 1);
                all.push_back(page.feeds.front()); after = page.next;
                check(page.complete == !page.next.has_value());
            } while (after);
            check(all.size() == 4);
            rejects(ErrorCode::Budget, [&] { catalogue.feeds({}, 0); });
            check(catalogue.accept_local("old", initial.id(), 0, key, directory.path / "a", 0).generation == 1);
            check(catalogue.accept_local("new", update.id(), 0, key, directory.path / "b", 0).generation == 1);
            check(catalogue.mounts().size() == 2);
            ArchiveRegistry local_registry(directory.path / "local-registry.db");
            local_registry.register_snapshot(directory.path / "a", snapshot_a.root);
            local_registry.mount("base", "same-name", "1", directory.path / "a", 0);
            VirtualDrive combined(overlay, local_registry);
            const auto combined_view = combined.open_view(catalogue.mounts());
            check(combined_view->knowledge().get(a).has_value() && combined_view->knowledge().get(b).has_value());
            check(combined_view->mounts().size() == 3);
            local_registry.mount("old", "same-name", "1", directory.path / "a", 0);
            rejects(ErrorCode::Schema, [&] { combined.open_view(catalogue.mounts()); });
            check(local_registry.resolve("old").binding->archive.root == snapshot_a.root);
            DriveView old_view(overlay, catalogue.mounts());
            check(old_view.knowledge().get(a).has_value() && old_view.knowledge().get(b).has_value());
            check(old_view.knowledge().get(b)->record.assessment == second.assessment(b));
            check(!overlay.assessment(b) && overlay.objects().objects.empty());
            KnowledgeQuery all_objects;
            check(old_view.knowledge().query(all_objects).objects.size() == 2);
            rejects(ErrorCode::Unavailable, [&] { catalogue.accept_local("old", update.id(), 0, key, directory.path / "b", 0); });
            rejects(ErrorCode::Schema, [&] { catalogue.accept_local("bad", initial.id(), 1, key, directory.path / "a", 0); });
            rejects(ErrorCode::Permission, [&] { catalogue.accept_local("bad", initial.id(), 0, other_key, directory.path / "a", 0); });
            const auto wrong = signed_feed({"stable", 1, {}, {{"wrong-name", "1", snapshot_a.root, {a}, {}}}}, seed);
            catalogue.discover(wrong);
            rejects(ErrorCode::Integrity, [&] { catalogue.accept_local("bad", wrong.id(), 0, key, directory.path / "a", 0); });
            check(catalogue.resolve("bad").generation == 0);
            check(catalogue.accept_local("old", update.id(), 0, key, directory.path / "b", 1).generation == 2);
            check(old_view.knowledge().get(a).has_value()); // Prior view retains its exact original root.
            DriveView new_view(overlay, catalogue.mounts());
            check(!new_view.knowledge().get(a).has_value() && new_view.knowledge().get(b).has_value());
            auto source = std::make_shared<Source>(directory.path / "a", snapshot_a.root);
            check(catalogue.accept_remote("remote", initial.id(), 0, key, MountBackend::RemoteBlocks,
                "host-source", source, 0).generation == 1);
            check(source->reads == 1); // Only the bounded root manifest; no archive sweep.
            DriveAccess capabilities;
            capabilities.blocks = [&](std::string_view name) {
                check(name == "host-source"); return source;
            };
            DriveView remote_view(overlay, catalogue.mounts(), {}, capabilities);
            check(remote_view.knowledge().get(a).has_value());
            PublisherCatalogue concurrent(path);
            source->on_read = [&] { concurrent.unmount("remote", 1); };
            rejects(ErrorCode::Unavailable, [&] {
                catalogue.accept_remote("remote", initial.id(), 0, key, MountBackend::RemoteBlocks,
                    "host-source", source, 1);
            });
            check(catalogue.resolve("remote").generation == 2 && !catalogue.resolve("remote").binding);
            check(catalogue.unmount("new", 1).generation == 2);
            const auto original_history = catalogue.history("old", 0, 1);
            check(!original_history.complete && original_history.next == 1);
            check(original_history.records.front().feed == initial.id()
                && original_history.records.front().offer == 0
                && original_history.records.front().state.binding->archive.root == snapshot_a.root);
            const auto updated_history = catalogue.history("old", *original_history.next, 1);
            check(updated_history.complete && !updated_history.next
                && updated_history.records.front().feed == update.id()
                && updated_history.records.front().state.binding->archive.root == snapshot_b.root);
            const auto unmounted_history = catalogue.history("new");
            check(unmounted_history.records.size() == 2 && unmounted_history.records.front().feed == update.id()
                && !unmounted_history.records.back().feed && !unmounted_history.records.back().offer
                && !unmounted_history.records.back().state.binding);
            rejects(ErrorCode::Budget, [&] { catalogue.history("old", 0, 65); });
            check(catalogue.feeds().feeds.size() == 5 && catalogue.mounts().size() == 1);
            rejects(ErrorCode::Missing, [&] { catalogue.unmount("new", 2); });
            const auto bad_chain = signed_feed({"different-channel", 2, initial.id(), {}}, seed);
            catalogue.discover(bad_chain);
            rejects(ErrorCode::Integrity, [&] { catalogue.chain_complete(bad_chain.id(), key); });
        }
        {
            PublisherCatalogue reopened(path);
            check(reopened.mounts().size() == 1 && reopened.resolve("old").generation == 2);
            check(reopened.feeds().feeds.size() == 6);
            check(reopened.get(fork.id(), key).content() == fork.content());
            check(reopened.resolve("remote").generation == 2 && !reopened.resolve("remote").binding);
            check(reopened.history("old").records.size() == 2);
        }
        {
            PublisherCatalogueLimits tiny; tiny.feeds = 1;
            PublisherCatalogue catalogue(directory.path / "tiny.db", tiny);
            catalogue.discover(initial); catalogue.discover(initial);
            rejects(ErrorCode::Budget, [&] { catalogue.discover(update); });
            check(catalogue.feeds().feeds.size() == 1);
        }
        {
            PublisherCatalogueLimits tiny; tiny.history = 1;
            PublisherCatalogue catalogue(directory.path / "history.db", tiny);
            catalogue.discover(initial);
            catalogue.accept_local("only", initial.id(), 0, key, directory.path / "a", 0);
            rejects(ErrorCode::Budget, [&] { catalogue.unmount("only", 1); });
            check(catalogue.resolve("only").generation == 1);
        }
        {
            PublisherCatalogueLimits tiny; tiny.chain = 1;
            PublisherCatalogue catalogue(path, tiny);
            rejects(ErrorCode::Budget, [&] { catalogue.chain_complete(update.id(), key); });
        }
        {
            Journal unrelated(directory.path / "unrelated.db");
            rejects(ErrorCode::Schema, [&] { PublisherCatalogue wrong(directory.path / "unrelated.db"); });
        }
        {
            sqlite3* database = nullptr;
            check(sqlite3_open(path.c_str(), &database) == SQLITE_OK);
            check(sqlite3_exec(database, "UPDATE feeds SET wire=x'00'", nullptr, nullptr, nullptr) == SQLITE_OK);
            check(sqlite3_close(database) == SQLITE_OK);
            PublisherCatalogue catalogue(path);
            rejects(ErrorCode::Integrity, [&] { catalogue.get(initial.id(), key); });
        }
        std::cout << "Publisher catalogue passed: " << assertions << " deterministic assertions\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
