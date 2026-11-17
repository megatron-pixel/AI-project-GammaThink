#include "context.hpp"

namespace axiom::context {
ContextSources retrieve_document_context(ConversationMemory& memory, ContextDocuments& documents,
    std::string_view session, std::span<const std::uint64_t> instructions,
    std::string_view history_phrase, std::string_view document_phrase, ContextReadLimits limits) {
    auto result = retrieve_context(memory, session, instructions, history_phrase, limits);
    result.documents_enabled = true;
    if (!document_phrase.empty() && limits.historical_messages > 0) {
        auto page = documents.search(result.project, session, document_phrase,
            limits.historical_messages, limits.content_bytes);
        result.documents = std::move(page.excerpts);
        result.documents_complete = page.complete;
    }
    return result;
}
} // namespace axiom::context
