#pragma once

#include "conversation.hpp"

namespace axiom::context {

enum class DocumentKind : std::uint8_t { Project = 1, Preference, External, Summary };
enum class DocumentAttribution : std::uint8_t { Explicit = 1, Derived };
struct ContextDocument {
    std::string project, session, key, uri, timestamp, content;
    DocumentKind kind = DocumentKind::External;
    DocumentAttribution attribution = DocumentAttribution::Explicit;
    std::uint64_t revision = 0;
    bool active = true;
    knowledge::Digest content_hash{};
    bool operator==(const ContextDocument&) const = default;
};
struct DocumentExcerpt {
    ContextDocument source; // content is empty; the original is retrieved by revision.
    std::uint64_t offset = 0;
    std::uint64_t source_bytes = 0;
    std::string content;
    knowledge::Digest content_hash{};
};
struct DocumentPage {
    std::vector<DocumentExcerpt> excerpts;
    bool complete = true;
};
struct DocumentLimits {
    std::size_t source_bytes = 1048576, chunk_bytes = 2048;
    ConversationLimits database;
};

// Private context data, never the knowledge journal or an instruction source.
// Host-selected imports only. Empty session means shared within this project.
// Immutable revisions retain superseded/withdrawn originals and attribution.
class ContextDocuments {
public:
    explicit ContextDocuments(const std::filesystem::path& path, DocumentLimits limits = {});
    ~ContextDocuments();
    ContextDocuments(const ContextDocuments&) = delete;
    ContextDocuments& operator=(const ContextDocuments&) = delete;
    ContextDocument save(ContextDocument document, std::uint64_t expected_revision);
    std::optional<ContextDocument> get(std::string_view project, std::string_view session,
                                     std::string_view key, std::uint64_t revision = 0);
    // Literal unicode61 phrase, latest active revisions only; never another
    // session/project. Whole exact UTF-8 chunks; no incomplete claim is a fact.
    DocumentPage search(std::string_view project, std::string_view session, std::string_view phrase,
                        std::size_t limit = 16, std::size_t content_bytes = 32768);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace axiom::context
