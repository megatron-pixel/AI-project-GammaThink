#pragma once

#include "archive_registry.hpp"
#include "gks_snapshot.hpp"

#include <functional>

namespace axiom::knowledge {

struct IndexQueryLimits {
    std::size_t nodes = 1024;
    std::size_t entries = 65536;
    std::size_t stored_bytes = 16 * 1024 * 1024;
    std::uint64_t work = 1000000;
    std::size_t phrase_tokens = 64;
};
struct RemoteQueryOptions;
struct DriveAccess {
    // Explicit runtime capabilities; never constructed from catalogue text.
    std::function<std::shared_ptr<BlockSource>(std::string_view)> blocks;
    std::function<RemoteQueryOptions(std::string_view)> queries;
};
class GksArchive final : public KnowledgeArchive {
public:
    GksArchive(const std::filesystem::path& directory, const BlockId& root, IndexQueryLimits limits = {});
    GksArchive(std::shared_ptr<BlockSource> source, const BlockId& root, IndexQueryLimits limits = {});
    const GksManifest& manifest() const { return snapshot_.manifest(); }
    std::optional<KnowledgeRecord> get(const ObjectId& id) override;
    ObjectPage query(const KnowledgeQuery& query, std::optional<ObjectId> after, std::size_t limit) override;
private:
    GksSnapshot snapshot_;
    IndexQueryLimits limits_;
};

// Mounts/immutable roots are pinned; the journal remains a live local overlay.
// The journal and this view must outlive any KnowledgeStream using the view.
class DriveView {
public:
    DriveView(Journal& journal, std::vector<MountState> mounts, IndexQueryLimits limits = {}, DriveAccess access = {});
    DriveView(const DriveView&) = delete;
    DriveView& operator=(const DriveView&) = delete;
    UnifiedKnowledge& knowledge() { return *knowledge_; }
    const std::vector<MountState>& mounts() const { return mounts_; }
private:
    std::vector<MountState> mounts_;
    JournalArchive journal_;
    std::vector<std::unique_ptr<KnowledgeArchive>> archives_;
    std::unique_ptr<UnifiedKnowledge> knowledge_;
};
class VirtualDrive {
public:
    VirtualDrive(Journal& journal, ArchiveRegistry& registry, IndexQueryLimits limits = {}, DriveAccess access = {})
        : journal_(journal), registry_(registry), limits_(limits), access_(std::move(access)) {}
    // New requests see current mounts. Older views/streams retain their roots.
    // Failed mount admission throws instead of returning a partial provider set.
    // Explicitly accepted catalogue mounts can join local registry bindings.
    // Duplicate aliases and aggregate overflow fail instead of hiding sources.
    std::unique_ptr<DriveView> open_view(std::span<const MountState> accepted_mounts = {});
private:
    Journal& journal_;
    ArchiveRegistry& registry_;
    IndexQueryLimits limits_;
    DriveAccess access_;
};

} // namespace axiom::knowledge
