#pragma once

#include "conversation.hpp"
#include "context_documents.hpp"

#include <array>

namespace axiom::context {

enum class ContextLayer : std::uint8_t {
    Current = 1, Instruction, Constraint, Recent, Summary, History, Project, Preference, Document
};
struct ContextReadLimits {
    std::size_t recent_messages = 32, historical_messages = 16;
    std::size_t instructions = 32, constraints = 64, content_bytes = 262144;
};
struct ContextSources {
    Message current;
    std::vector<Message> instructions, recent, history;
    std::vector<Constraint> constraints;
    std::optional<Summary> summary;
    bool recent_complete = true, history_complete = true;
    std::string project;
    std::vector<DocumentExcerpt> documents;
    bool documents_enabled = false, documents_complete = true;
};

// Host-selected instruction IDs, never inferred from retrieved text.
// Reads all three layers from one SQLite snapshot. Latest message must be
// the current user request; retrieval never searches other sessions.
ContextSources retrieve_context(ConversationMemory& memory, std::string_view session,
    std::span<const std::uint64_t> instruction_ids = {}, std::string_view history_phrase = {},
    ContextReadLimits limits = {});
// Separate private document database, one bounded search snapshot. Interpretation
// rechecks both stores after generation; no cross-database atomicity is implied.
ContextSources retrieve_document_context(ConversationMemory& memory, ContextDocuments& documents,
    std::string_view session, std::span<const std::uint64_t> instruction_ids,
    std::string_view history_phrase, std::string_view document_phrase, ContextReadLimits limits = {});

struct ContextEntry {
    ContextLayer layer = ContextLayer::Current;
    Role role = Role::User;
    std::uint64_t message = 0, revision = 0, first_message = 0, last_message = 0;
    std::string key, timestamp, content;
    knowledge::Digest content_hash{};
    std::optional<DocumentExcerpt> document = {};
};
struct ContextPackage {
    std::string session;
    std::vector<ContextEntry> entries;
    // Counts known candidates omitted for budgets; window flags separately
    // describe retrieval limits. Index zero is unused.
    std::array<std::size_t, 10> omitted{};
    bool recent_complete = true, history_complete = true;
    bool documents_enabled = false, documents_complete = true;
    std::string json;
    std::size_t input_tokens = 0;
};
struct ContextBudget {
    std::size_t input_tokens = 4096, serialized_bytes = 262144, candidates = 256;
};
// Must deterministically count the actual model/template input including
// framing and tokenizer special tokens. No byte/word estimate is substituted.
using ContextTokenCounter = std::function<std::size_t(std::string_view)>;

// Current request, explicit instructions and all active pins are mandatory.
// Optional whole records are considered by priority, newest messages first.
// JSON carries numeric priority and source IDs; history is never promoted to
// current instructions. Model adherence still needs semantic evaluation.
ContextPackage assemble_context(const ContextSources& sources, const ContextTokenCounter& count,
                                ContextBudget budget = {});
// Count a complete host-specific interpreter envelope, not just its conversation
// JSON. The candidate includes all attribution and omission metadata.
ContextPackage assemble_context_package(const ContextSources& sources,
    const std::function<std::size_t(const ContextPackage&)>& count, ContextBudget budget = {});

} // namespace axiom::context
