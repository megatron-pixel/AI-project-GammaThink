#pragma once

#include "gks.hpp"

namespace axiom::knowledge {

class BlockSource;
enum class MountBackend : std::uint8_t { Local = 1, RemoteBlocks, RemoteQuery };
struct RegistryLimits {
    std::uint32_t cache_kib = 4096;
    std::uint64_t sql_instructions = 10000000;
    std::size_t versions = maximum_items;
    std::size_t aliases = maximum_items;
    std::size_t active_mounts = 16;
};
struct ArchiveVersion {
    std::string name, revision;
    BlockId root;
    bool operator==(const ArchiveVersion&) const = default;
};
struct MountBinding {
    ArchiveVersion archive;
    std::filesystem::path directory;
    MountBackend backend = MountBackend::Local;
    // Runtime capability key, not a URL, credential or network permission.
    std::string source{};
    bool operator==(const MountBinding&) const = default;
};
struct MountState {
    std::string alias;
    // Zero means never mounted. Unmount retains a positive tombstone generation.
    std::uint64_t generation = 0;
    std::optional<MountBinding> binding;
    bool operator==(const MountState&) const = default;
};

// Local, owner-thread catalogue. Names/revisions bind immutable roots, not
// factual truth or publisher trust. This is not an IPFS CID/signature registry.
class ArchiveRegistry {
public:
    explicit ArchiveRegistry(const std::filesystem::path& path, RegistryLimits limits = {});
    ~ArchiveRegistry();
    ArchiveRegistry(const ArchiveRegistry&) = delete;
    ArchiveRegistry& operator=(const ArchiveRegistry&) = delete;
    ArchiveVersion register_snapshot(const std::filesystem::path& directory, const BlockId& root);
    ArchiveVersion register_snapshot(std::shared_ptr<BlockSource> source, const BlockId& root);
    std::optional<ArchiveVersion> lookup(std::string_view name, std::string_view revision);
    MountState resolve(std::string_view alias);
    // One coherent read of active bindings, ordered by alias; never truncated.
    std::vector<MountState> mounts();
    // Compare-and-swap against the observed generation. A stale request throws
    // Unavailable; failed admission leaves the old binding/generation intact.
    MountState mount(std::string_view alias, std::string_view name, std::string_view revision,
                     const std::filesystem::path& directory, std::uint64_t expected_generation);
    MountState mount_remote(std::string_view alias, std::string_view name, std::string_view revision,
                            MountBackend backend, std::string_view source_key,
                            std::shared_ptr<BlockSource> admission_source, std::uint64_t expected_generation);
    MountState unmount(std::string_view alias, std::uint64_t expected_generation);
private:
    struct Close { void operator()(sqlite3* database) const noexcept; };
    std::unique_ptr<sqlite3, Close> database_;
    RegistryLimits limits_;
    ArchiveVersion register_version(const ArchiveVersion& archive);
    MountState bind(std::string_view alias, const ArchiveVersion& archive, std::string_view location,
                    MountBackend backend, std::uint64_t expected_generation);
};

} // namespace axiom::knowledge
