#include "block_cache.hpp"

#include <algorithm>
#include <fcntl.h>
#include <functional>
#include <iostream>
#include <map>
#include <sys/stat.h>
#include <unistd.h>

namespace {
using namespace axiom::knowledge;
std::size_t assertions = 0;
void check(bool condition) {
    ++assertions;
    if (!condition) throw std::runtime_error("Block pin assertion " + std::to_string(assertions));
}
template<class Function> void rejects(ErrorCode expected, Function function) {
    try { function(); }
    catch (const Error& error) { check(error.code == expected); return; }
    throw std::runtime_error("Expected pin/budget rejection");
}
struct Directory {
    std::filesystem::path path;
    Directory() {
        auto pattern = (std::filesystem::temp_directory_path() / "gammathink-pins-XXXXXX").string();
        std::vector<char> value(pattern.begin(), pattern.end()); value.push_back(0);
        const auto* made = mkdtemp(value.data());
        if (!made) throw std::runtime_error("Cannot create pin fixture");
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
        if (found == blocks.end()) throw Error(ErrorCode::Unavailable, "Missing fixture");
        return found->second;
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
Bytes metadata(std::span<const BlockId> ids) {
    Bytes bytes{'G','K','P','N',1,0,0,0};
    for (unsigned i = 0; i < 4; ++i) bytes.push_back(static_cast<std::uint8_t>(ids.size() >> (8 * i)));
    for (const auto& id : ids) bytes.insert(bytes.end(), id.digest.begin(), id.digest.end());
    const auto digest = identify_block(bytes).digest;
    bytes.insert(bytes.end(), digest.begin(), digest.end());
    return bytes;
}
}
int main() {
    try {
        Directory directory;
        const Bytes a(32, 1), b(48, 2), c(64, 3);
        const auto aid = identify_block(a), bid = identify_block(b), cid = identify_block(c);
        const std::array<BlockId, 1> ap{aid}, bp{bid}, cp{cid};
        const std::array<BlockId, 2> bc{bid, cid};
        auto source = std::make_shared<Source>();
        source->blocks = {{aid,a},{bid,b},{cid,c}};
        BlockCacheLimits limits; limits.ram_bytes = 0; limits.ssd_bytes = 96; limits.ssd_entries = 3;
        const auto path = directory.path / "pins";
        const auto file = [&](const auto& root, const auto& id) { return root / (hex(id) + ".gksb"); };
        {
            BlockCache cache(path, source, limits);
            cache.pin(ap); check(cache.pinned() == std::vector<BlockId>{aid});
            check(cache.read(bid, 48) == b);
            rejects(ErrorCode::Budget, [&] { cache.pin(bc); });
            check(cache.pinned() == std::vector<BlockId>{aid});
            check(std::filesystem::exists(file(path, aid)) && std::filesystem::exists(file(path, bid)));
            check(!std::filesystem::exists(file(path, cid)));
        }
        {
            BlockCache cache(path, source, limits);
            check(cache.pinned() == std::vector<BlockId>{aid});
            cache.pin(cp);
            auto expected = std::vector<BlockId>{aid,cid}; std::sort(expected.begin(), expected.end());
            check(cache.pinned() == expected && cache.stats().ssd_bytes == 96);
            check(!std::filesystem::exists(file(path, bid)));
            cache.set_offline(true);
            const auto before = source->calls;
            check(cache.read(aid, 32) == a && cache.read(cid, 64) == c);
            rejects(ErrorCode::Unavailable, [&] { cache.read(bid, 48); });
            check(source->calls == before);
            cache.set_offline(false);
            check(cache.read(bid, 48) == b); // All SSD space is pinned: bypass.
            check(!std::filesystem::exists(file(path, bid)) && cache.stats().ssd_bytes == 96);
            cache.unpin(cp);
            check(cache.pinned() == std::vector<BlockId>{aid});
            cache.read(bid, 48);
            check(!std::filesystem::exists(file(path, cid)) && std::filesystem::exists(file(path, aid)));
        }
        {
            BlockCache cache(path, {}, limits);
            check(cache.pinned() == std::vector<BlockId>{aid});
            cache.unpin(ap); check(cache.pinned().empty());
        }
        {
            BlockCache cache(path, source, limits);
            check(cache.pinned().empty());
            cache.pin(ap);
            write_file(path / ".pins.pending", metadata(bp));
            rejects(ErrorCode::Storage, [&] { cache.pin(bp); });
            rejects(ErrorCode::Storage, [&] { cache.pinned(); });
            rejects(ErrorCode::Storage, [&] { cache.begin_retrieval(); });
        }
        {
            BlockCache cache(path, source, limits);
            check(cache.pinned() == std::vector<BlockId>{aid});
            check(!std::filesystem::exists(path / ".pins.pending"));
            cache.read(cid, 64); check(std::filesystem::exists(file(path, aid)));
        }
        {
            // A completed rename is the pin transaction's commit point.
            write_file(path / ".pins.pending", metadata(cp));
            std::filesystem::rename(path / ".pins.pending", path / ".pins");
            BlockCache cache(path, {}, limits);
            check(cache.pinned() == std::vector<BlockId>{cid});
        }
        {
            const auto missing = directory.path / "missing";
            BlockCache cache(missing, {}, limits);
            rejects(ErrorCode::Unavailable, [&] { cache.pin(ap); });
            check(cache.pinned().empty());
            std::array<BlockId, 4> excessive{aid,aid,aid,aid};
            rejects(ErrorCode::Budget, [&] { cache.pin(excessive); });
            rejects(ErrorCode::Budget, [&] { cache.unpin(excessive); });
        }
        {
            auto tiny = limits; tiny.ssd_bytes = 16;
            BlockCache cache(directory.path / "tiny", source, tiny);
            rejects(ErrorCode::Budget, [&] { cache.pin(ap); });
            check(cache.pinned().empty() && cache.stats().ssd_entries == 0);
        }
        {
            const auto warm = directory.path / "warm-corrupt";
            BlockCache cache(warm, source);
            cache.read(aid, 32);
            check(cache.stats().ram_entries == 1);
            check(chmod(file(warm, aid).c_str(), 0600) == 0);
            auto damaged = a; damaged.front() ^= 1;
            write_file(file(warm, aid), damaged);
            rejects(ErrorCode::Integrity, [&] { cache.pin(ap); });
            check(cache.pinned().empty());
            write_file(file(warm, aid), a);
            cache.read(aid, 32);
            std::filesystem::remove(file(warm, aid));
            rejects(ErrorCode::Unavailable, [&] { cache.pin(ap); });
            check(cache.pinned().empty());
        }
        const auto budget_path = directory.path / "budgets";
        {
            BlockCache cache(budget_path, source);
            RetrievalLimits budget; budget.reads = 2; budget.returned_bytes = 64;
            budget.upstream_reads = 1; budget.upstream_bytes = 32;
            cache.begin_retrieval(budget);
            check(cache.read(aid, 32) == a && cache.read(aid, 32) == a);
            rejects(ErrorCode::Budget, [&] { cache.read(aid, 32); });
            auto usage = cache.retrieval_stats();
            check(usage.reads == 2 && usage.returned_bytes == 64 && usage.upstream_reads == 1 && usage.upstream_bytes == 32);
            budget = {}; budget.returned_bytes = 31;
            cache.begin_retrieval(budget);
            rejects(ErrorCode::Budget, [&] { cache.read(aid, 32); });
            check(cache.retrieval_stats().returned_bytes == 0);
            budget = {}; budget.upstream_reads = 0;
            cache.begin_retrieval(budget);
            const auto before = source->calls;
            rejects(ErrorCode::Budget, [&] { cache.read(bid, 48); });
            cache.set_offline(true);
            rejects(ErrorCode::Unavailable, [&] { cache.read(bid, 48); });
            cache.set_offline(false);
            check(source->calls == before && cache.retrieval_stats().reads == 2);
            budget = {}; budget.upstream_bytes = 31;
            cache.begin_retrieval(budget);
            rejects(ErrorCode::Budget, [&] { cache.read(bid, 48); });
            check(cache.retrieval_stats().upstream_bytes == 31);
            rejects(ErrorCode::Budget, [&] { cache.read(bid, 48); });
            check(source->calls == before + 1);
            budget = {}; budget.deadline = std::chrono::steady_clock::time_point::min();
            cache.begin_retrieval(budget);
            rejects(ErrorCode::Budget, [&] { cache.read(aid, 32); });
            budget = {}; budget.deadline += std::chrono::hours(1);
            rejects(ErrorCode::Budget, [&] { cache.begin_retrieval(budget); });
            budget = {}; budget.upstream_bytes = 64;
            cache.begin_retrieval(budget);
            const auto absent = identify_block(Bytes{9});
            rejects(ErrorCode::Unavailable, [&] { cache.read(absent, 16); });
            rejects(ErrorCode::Unavailable, [&] { cache.read(absent, 48); });
            check(cache.retrieval_stats().upstream_bytes == 64);
            const auto failures = source->calls;
            rejects(ErrorCode::Budget, [&] { cache.read(absent, 1); });
            check(source->calls == failures);
            budget = {}; budget.upstream_bytes = 32;
            cache.begin_retrieval(budget);
            source->blocks[absent] = a;
            rejects(ErrorCode::Integrity, [&] { cache.read(absent, 32); });
            check(cache.retrieval_stats().upstream_bytes == 32);
            source->blocks.erase(absent);
            const auto lifetime = cache.stats().source_reads;
            cache.begin_retrieval();
            check(cache.retrieval_stats().reads == 0 && cache.stats().source_reads == lifetime);
        }
        // Schema, checksum and canonical ordering checks on persisted pins.
        const auto malformed = directory.path / "malformed";
        {
            BlockCache cache(malformed, source); cache.read(aid, 32); cache.read(bid, 48);
        }
        auto corrupt = metadata(ap); corrupt.back() ^= 1;
        write_file(malformed / ".pins", corrupt);
        rejects(ErrorCode::Integrity, [&] { BlockCache rejected(malformed); });
        corrupt = metadata(ap); corrupt[4] = 2;
        write_file(malformed / ".pins", corrupt);
        rejects(ErrorCode::Schema, [&] { BlockCache rejected(malformed); });
        std::array<BlockId, 2> duplicates{aid,aid};
        write_file(malformed / ".pins", metadata(duplicates));
        rejects(ErrorCode::Integrity, [&] { BlockCache rejected(malformed); });
        auto reversed = std::array<BlockId, 2>{aid,bid};
        std::sort(reversed.begin(), reversed.end(), std::greater<BlockId>());
        write_file(malformed / ".pins", metadata(reversed));
        rejects(ErrorCode::Integrity, [&] { BlockCache rejected(malformed); });
        write_file(malformed / ".pins", metadata(cp));
        rejects(ErrorCode::Unavailable, [&] { BlockCache rejected(malformed); });
        std::filesystem::remove(malformed / ".pins");
        std::filesystem::create_symlink(file(malformed, aid), malformed / ".pins");
        rejects(ErrorCode::Storage, [&] { BlockCache rejected(malformed); });
        std::cout << "block pin tests passed: " << assertions << " deterministic assertions\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
