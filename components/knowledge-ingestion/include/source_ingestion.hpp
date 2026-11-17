#pragma once

#include "source_archive.hpp"
#include "source_compiler.hpp"
#include "journal.hpp"
#include "problem_sources.hpp"

namespace axiom::knowledge {

struct SourceTranslation {
    // Captured by the host before translation, not selected by the model.
    ObjectId source;
    std::string context_snapshot, archive_dsl;
};
struct SourceIngestionLimits {
    SourceKnowledgeLimits storage;
    ProblemLimits admission;
    ProblemSelectorMode selectors = ProblemSelectorMode::OffsetV1;
};
struct SourceIngestion {
    SourceCompilationKind kind = SourceCompilationKind::Candidate;
    ObjectId source;
    std::optional<ObjectId> context, module;
    std::vector<ObjectId> members;
    std::size_t new_objects = 0, encoded_bytes = 0;
    std::string reason;
};

// Complete raw ZIM entry -> existing goal-free archive DSL -> admitted scoped
// module and members in the private journal. Identity, context and aggregate
// storage preflight precede writes. No model, solver, inferred premise,
// authoritative assessment, publication or lossy text extraction occurs.
SourceIngestion ingest_zim_entry(const ZimSourceArchive& archive, std::string_view entry,
    Journal& journal, const ProblemContext& context, const SourceTranslation& translation,
    SourceIngestionLimits limits = {});

} // namespace axiom::knowledge
