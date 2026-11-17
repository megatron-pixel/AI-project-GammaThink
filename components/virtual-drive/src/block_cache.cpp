#include "block_cache.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace axiom::knowledge {
namespace {
constexpr std::size_t stored_limit = 16 + gks_max_block_bytes + 65536;
void require(bool value, ErrorCode code, const char* message) {
    if (!value) throw Error(code, message);
}
struct Descriptor {
    int fd = -1;
    ~Descriptor() { if (fd >= 0) close(fd); }
    Descriptor() = default;
    explicit Descriptor(int value) : fd(value) {}
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
};
void private_file(const struct stat& status, nlink_t maximum_links = 1) {
    require(S_ISREG(status.st_mode) && status.st_uid == geteuid()
        && (status.st_mode & 077) == 0 && status.st_nlink >= 1 && status.st_nlink <= maximum_links,
        ErrorCode::Permission, "Cache file must be private, regular and owned");
    require(status.st_size >= 0 && static_cast<std::uint64_t>(status.st_size) <= stored_limit,
            ErrorCode::Budget, "Cache block extent exceeds format bounds");
}
void verify(const BlockId& id, const Bytes& bytes, std::size_t maximum_bytes) {
    require(bytes.size() <= maximum_bytes && bytes.size() <= stored_limit,
            ErrorCode::Budget, "Cache source exceeded block read bounds");
    require(identify_block(bytes) == id, ErrorCode::Integrity, "Cache block hash mismatch");
}
}

struct BlockLease::State {
    std::atomic<bool> alive{true};
    std::mutex mutex;
    std::map<BlockId, std::size_t> references;
    std::size_t total = 0;
};
BlockLease::BlockLease(std::weak_ptr<State> owner, std::vector<BlockId> ids) noexcept
    : owner_(std::move(owner)), ids_(std::move(ids)) {}
BlockLease::~BlockLease() { release(); }
bool BlockLease::active() const noexcept {
    const auto state = owner_.lock();
    return state && state->alive.load();
}
BlockLease::BlockLease(BlockLease&& other) noexcept
    : owner_(std::move(other.owner_)), ids_(std::move(other.ids_)) {}
BlockLease& BlockLease::operator=(BlockLease&& other) noexcept {
    if (this != &other) {
        release(); owner_ = std::move(other.owner_); ids_ = std::move(other.ids_);
    }
    return *this;
}
void BlockLease::release() noexcept {
    if (const auto state = owner_.lock()) {
        std::lock_guard lock(state->mutex);
        for (const auto& id : ids_) {
            const auto found = state->references.find(id);
            if (--found->second == 0) state->references.erase(found);
        }
        state->total -= ids_.size();
    }
    owner_.reset();
    std::vector<BlockId>().swap(ids_);
}

struct BlockCache::Impl {
    struct Disk { std::size_t size; std::uint64_t used; };
    struct Memory { Bytes bytes; std::uint64_t used; };
    Descriptor directory, lock;
    std::shared_ptr<BlockSource> upstream;
    BlockCacheLimits limits;
    BlockCacheStats counts;
    std::map<BlockId, Disk> disk;
    std::map<BlockId, Memory> memory;
    std::set<BlockId> pins;
    std::optional<std::set<BlockId>> pending_pins;
    std::shared_ptr<BlockLease::State> leases = std::make_shared<BlockLease::State>();
    RetrievalLimits retrieval;
    RetrievalStats session;
    std::uint64_t clock = 0;
    bool failed = false, offline = false;
    ~Impl() { leases->alive.store(false); }

    Impl(const std::filesystem::path& path, std::shared_ptr<BlockSource> source, BlockCacheLimits budget)
        : upstream(std::move(source)), limits(budget) {
        require(limits.ram_bytes <= 1024ULL * 1024 * 1024 && limits.ram_entries <= 65536
            && limits.ssd_bytes <= 1024ULL * 1024 * 1024 * 1024 && limits.ssd_entries <= 65536
            && limits.lease_references <= 65536,
            ErrorCode::Budget, "Cache limits exceed supported bounds");
        validate_text(path.string());
        require(mkdir(path.c_str(), 0700) == 0 || errno == EEXIST,
                ErrorCode::Storage, "Cannot create cache directory");
        directory.fd = open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        require(directory.fd >= 0, ErrorCode::Storage, "Cannot open cache directory");
        struct stat status{};
        require(fstat(directory.fd, &status) == 0, ErrorCode::Storage, "Cannot inspect cache directory");
        require(status.st_uid == geteuid() && (status.st_mode & 077) == 0,
                ErrorCode::Permission, "Cache directory must be private and owned");
        lock.fd = openat(directory.fd, ".lock", O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, 0600);
        require(lock.fd >= 0 && fstat(lock.fd, &status) == 0, ErrorCode::Storage, "Cannot open cache lock");
        private_file(status);
        require(status.st_size == 0, ErrorCode::Integrity, "Invalid cache lock extent");
        require(flock(lock.fd, LOCK_EX | LOCK_NB) == 0, ErrorCode::Storage, "Cache is already open");
        // Only this reserved staging name is recovered. Never sweep unknown
        // files or follow links in the caller's dedicated directory.
        for (const auto* name : {".pending", ".pins.pending"}) {
            if (fstatat(directory.fd, name, &status, AT_SYMLINK_NOFOLLOW) == 0) {
                private_file(status, std::string_view(name) == ".pending" ? 2 : 1);
                require(unlinkat(directory.fd, name, 0) == 0, ErrorCode::Storage, "Cannot recover cache staging file");
            } else require(errno == ENOENT, ErrorCode::Storage, "Cannot inspect cache staging file");
        }
        const auto scan_fd = openat(directory.fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        require(scan_fd >= 0, ErrorCode::Storage, "Cannot scan cache directory");
        std::unique_ptr<DIR, decltype(&closedir)> scan(fdopendir(scan_fd), closedir);
        if (!scan) { close(scan_fd); throw Error(ErrorCode::Storage, "Cannot enumerate cache directory"); }
        for (;;) {
            errno = 0;
            const auto* entry = readdir(scan.get());
            if (!entry) {
                require(errno == 0, ErrorCode::Storage, "Incomplete cache directory scan"); break;
            }
            const std::string name(entry->d_name);
            if (name == "." || name == ".." || name == ".lock" || name == ".pins") continue;
            require(name.size() == 69 && name.ends_with(".gksb"),
                    ErrorCode::Schema, "Unexpected file in dedicated cache directory");
            const auto id = parse_block_id(name.substr(0, 64));
            require(name == hex(id) + ".gksb", ErrorCode::Schema, "Noncanonical cache filename");
            require(fstatat(directory.fd, name.c_str(), &status, AT_SYMLINK_NOFOLLOW) == 0,
                    ErrorCode::Storage, "Cannot inspect cached block");
            private_file(status);
            const auto size = static_cast<std::size_t>(status.st_size);
            require(limits.ssd_bytes && disk.size() < limits.ssd_entries && size <= limits.ssd_bytes - counts.ssd_bytes,
                    ErrorCode::Budget, "Existing cache exceeds configured quota");
            disk.emplace(id, Disk{size, 0}); counts.ssd_bytes += size;
        }
        load_pins();
        begin_retrieval({});
    }
    void checkpoint() const {
        require(!failed, ErrorCode::Storage, "Cache write failed; reopen required");
        require(std::chrono::steady_clock::now() < retrieval.deadline, ErrorCode::Budget, "Retrieval deadline exceeded");
    }
    void begin_retrieval(RetrievalLimits budget) {
        require(!failed, ErrorCode::Storage, "Cache write failed; reopen required");
        require(budget.reads <= 1000000 && budget.upstream_reads <= 1000000
            && budget.returned_bytes <= 1024ULL * 1024 * 1024 * 1024
            && budget.upstream_bytes <= 1024ULL * 1024 * 1024 * 1024
            && budget.deadline <= std::chrono::steady_clock::now() + std::chrono::minutes(5),
            ErrorCode::Budget, "Unsupported retrieval session limits");
        retrieval = budget; session = {};
    }
    void load_pins() {
        Descriptor input(openat(directory.fd, ".pins", O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK));
        if (input.fd < 0 && errno == ENOENT) return;
        require(input.fd >= 0, ErrorCode::Storage, "Cannot open pin metadata");
        struct stat status{};
        require(fstat(input.fd, &status) == 0, ErrorCode::Storage, "Cannot inspect pin metadata");
        private_file(status);
        require(status.st_size >= 44 && static_cast<std::uint64_t>(status.st_size) <= 44 + 32 * limits.ssd_entries,
                ErrorCode::Budget, "Pin metadata exceeds entry limit");
        Bytes bytes(static_cast<std::size_t>(status.st_size));
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            const auto size = ::read(input.fd, bytes.data() + offset, bytes.size() - offset);
            require(size > 0, ErrorCode::Storage, "Incomplete pin metadata read");
            offset += static_cast<std::size_t>(size);
        }
        std::uint8_t extra;
        require(::read(input.fd, &extra, 1) == 0, ErrorCode::Integrity, "Pin metadata changed during read");
        const Bytes prefix{'G','K','P','N',1,0,0,0};
        require(std::equal(prefix.begin(), prefix.end(), bytes.begin()), ErrorCode::Schema, "Unsupported pin metadata");
        std::uint32_t count = 0;
        for (unsigned i = 0; i < 4; ++i) count |= static_cast<std::uint32_t>(bytes[8 + i]) << (8 * i);
        require(count <= limits.ssd_entries && bytes.size() == 44 + 32ULL * count,
                ErrorCode::Integrity, "Pin metadata count mismatch");
        const auto digest = identify_block(std::span(bytes).first(bytes.size() - 32)).digest;
        require(std::equal(digest.begin(), digest.end(), bytes.end() - 32),
                ErrorCode::Integrity, "Pin metadata hash mismatch");
        for (std::uint32_t i = 0; i < count; ++i) {
            BlockId id;
            std::copy_n(bytes.begin() + 12 + 32 * i, 32, id.digest.begin());
            require(pins.empty() || *pins.rbegin() < id, ErrorCode::Integrity, "Pin IDs are not canonical");
            require(disk.contains(id), ErrorCode::Unavailable, "Pinned block is missing");
            pins.insert(id);
        }
    }
    void commit_pins(const std::set<BlockId>& desired) {
        checkpoint();
        Bytes bytes{'G','K','P','N',1,0,0,0};
        for (unsigned i = 0; i < 4; ++i) bytes.push_back(static_cast<std::uint8_t>(desired.size() >> (8 * i)));
        for (const auto& id : desired) bytes.insert(bytes.end(), id.digest.begin(), id.digest.end());
        const auto digest = identify_block(bytes).digest;
        bytes.insert(bytes.end(), digest.begin(), digest.end());
        try {
            struct stat status{};
            if (fstatat(directory.fd, ".pins", &status, AT_SYMLINK_NOFOLLOW) == 0) private_file(status);
            else require(errno == ENOENT && pins.empty(), ErrorCode::Integrity, "Pin metadata disappeared");
            Descriptor output(openat(directory.fd, ".pins.pending",
                O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
            require(output.fd >= 0, ErrorCode::Storage, "Cannot stage pin metadata");
            std::size_t offset = 0;
            while (offset < bytes.size()) {
                const auto size = write(output.fd, bytes.data() + offset, bytes.size() - offset);
                require(size > 0, ErrorCode::Storage, "Incomplete pin metadata write");
                offset += static_cast<std::size_t>(size);
            }
            require(fchmod(output.fd, 0400) == 0 && fsync(output.fd) == 0,
                    ErrorCode::Storage, "Cannot finalize pin metadata");
            require(renameat(directory.fd, ".pins.pending", directory.fd, ".pins") == 0 && fsync(directory.fd) == 0,
                    ErrorCode::Storage, "Cannot commit pin metadata");
        } catch (...) { failed = true; throw; }
    }
    void fetch_retained(std::span<const BlockId> ids, std::size_t maximum_bytes) {
        for (const auto& id : ids) {
            // Retention certifies the SSD copy, not a warm RAM substitute.
            if (disk.contains(id)) {
                if (const auto cached = memory.find(id); cached != memory.end()) {
                    counts.ram_bytes -= cached->second.bytes.capacity();
                    memory.erase(cached);
                }
            }
            const auto bytes = read(id, maximum_bytes);
            if (!disk.contains(id)) store(id, bytes);
            require(disk.contains(id), ErrorCode::Budget, "Pinned blocks cannot fit SSD quota");
        }
    }
    std::set<BlockId> protected_union(std::span<const BlockId> ids) {
        auto desired = pins;
        desired.insert(ids.begin(), ids.end());
        std::lock_guard lock(leases->mutex);
        for (const auto& [id, count] : leases->references) {
            (void)count;
            desired.insert(id);
        }
        require(desired.size() <= limits.ssd_entries, ErrorCode::Budget, "Retained union exceeds entry quota");
        return desired;
    }
    void pin(std::span<const BlockId> ids) {
        checkpoint();
        require(!pending_pins, ErrorCode::Permission, "Retention admission is already active");
        require(ids.size() <= limits.ssd_entries, ErrorCode::Budget, "Pin input exceeds entry quota");
        auto desired = pins;
        desired.insert(ids.begin(), ids.end());
        pending_pins.emplace(protected_union(ids));
        try {
            fetch_retained(ids, stored_limit);
            commit_pins(desired);
            pins.swap(desired); pending_pins.reset();
        } catch (...) { pending_pins.reset(); throw; }
    }
    BlockLease lease(std::span<const BlockId> ids, std::size_t maximum_bytes) {
        checkpoint();
        require(!pending_pins, ErrorCode::Permission, "Retention admission is already active");
        require(!ids.empty() && ids.size() <= limits.ssd_entries, ErrorCode::Budget, "Lease input exceeds entry quota");
        maximum_bytes = std::min(maximum_bytes, stored_limit);
        require(maximum_bytes > 0, ErrorCode::Budget, "Invalid lease block byte limit");
        std::set<BlockId> unique(ids.begin(), ids.end());
        std::vector<BlockId> selected(unique.begin(), unique.end());
        {
            std::lock_guard lock(leases->mutex);
            require(selected.size() <= limits.lease_references - leases->total,
                    ErrorCode::Budget, "Lease reference budget exceeded");
        }
        pending_pins.emplace(protected_union(selected));
        try {
            fetch_retained(selected, maximum_bytes);
            std::lock_guard lock(leases->mutex);
            auto updated = leases->references;
            for (const auto& id : selected) ++updated[id];
            leases->references.swap(updated);
            leases->total += selected.size();
            pending_pins.reset();
            return BlockLease(leases, std::move(selected));
        } catch (...) { pending_pins.reset(); throw; }
    }
    void unpin(std::span<const BlockId> ids) {
        checkpoint();
        require(!pending_pins, ErrorCode::Permission, "Retention admission is already active");
        require(ids.size() <= limits.ssd_entries, ErrorCode::Budget, "Unpin input exceeds entry quota");
        auto desired = pins;
        for (const auto& id : ids) desired.erase(id);
        commit_pins(desired); pins.swap(desired);
    }
    std::uint64_t tick() {
        require(clock < std::numeric_limits<std::uint64_t>::max(), ErrorCode::Budget, "Cache access counter exhausted");
        return ++clock;
    }
    void remember(const BlockId& id, const Bytes& bytes) {
        if (!limits.ram_entries || !limits.ram_bytes || bytes.size() > limits.ram_bytes) return;
        Bytes copy(bytes.begin(), bytes.end());
        const auto size = copy.capacity();
        if (size > limits.ram_bytes) return;
        while (memory.size() >= limits.ram_entries || size > limits.ram_bytes - counts.ram_bytes) {
            const auto oldest = std::min_element(memory.begin(), memory.end(),
                [](const auto& a, const auto& b) { return a.second.used < b.second.used; });
            counts.ram_bytes -= oldest->second.bytes.capacity();
            memory.erase(oldest);
        }
        memory.emplace(id, Memory{std::move(copy), tick()}); counts.ram_bytes += size;
    }
    void store(const BlockId& id, const Bytes& bytes) {
        if (!limits.ssd_entries || !limits.ssd_bytes || bytes.size() > limits.ssd_bytes) return;
        const auto& protected_ids = pending_pins ? *pending_pins : pins;
        std::lock_guard lock(leases->mutex);
        const auto retained = [&](const BlockId& key) {
            return protected_ids.contains(key) || leases->references.contains(key);
        };
        std::uint64_t protected_bytes = 0; std::size_t protected_entries = 0;
        for (const auto& [key, entry] : disk) if (retained(key)) {
            protected_bytes += entry.size; ++protected_entries;
        }
        if (protected_entries >= limits.ssd_entries || bytes.size() > limits.ssd_bytes - protected_bytes) return;
        while (disk.size() >= limits.ssd_entries || bytes.size() > limits.ssd_bytes - counts.ssd_bytes) {
            auto oldest = disk.end();
            for (auto entry = disk.begin(); entry != disk.end(); ++entry)
                if (!retained(entry->first) && (oldest == disk.end() || entry->second.used < oldest->second.used))
                    oldest = entry;
            require(oldest != disk.end(), ErrorCode::Budget, "No evictable cache entry");
            const auto name = hex(oldest->first) + ".gksb";
            struct stat status{};
            require(fstatat(directory.fd, name.c_str(), &status, AT_SYMLINK_NOFOLLOW) == 0,
                    ErrorCode::Storage, "Cannot inspect eviction candidate");
            private_file(status);
            require(static_cast<std::size_t>(status.st_size) == oldest->second.size,
                    ErrorCode::Integrity, "Cache eviction candidate changed");
            require(unlinkat(directory.fd, name.c_str(), 0) == 0, ErrorCode::Storage, "Cannot evict cached block");
            counts.ssd_bytes -= oldest->second.size; disk.erase(oldest);
        }
        // Reserve metadata before publishing a file. A failed write poisons
        // this handle; reopen recovers the one fixed staging file and rescans.
        disk.emplace(id, Disk{bytes.size(), tick()}); counts.ssd_bytes += bytes.size();
        try {
            Descriptor output(openat(directory.fd, ".pending",
                O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
            require(output.fd >= 0, ErrorCode::Storage, "Cannot create cache staging file");
            std::size_t offset = 0;
            while (offset < bytes.size()) {
                const auto size = write(output.fd, bytes.data() + offset, bytes.size() - offset);
                require(size > 0, ErrorCode::Storage, "Incomplete cache write");
                offset += static_cast<std::size_t>(size);
            }
            require(fchmod(output.fd, 0400) == 0 && fsync(output.fd) == 0,
                    ErrorCode::Storage, "Cannot finalize cached block");
            const auto name = hex(id) + ".gksb";
            require(linkat(directory.fd, ".pending", directory.fd, name.c_str(), 0) == 0,
                    ErrorCode::Storage, "Cannot publish cached block");
            require(unlinkat(directory.fd, ".pending", 0) == 0 && fsync(directory.fd) == 0,
                    ErrorCode::Storage, "Cannot commit cached block directory");
        } catch (...) { failed = true; throw; }
    }
    Bytes read(const BlockId& id, std::size_t maximum_bytes) {
        checkpoint();
        require(session.reads < retrieval.reads, ErrorCode::Budget, "Retrieval read quota exceeded");
        ++session.reads;
        maximum_bytes = static_cast<std::size_t>(std::min<std::uint64_t>(
            std::min(maximum_bytes, stored_limit), retrieval.returned_bytes - session.returned_bytes));
        const auto finish = [&](Bytes bytes) {
            checkpoint(); session.returned_bytes += bytes.size(); return bytes;
        };
        if (const auto hit = memory.find(id); hit != memory.end()) {
            verify(id, hit->second.bytes, maximum_bytes);
            hit->second.used = tick();
            if (const auto stored = disk.find(id); stored != disk.end()) stored->second.used = hit->second.used;
            ++counts.ram_hits;
            return finish(hit->second.bytes);
        }
        if (const auto hit = disk.find(id); hit != disk.end()) {
            require(hit->second.size <= maximum_bytes, ErrorCode::Budget, "Cached block exceeds read budget");
            const auto name = hex(id) + ".gksb";
            Descriptor input(openat(directory.fd, name.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK));
            require(input.fd >= 0, errno == ENOENT ? ErrorCode::Unavailable : ErrorCode::Storage, "Cannot read cached block");
            struct stat status{};
            require(fstat(input.fd, &status) == 0, ErrorCode::Storage, "Cannot inspect cached block");
            private_file(status);
            require(static_cast<std::size_t>(status.st_size) == hit->second.size,
                    ErrorCode::Integrity, "Cached block extent changed");
            Bytes bytes(hit->second.size);
            std::size_t offset = 0;
            while (offset < bytes.size()) {
                const auto size = ::read(input.fd, bytes.data() + offset, bytes.size() - offset);
                require(size > 0, ErrorCode::Storage, "Incomplete cached block read");
                offset += static_cast<std::size_t>(size);
            }
            std::uint8_t extra;
            require(::read(input.fd, &extra, 1) == 0, ErrorCode::Integrity, "Cached block grew during read");
            verify(id, bytes, maximum_bytes);
            hit->second.used = tick(); ++counts.ssd_hits; remember(id, bytes);
            return finish(std::move(bytes));
        }
        require(!offline && upstream, ErrorCode::Unavailable, "Block is not cached and upstream retrieval is disabled");
        require(session.upstream_reads < retrieval.upstream_reads, ErrorCode::Budget, "Retrieval upstream request quota exceeded");
        maximum_bytes = static_cast<std::size_t>(std::min<std::uint64_t>(
            maximum_bytes, retrieval.upstream_bytes - session.upstream_bytes));
        require(maximum_bytes > 0, ErrorCode::Budget, "Retrieval upstream byte quota exceeded");
        require(counts.source_reads < std::numeric_limits<std::uint64_t>::max(),
                ErrorCode::Budget, "Cache source counter exhausted");
        ++counts.source_reads; ++session.upstream_reads;
        session.upstream_bytes += maximum_bytes;
        auto bytes = upstream->read(id, maximum_bytes);
        verify(id, bytes, maximum_bytes);
        checkpoint();
        session.upstream_bytes -= maximum_bytes - bytes.size();
        store(id, bytes); remember(id, bytes);
        return finish(std::move(bytes));
    }
};
BlockCache::BlockCache(const std::filesystem::path& path, std::shared_ptr<BlockSource> upstream, BlockCacheLimits limits)
    : impl_(std::make_unique<Impl>(path, std::move(upstream), limits)) {}
BlockCache::~BlockCache() = default;
Bytes BlockCache::read(const BlockId& id, std::size_t maximum_bytes) { return impl_->read(id, maximum_bytes); }
BlockCacheStats BlockCache::stats() const {
    auto result = impl_->counts;
    result.ram_entries = impl_->memory.size(); result.ssd_entries = impl_->disk.size();
    std::lock_guard lock(impl_->leases->mutex);
    result.leased_blocks = impl_->leases->references.size();
    result.leased_references = impl_->leases->total;
    return result;
}
void BlockCache::set_offline(bool value) { impl_->offline = value; }
void BlockCache::begin_retrieval(RetrievalLimits limits) { impl_->begin_retrieval(limits); }
RetrievalStats BlockCache::retrieval_stats() const { return impl_->session; }
void BlockCache::pin(std::span<const BlockId> ids) { impl_->pin(ids); }
void BlockCache::unpin(std::span<const BlockId> ids) { impl_->unpin(ids); }
BlockLease BlockCache::lease(std::span<const BlockId> ids, std::size_t maximum_bytes) {
    return impl_->lease(ids, maximum_bytes);
}
CachePrunePage BlockCache::prune(std::optional<BlockId> after, CachePruneLimits limits) {
    require(!impl_->failed, ErrorCode::Storage, "Cache write failed; reopen required");
    require(limits.examined > 0 && limits.examined <= 65536 && limits.candidates > 0
        && limits.candidates <= limits.examined, ErrorCode::Budget, "Invalid cache prune limits");
    std::lock_guard lock(impl_->leases->mutex);
    const auto& protected_ids = impl_->pending_pins ? *impl_->pending_pins : impl_->pins;
    auto disk = after ? impl_->disk.upper_bound(*after) : impl_->disk.begin();
    auto memory = after ? impl_->memory.upper_bound(*after) : impl_->memory.begin();
    CachePrunePage page;
    while ((disk != impl_->disk.end() || memory != impl_->memory.end())
        && page.examined < limits.examined && page.evictable.size() < limits.candidates) {
        const auto id = memory == impl_->memory.end() || (disk != impl_->disk.end() && disk->first < memory->first)
            ? disk->first : memory->first;
        if (disk != impl_->disk.end() && disk->first == id) ++disk;
        if (memory != impl_->memory.end() && memory->first == id) ++memory;
        ++page.examined; page.next = id;
        if (protected_ids.contains(id) || impl_->leases->references.contains(id)) continue;
        page.evictable.push_back(id);
        if (!limits.apply) continue;
        if (const auto stored = impl_->disk.find(id); stored != impl_->disk.end()) {
            const auto name = hex(id) + ".gksb";
            struct stat status{};
            require(fstatat(impl_->directory.fd, name.c_str(), &status, AT_SYMLINK_NOFOLLOW) == 0,
                    ErrorCode::Storage, "Cannot inspect prune candidate");
            private_file(status);
            require(static_cast<std::size_t>(status.st_size) == stored->second.size,
                    ErrorCode::Integrity, "Cache prune candidate changed");
            require(unlinkat(impl_->directory.fd, name.c_str(), 0) == 0, ErrorCode::Storage, "Cannot prune cache block");
            page.ssd_bytes += stored->second.size; impl_->counts.ssd_bytes -= stored->second.size;
            impl_->disk.erase(stored);
        }
        if (const auto cached = impl_->memory.find(id); cached != impl_->memory.end()) {
            page.ram_bytes += cached->second.bytes.capacity();
            impl_->counts.ram_bytes -= cached->second.bytes.capacity();
            impl_->memory.erase(cached);
        }
        ++page.evicted;
    }
    page.complete = disk == impl_->disk.end() && memory == impl_->memory.end();
    if (page.complete) page.next.reset();
    if (page.ssd_bytes) require(fsync(impl_->directory.fd) == 0, ErrorCode::Storage, "Cannot commit cache cleanup");
    return page;
}
std::vector<BlockId> BlockCache::pinned() const {
    require(!impl_->failed, ErrorCode::Storage, "Cache write failed; reopen required");
    return {impl_->pins.begin(), impl_->pins.end()};
}

} // namespace axiom::knowledge
