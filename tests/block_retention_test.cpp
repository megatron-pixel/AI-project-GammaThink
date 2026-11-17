#include "block_cache.hpp"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <map>
#include <thread>
#include <unistd.h>

namespace {
using namespace axiom::knowledge;
std::size_t assertions = 0;
void check(bool value) {
    ++assertions;
    if (!value) throw std::runtime_error("Block retention assertion " + std::to_string(assertions));
}
template<class Function> void rejects(ErrorCode expected, Function function) {
    try { function(); }
    catch (const Error& error) { check(error.code == expected); return; }
    throw std::runtime_error("Expected block retention rejection");
}
struct Directory {
    std::filesystem::path path;
    Directory() {
        auto pattern = (std::filesystem::temp_directory_path() / "gammathink-retention-XXXXXX").string();
        std::vector<char> value(pattern.begin(), pattern.end()); value.push_back(0);
        const auto* made = mkdtemp(value.data());
        if (!made) throw std::runtime_error("Cannot create retention fixture");
        path = made;
    }
    ~Directory() { std::filesystem::remove_all(path); }
};
struct Source final : BlockSource {
    std::map<BlockId, Bytes> blocks;
    std::size_t calls = 0;
    Bytes read(const BlockId& id, std::size_t maximum) override {
        ++calls;
        const auto found = blocks.find(id);
        if (found == blocks.end()) throw Error(ErrorCode::Unavailable, "Missing retention fixture");
        if (found->second.size() > maximum) throw Error(ErrorCode::Budget, "Retention fixture byte budget");
        return found->second;
    }
};
}
int main() {
    try {
        Directory directory;
        const Bytes a(32, 1), b(48, 2), c(16, 3), d(24, 4);
        const auto aid = identify_block(a), bid = identify_block(b), cid = identify_block(c), did = identify_block(d);
        const std::array ap{aid}, bp{bid}, cp{cid};
        const std::array ab{aid, bid};
        auto source = std::make_shared<Source>();
        source->blocks = {{aid, a}, {bid, b}, {cid, c}, {did, d}};
        BlockCacheLimits limits;
        limits.ram_bytes = 512; limits.ram_entries = 8;
        limits.ssd_bytes = 512; limits.ssd_entries = 8; limits.lease_references = 8;
        const auto path = directory.path / "cache";
        CachePruneLimits apply; apply.apply = true;
        {
            BlockCache cache(path, source, limits);
            cache.pin(ap);
            auto first = cache.lease(std::array{bid, cid, bid});
            check(first.active() && first.blocks().size() == 2);
            check(cache.pinned() == std::vector<BlockId>{aid});
            check(cache.stats().leased_blocks == 2 && cache.stats().leased_references == 2);
            auto second = cache.lease(bp);
            check(cache.stats().leased_blocks == 2 && cache.stats().leased_references == 3);
            check(cache.read(did, 24) == d);
            first.release(); first.release();
            check(!first.active() && first.blocks().empty());
            check(cache.stats().leased_blocks == 1 && cache.stats().leased_references == 1);
            const auto before = cache.stats();
            const auto retrieval = cache.retrieval_stats();
            const auto plan = cache.prune();
            check(plan.complete && plan.examined == 4 && plan.evictable.size() == 2 && plan.evicted == 0);
            check(std::find(plan.evictable.begin(), plan.evictable.end(), aid) == plan.evictable.end()
                && std::find(plan.evictable.begin(), plan.evictable.end(), bid) == plan.evictable.end());
            check(cache.stats().ssd_bytes == before.ssd_bytes && cache.stats().ram_bytes == before.ram_bytes);
            check(cache.retrieval_stats().reads == retrieval.reads
                && cache.retrieval_stats().upstream_reads == retrieval.upstream_reads);
            const auto pruned = cache.prune({}, apply);
            check(pruned.complete && pruned.evicted == 2 && pruned.ssd_bytes == c.size() + d.size());
            cache.set_offline(true);
            check(cache.read(aid, 32) == a && cache.read(bid, 48) == b);
            rejects(ErrorCode::Unavailable, [&] { cache.read(cid, 16); });
            auto temporary_a = cache.lease(ap);
            cache.unpin(ap);
            check(cache.pinned().empty() && cache.prune({}, apply).evicted == 0);
            temporary_a.release();
            check(cache.prune({}, apply).evicted == 1);
            cache.pin(bp); // A later permanent user pin must survive release.
            auto moved = std::move(second);
            check(!second.active() && moved.active());
            std::thread release([lease = std::move(moved)]() mutable { lease.release(); });
            release.join();
            check(cache.stats().leased_blocks == 0 && cache.stats().leased_references == 0);
            check(cache.prune({}, apply).evicted == 0 && cache.pinned() == std::vector<BlockId>{bid});
        }
        {
            BlockCache reopened(path, {}, limits);
            check(reopened.pinned() == std::vector<BlockId>{bid} && reopened.read(bid, 48) == b);
            check(reopened.stats().leased_references == 0);
            reopened.unpin(bp);
            check(reopened.prune({}, apply).evicted == 1 && reopened.stats().ssd_entries == 0);
        }
        BlockLease expired;
        const auto ephemeral = directory.path / "ephemeral";
        {
            BlockCache cache(ephemeral, source, limits);
            expired = cache.lease(ab);
            check(expired.active() && cache.pinned().empty());
        }
        check(!expired.active());
        {
            BlockCache reopened(ephemeral, {}, limits);
            check(reopened.pinned().empty() && reopened.stats().leased_references == 0);
            check(reopened.prune({}, apply).evicted == 2);
            expired.release(); // Cannot unprotect an unrelated reopened owner.
        }
        {
            auto quota = limits; quota.lease_references = 4;
            BlockCache cache(directory.path / "references", source, quota);
            auto retained = cache.lease(ab);
            rejects(ErrorCode::Budget, [&] { cache.lease(std::array{aid, bid, cid}); });
            check(cache.stats().leased_references == 2);
            BlockId absent; absent.digest.fill(255);
            check(!source->blocks.contains(absent) && cid < absent);
            const std::array missing{cid, absent};
            rejects(ErrorCode::Unavailable, [&] { cache.lease(missing); });
            check(cache.stats().leased_references == 2 && cache.pinned().empty());
            const auto page = cache.prune({}, apply);
            check(page.evicted == 1);
            check(cache.read(aid, 32) == a && cache.read(bid, 48) == b);
            retained.release();
            check(cache.prune({}, apply).evicted == 2);
        }
        {
            auto quota = limits; quota.ssd_entries = 2;
            BlockCache cache(directory.path / "entries", source, quota);
            cache.pin(ap);
            auto retained = cache.lease(bp);
            const auto calls = source->calls;
            rejects(ErrorCode::Budget, [&] { cache.lease(cp); });
            check(source->calls == calls && cache.stats().leased_references == 1);
            check(cache.pinned() == std::vector<BlockId>{aid});
        }
        {
            auto quota = limits; quota.ssd_bytes = 64;
            BlockCache cache(directory.path / "bytes", source, quota);
            cache.pin(ap);
            rejects(ErrorCode::Budget, [&] { cache.lease(bp); });
            check(cache.stats().leased_references == 0 && cache.pinned() == std::vector<BlockId>{aid});
            auto fits = cache.lease(cp);
            check(fits.active() && cache.stats().leased_blocks == 1);
        }
        {
            BlockCache cache(directory.path / "pages", source, limits);
            for (const auto& [id, bytes] : source->blocks) cache.read(id, bytes.size());
            auto retained = cache.lease(ap);
            CachePruneLimits tiny{1, 1, true};
            std::optional<BlockId> after;
            std::size_t seen = 0, evicted = 0;
            for (;;) {
                const auto page = cache.prune(after, tiny);
                check(page.examined <= 1 && page.evicted <= 1);
                seen += page.examined; evicted += page.evicted;
                if (page.complete) { check(!page.next); break; }
                check(page.next && (!after || *page.next > *after));
                after = page.next;
                check(seen <= 4);
            }
            check(seen == 4 && evicted == 3 && cache.read(aid, 32) == a);
            auto invalid = tiny; invalid.examined = 0;
            rejects(ErrorCode::Budget, [&] { cache.prune({}, invalid); });
            invalid = tiny; invalid.candidates = 2;
            rejects(ErrorCode::Budget, [&] { cache.prune({}, invalid); });
        }
        {
            BlockCache cache(directory.path / "expired-retrieval", source, limits);
            cache.read(cid, 16);
            RetrievalLimits ended; ended.deadline = {};
            cache.begin_retrieval(ended);
            const auto before = cache.retrieval_stats();
            check(cache.prune({}, apply).evicted == 1);
            check(cache.retrieval_stats().reads == before.reads
                && cache.retrieval_stats().upstream_reads == before.upstream_reads);
        }
        {
            auto quota = limits; quota.lease_references = 0;
            BlockCache cache(directory.path / "disabled-leases", source, quota);
            const auto calls = source->calls;
            rejects(ErrorCode::Budget, [&] { cache.lease(ap); });
            check(source->calls == calls);
        }
        {
            const auto path = directory.path / "corrupt-ssd";
            BlockCache cache(path, source, limits);
            cache.read(aid, 32); // Deliberately warm RAM before corrupting SSD.
            const auto file_path = path / (hex(aid) + ".gksb");
            std::filesystem::permissions(file_path, std::filesystem::perms::owner_write,
                                          std::filesystem::perm_options::add);
            {
                std::fstream file(file_path, std::ios::binary | std::ios::in | std::ios::out);
                char first = 0; file.read(&first, 1); first ^= 1;
                file.seekp(0); file.write(&first, 1); check(bool(file));
            }
            const auto calls = source->calls;
            rejects(ErrorCode::Integrity, [&] { cache.lease(ap); });
            check(cache.stats().leased_references == 0 && cache.pinned().empty() && source->calls == calls);
            check(cache.prune({}, apply).evicted == 1);
            check(cache.read(aid, 32) == a);
        }
        std::cout << "Block retention passed: " << assertions << " deterministic assertions\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
