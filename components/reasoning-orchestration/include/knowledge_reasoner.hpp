#pragma once

#include "native_cok.hpp"
#include "retrieval.hpp"
#include <set>

namespace axiom::knowledge {

enum class KnowledgeRunStatus {
    Checked, Unresolved, Conflict, DependenciesUnavailable, RevalidationRequired, ResourceLimited
};
struct KnowledgeRunLimits {
    ResolutionLimits retrieval;
    NativeRunLimits reasoning;
    NativeCoKLimits materialization;
};
struct KnowledgeRun {
    KnowledgeRunStatus status = KnowledgeRunStatus::Unresolved;
    NativeRun execution;
    std::vector<ObjectId> unavailable, revalidation_required, conclusions, derivations;
    // One exact-object lookup per discovered dependency, not provider requests,
    // network round trips, empirical checks or semantic-search operations.
    std::size_t object_lookups = 0, imported_objects = 0;
    std::string reason;
};

// Formal native goal lookup across the existing unified drive. Repeated exact
// queries discover its dependency closure; missing/limited retrieval never
// becomes falsity. Stored requests remain scoped: this does not join independent
// source modules by guessed entity/predicate names or invent new premises.
// Snapshot assessment rows remain observations and are never made authoritative
// in the writable journal. Certificates are produced/replayed by the local core.
// A Requires prerequisite of kind Assumption also needs explicit acceptance on
// every run, including certificate replay. Importing it cannot grant acceptance.
KnowledgeRun run_knowledge_goal(UnifiedKnowledge& knowledge, Journal& journal,
    const ObjectId& goal, KnowledgeRunLimits limits = {},
    const std::set<ObjectId>& accepted_assumptions = {});

} // namespace axiom::knowledge
