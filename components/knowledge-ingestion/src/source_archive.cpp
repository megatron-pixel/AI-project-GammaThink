#include "source_archive.hpp"

#include <zim/archive.h>
#include <zim/item.h>

namespace axiom::knowledge {

ZimSourceArchive::ZimSourceArchive(const std::filesystem::path& path,
                                 SourceArchiveLimits limits) : limits_(limits) {
    if (!limits_.archive_bytes || !limits_.document_bytes
        || limits_.document_bytes > maximum_object_bytes / 2) {
        throw Error(ErrorCode::Budget, "Invalid source archive limits");
    }
    if (path.extension() != ".zim" && path.extension() != ".zimaa") {
        throw Error(ErrorCode::Schema, "Expected a ZIM or first split ZIM file");
    }
    try {
        // This bounds retained decompressed cache, not transient libzim memory.
        zim::setClusterCacheMaxSize(16 * 1024 * 1024);
        archive_ = std::make_unique<zim::Archive>(path.string());
        archive_->setDirentCacheMaxSize(128);
        info_.logical_bytes = archive_->getFilesize();
        if (info_.logical_bytes > limits_.archive_bytes) {
            throw Error(ErrorCode::Budget, "Source archive size limit exceeded");
        }
        info_.id = "zim:" + static_cast<std::string>(archive_->getUuid());
        info_.declared_checksum = archive_->getChecksum();
        info_.version = info_.declared_checksum.empty()
            ? "no-declared-checksum" : "declared-checksum:" + info_.declared_checksum;
        validate_text(info_.id);
        validate_text(info_.version);
    } catch (const Error&) {
        throw;
    } catch (const std::exception& error) {
        throw Error(ErrorCode::Unavailable, std::string("Cannot open source archive: ") + error.what());
    }
}

ZimSourceArchive::~ZimSourceArchive() = default;

Object ZimSourceArchive::read(std::string_view entry, Visibility visibility) const {
    validate_text(entry, 4096);
    if (entry.empty()) throw Error(ErrorCode::Schema, "Empty source entry path");
    if (visibility != Visibility::Private && visibility != Visibility::Public) {
        throw Error(ErrorCode::Schema, "Invalid source visibility");
    }
    try {
        if (!archive_->hasEntryByPath(std::string(entry))) {
            throw Error(ErrorCode::Missing, "Source entry is absent");
        }
        const auto item = archive_->getEntryByPath(std::string(entry)).getItem(true);
        const auto size = item.getSize();
        if (size > limits_.document_bytes) {
            throw Error(ErrorCode::Budget, "Complete source document exceeds byte limit");
        }
        const auto mime = item.getMimetype();
        validate_text(mime, 256);
        if (!mime.starts_with("text/") && mime != "application/xhtml+xml"
            && mime != "application/mathml+xml") {
            throw Error(ErrorCode::Schema, "Unsupported source media type");
        }
        const auto bytes = item.getData(0, size);
        if (bytes.size() != size) {
            throw Error(ErrorCode::Integrity, "Source item read was incomplete");
        }
        Object result;
        result.kind = ObjectKind::Evidence;
        result.visibility = visibility;
        result.label = item.getTitle();
        result.format = "source.zim.raw.v1:" + mime;
        if (size) {
            const auto* begin = reinterpret_cast<const std::uint8_t*>(bytes.data());
            result.payload.assign(begin, begin + size);
        }
        result.sources.push_back({info_.id, info_.version, item.getPath(),
                                  "libzim-raw-entry-v1", digest(result.payload), 0, size});
        // Validate the canonical metadata and full resulting object budget.
        (void)encode_object(result);
        return result;
    } catch (const Error&) {
        throw;
    } catch (const std::exception& error) {
        throw Error(ErrorCode::Unavailable, std::string("Cannot read source item: ") + error.what());
    }
}

} // namespace axiom::knowledge
