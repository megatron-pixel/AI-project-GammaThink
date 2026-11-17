#include "block_cache.hpp"

#include <fcntl.h>
#include <iostream>
#include <map>
#include <sys/stat.h>
#include <unistd.h>

namespace {
using namespace axiom::knowledge;
constexpr std::size_t stored_limit = 16 + gks_max_block_bytes + 65536;
std::size_t assertions = 0;
void check(bool condition) {
    ++assertions;
    if (!condition) throw std::runtime_error("Block cache assertion " + std::to_string(assertions));
}
template<class Function> void rejects(ErrorCode expected, Function function) {
    try { function(); }
    catch (const Error& error) { check(error.code == expected); return; }
    throw std::runtime_error("Expected cache rejection");
}
struct Directory {
    std::filesystem::path path;
    Directory() {
        auto pattern = (std::filesystem::temp_directory_path() / "gammathink-cache-XXXXXX").string();
        std::vector<char> data(pattern.begin(), pattern.end()); data.push_back(0);
        const auto* made = mkdtemp(data.data());
        if (!made) throw std::runtime_error("Cannot create cache fixture");
        path = made;
    }
    ~Directory() { std::filesystem::remove_all(path); }
};
struct Source final : BlockSource {
    std::map<BlockId, Bytes> blocks;
    std::size_t calls = 0;
    Bytes read(const BlockId& id, std::size_t) override {
        ++calls;
        const auto found = blocks.find(id);
        if (found == blocks.end()) throw Error(ErrorCode::Unavailable, "Fixture block missing");
        return found->second; // Deliberately does not enforce caller bounds.
    }
};
void write_file(const std::filesystem::path& path, const Bytes& bytes) {
    const auto fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
    check(fd >= 0);
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto size = write(fd, bytes.data() + offset, bytes.size() - offset);
        check(size > 0); offset += static_cast<std::size_t>(size);
    }
    check(close(fd) == 0);
}
void bounded(const BlockCache& cache, const BlockCacheLimits& limits, const std::filesystem::path& path) {
    const auto stats = cache.stats();
    check(stats.ram_bytes <= limits.ram_bytes && stats.ram_entries <= limits.ram_entries);
    check(stats.ssd_bytes <= limits.ssd_bytes && stats.ssd_entries <= limits.ssd_entries);
    std::uint64_t bytes = 0; std::size_t files = 0;
    for (const auto& file : std::filesystem::directory_iterator(path)) {
        if (file.path().extension() == ".gksb") { bytes += file.file_size(); ++files; }
    }
    check(bytes == stats.ssd_bytes && files == stats.ssd_entries);
    check(!std::filesystem::exists(path / ".pending"));
}
}
int main() {
    try {
        Directory directory;
        auto source = std::make_shared<Source>();
        const Bytes a(32, 1), b(48, 2), c(64, 3), large(80, 4);
        const auto aid = identify_block(a), bid = identify_block(b), cid = identify_block(c), lid = identify_block(large);
        source->blocks = {{aid, a}, {bid, b}, {cid, c}, {lid, large}};
        const auto file = [&](const std::filesystem::path& path, const BlockId& id) { return path / (hex(id) + ".gksb"); };
        BlockCacheLimits limits; limits.ram_bytes = 96; limits.ram_entries = 2; limits.ssd_bytes = 0;
        {
            const auto path = directory.path / "ram";
            BlockCache cache(path, source, limits);
            const auto before = source->calls;
            check(cache.read(aid, 32) == a); check(cache.read(bid, 48) == b);
            auto returned = cache.read(aid, 32); returned.front() ^= 1;
            check(cache.read(aid, 32) == a); // Caller mutation cannot poison RAM.
            check(cache.read(cid, 64) == c); bounded(cache, limits, path);
            check(cache.stats().ram_bytes == 96 && cache.stats().ram_hits == 2);
            check(cache.read(bid, 48) == b);
            check(source->calls == before + 4);
            rejects(ErrorCode::Budget, [&] { cache.read(bid, 47); });
            bounded(cache, limits, path);
        }
        {
            auto entries = limits; entries.ram_bytes = 1024; entries.ram_entries = 1;
            const auto path = directory.path / "ram-entries";
            BlockCache cache(path, source, entries);
            cache.read(aid, 32); cache.read(bid, 48); cache.read(aid, 32);
            check(cache.stats().ram_entries == 1 && cache.stats().source_reads == 3);
            bounded(cache, entries, path);
        }
        limits.ram_bytes = 0; limits.ssd_bytes = 112; limits.ssd_entries = 2;
        const auto persistent = directory.path / "persistent";
        {
            BlockCache cache(persistent, source, limits);
            cache.read(aid, 32); cache.read(bid, 48); cache.read(aid, 32); cache.read(cid, 64);
            check(std::filesystem::exists(file(persistent, aid)));
            check(!std::filesystem::exists(file(persistent, bid)));
            check(std::filesystem::exists(file(persistent, cid)));
            check(cache.stats().ssd_bytes == 96 && cache.stats().ssd_hits == 1);
            bounded(cache, limits, persistent);
            rejects(ErrorCode::Storage, [&] { BlockCache duplicate(persistent, source, limits); });
        }
        {
            BlockCache cache(persistent, {}, limits);
            const auto before = source->calls;
            check(cache.read(aid, 32) == a && cache.read(cid, 64) == c);
            rejects(ErrorCode::Unavailable, [&] { cache.read(bid, 48); });
            rejects(ErrorCode::Budget, [&] { cache.read(aid, 31); });
            check(source->calls == before && cache.stats().source_reads == 0);
            bounded(cache, limits, persistent);
        }
        {
            auto smaller = limits; smaller.ssd_bytes = 31;
            rejects(ErrorCode::Budget, [&] { BlockCache cache(persistent, {}, smaller); });
            check(std::filesystem::exists(file(persistent, aid)) && std::filesystem::exists(file(persistent, cid)));
        }
        {
            auto dual = limits; dual.ram_bytes = 96; dual.ram_entries = 2;
            const auto path = directory.path / "dual";
            BlockCache cache(path, source, dual);
            cache.read(aid, 32); cache.read(bid, 48); cache.read(aid, 32); cache.read(cid, 64);
            check(std::filesystem::exists(file(path, aid)) && !std::filesystem::exists(file(path, bid)));
            check(cache.stats().ram_hits == 1); bounded(cache, dual, path);
        }
        {
            auto entries = limits; entries.ssd_bytes = 1024;
            const auto path = directory.path / "entries";
            BlockCache cache(path, source, entries);
            cache.read(aid, 32); cache.read(bid, 48); cache.read(cid, 64);
            check(!std::filesystem::exists(file(path, aid)) && cache.stats().ssd_entries == 2);
            bounded(cache, entries, path);
        }
        {
            auto small = limits; small.ram_bytes = 16; small.ssd_bytes = 16;
            const auto path = directory.path / "bypass";
            BlockCache cache(path, source, small);
            check(cache.read(lid, 80) == large && cache.read(lid, 80) == large);
            check(cache.stats().source_reads == 2 && cache.stats().ssd_entries == 0 && cache.stats().ram_entries == 0);
            bounded(cache, small, path);
        }
        {
            const auto path = directory.path / "bad-source";
            BlockCache cache(path, source, limits);
            rejects(ErrorCode::Budget, [&] { cache.read(aid, 16); });
            source->blocks[aid] = b;
            rejects(ErrorCode::Integrity, [&] { cache.read(aid, 48); });
            source->blocks[aid] = a;
            check(cache.stats().source_reads == 2 && cache.stats().ssd_entries == 0);
            bounded(cache, limits, path);
        }
        check(chmod(file(persistent, aid).c_str(), 0600) == 0);
        auto damaged = a; damaged.front() ^= 1;
        write_file(file(persistent, aid), damaged);
        {
            BlockCache cache(persistent, source, limits);
            const auto before = source->calls;
            rejects(ErrorCode::Integrity, [&] { cache.read(aid, 32); });
            check(source->calls == before && cache.stats().ram_entries == 0);
        }
        write_file(file(persistent, aid), a);
        // Simulate interruption after publishing the hard link, before
        // removing the fixed staging name.
        check(link(file(persistent, aid).c_str(), (persistent / ".pending").c_str()) == 0);
        {
            BlockCache cache(persistent, {}, limits);
            check(!std::filesystem::exists(persistent / ".pending") && cache.read(aid, 32) == a);
        }
        write_file(persistent / ".pending", Bytes{9, 8, 7});
        {
            BlockCache cache(persistent, {}, limits);
            check(cache.read(aid, 32) == a && !std::filesystem::exists(persistent / ".pending"));
        }
        {
            const auto path = directory.path / "write-failure";
            {
                BlockCache cache(path, source, limits);
                write_file(path / ".pending", Bytes{1});
                rejects(ErrorCode::Storage, [&] { cache.read(aid, 32); });
                rejects(ErrorCode::Storage, [&] { cache.read(bid, 48); });
            }
            BlockCache recovered(path, source, limits);
            check(recovered.read(aid, 32) == a); bounded(recovered, limits, path);
        }
        {
            const auto path = directory.path / "unknown";
            check(mkdir(path.c_str(), 0700) == 0);
            write_file(path / "unrelated", a);
            rejects(ErrorCode::Schema, [&] { BlockCache rejected(path); });
            check(std::filesystem::exists(path / "unrelated"));
        }
        {
            const auto path = directory.path / "public";
            check(mkdir(path.c_str(), 0755) == 0 && chmod(path.c_str(), 0755) == 0);
            rejects(ErrorCode::Permission, [&] { BlockCache rejected(path); });
            check(!std::filesystem::exists(path / ".lock"));
        }
        {
            const auto path = directory.path / "symlink";
            std::filesystem::create_directory_symlink(persistent, path);
            rejects(ErrorCode::Storage, [&] { BlockCache rejected(path); });
        }
        for (const bool hard : {false, true}) {
            const auto path = directory.path / (hard ? "hard" : "soft");
            check(mkdir(path.c_str(), 0700) == 0);
            if (hard) check(link(file(persistent, aid).c_str(), file(path, aid).c_str()) == 0);
            else std::filesystem::create_symlink(file(persistent, aid), file(path, aid));
            rejects(ErrorCode::Permission, [&] { BlockCache rejected(path); });
            std::filesystem::remove(file(path, aid));
        }
        {
            const auto path = directory.path / "huge";
            check(mkdir(path.c_str(), 0700) == 0);
            const auto fd = open(file(path, aid).c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
            check(fd >= 0 && ftruncate(fd, stored_limit + 1) == 0 && close(fd) == 0);
            rejects(ErrorCode::Budget, [&] { BlockCache rejected(path); });
        }
        {
            auto invalid = limits; invalid.ram_entries = 65537;
            rejects(ErrorCode::Budget, [&] { BlockCache rejected(directory.path / "invalid", {}, invalid); });
            check(!std::filesystem::exists(directory.path / "invalid"));
        }
        // Actual snapshot traversal through the cache, followed by offline
        // reopening with no source and the original journal gone.
        const auto snapshot_path = directory.path / "snapshot";
        const auto snapshot_cache = directory.path / "snapshot-cache";
        Object object; object.label = "offline source"; object.format = "text/plain"; object.payload = {4, 5, 6};
        const auto object_id = identify(object);
        BlockId root;
        BlockCacheLimits generous;
        {
            Journal journal(":memory:"); check(journal.put(object) == object_id);
            root = compile_snapshot(journal, snapshot_path, "cached", "v1").root;
            GksSnapshot local(snapshot_path, root);
            auto blocks = std::make_shared<Source>();
            for (const auto& entry : std::filesystem::directory_iterator(snapshot_path)) {
                if (entry.path().extension() != ".gksb") continue;
                const auto id = parse_block_id(entry.path().stem().string());
                blocks->blocks.emplace(id, local.block(id));
            }
            auto cache = std::make_shared<BlockCache>(snapshot_cache, blocks, generous);
            GksSnapshot cached(cache, root);
            check(cached.get(object_id)->object == object);
            const auto before = blocks->calls;
            check(cached.get(object_id)->object == object && blocks->calls == before);
            check(cache->stats().ram_hits > 0); bounded(*cache, generous, snapshot_cache);
        }
        std::filesystem::remove_all(snapshot_path);
        {
            auto cache = std::make_shared<BlockCache>(snapshot_cache);
            GksSnapshot offline(cache, root);
            check(offline.get(object_id)->object == object);
            check(cache->stats().source_reads == 0 && cache->stats().ssd_hits > 0);
            rejects(ErrorCode::Unavailable, [&] { cache->read(lid, 80); });
        }
        std::cout << "block cache tests passed: " << assertions << " deterministic assertions\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
