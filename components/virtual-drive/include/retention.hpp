#pragma once

#include "gks_snapshot.hpp"

namespace axiom::knowledge {

enum class RetentionStatus { Complete, DependenciesUnavailable, ResourceLimited };
struct RetentionLimits {
    std::size_t objects = 512, edges = 16384, depth = 128;
    std::uint64_t record_bytes = 16 * 1024 * 1024;
};
struct RetentionPlan {
    RetentionStatus status = RetentionStatus::Complete;
    std::vector<ObjectId> roots, unavailable, optional_omissions;
    // Original full records: ordered joint premises, signs, assumptions,
    // provenance, contradictions and alternatives are never rewritten.
    std::vector<RetrievedObject> retained;
    // Positions in this exact pinned UnifiedKnowledge view, not catalogue IDs
    // or permission to fetch/publish another archive.
    std::vector<std::size_t> archives;
    std::size_t lookups = 0, edges = 0;
    std::uint64_t record_bytes = 0;
};

// A bounded read-only working-set plan. journal must be the view's local
// overlay; one coherent local snapshot covers the traversal. Follows ALL
// recorded references plus observed checked dependencies and certificates.
// Omissions concern optional working-memory references ONLY, never permission
// to delete CoK evidence, messages, assessments, snapshots or permanent pins.
// Missing/limited closure yields no omission proposal and never means false.
RetentionPlan plan_retention(UnifiedKnowledge& knowledge, Journal& journal,
                             std::span<const ObjectId> mandatory_roots,
                             std::span<const ObjectId> optional_candidates,
                             RetentionLimits limits = {});

} // namespace axiom::knowledge
