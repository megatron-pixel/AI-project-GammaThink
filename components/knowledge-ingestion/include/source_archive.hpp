#pragma once

#include "knowledge.hpp"

#include <filesystem>
#include <memory>

namespace zim { class Archive; }

namespace axiom::knowledge {

struct SourceArchiveLimits {
    std::uint64_t archive_bytes = 1ULL << 40;
    std::size_t document_bytes = 1024 * 1024;
};

struct SourceArchiveInfo {
    std::string id, version;
    // An unverified publisher/container declaration, not a computed file hash.
    std::string declared_checksum;
    std::uint64_t logical_bytes = 0;
};

// Read-only source access, independent of a full-text index. Construct another
// instance to replace an archive. Catalogue/mount versioning is a separate layer.
class ZimSourceArchive {
public:
    explicit ZimSourceArchive(const std::filesystem::path& path,
                              SourceArchiveLimits limits = {});
    ~ZimSourceArchive();
    ZimSourceArchive(const ZimSourceArchive&) = delete;
    ZimSourceArchive& operator=(const ZimSourceArchive&) = delete;

    const SourceArchiveInfo& info() const { return info_; }
    // Exact, complete item bytes, including original markup and mathematical
    // structure. Oversized items fail; search excerpts are never substituted.
    // The payload's media type is retained in its format discriminator.
    // Evidence is unassessed and private unless explicitly marked otherwise.
    Object read(std::string_view entry, Visibility visibility = Visibility::Private) const;

private:
    SourceArchiveLimits limits_;
    SourceArchiveInfo info_;
    std::unique_ptr<zim::Archive> archive_;
};

} // namespace axiom::knowledge
