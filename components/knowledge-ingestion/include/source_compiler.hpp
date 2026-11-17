#pragma once

#include "knowledge.hpp"
#include "problem_semantic.hpp"

namespace axiom::knowledge {

enum class SourceCompilationKind { Candidate, Clarify, Unsupported };

struct SourceCompilation {
    SourceCompilationKind kind = SourceCompilationKind::Candidate;
    std::optional<Object> candidate;
    // The existing shared {theory,goals} JSON profile, not a new meaning schema.
    std::string parsed_json, reason;
};

// Translation-only boundary. The interpreter supplies the normal archive DSL
// and the identity of its input document. No model or reasoner is called here.
// Preserve the whole scoped module; do not split index-bound tuples into
// freestanding assertions. Candidate claims have no authoritative assessment.
// This checks serialization and source binding, not source-meaning fidelity or
// native semantic admission. Both remain necessary before reasoning/reuse.
SourceCompilation compile_source(const Object& source, const ObjectId& translated_source,
                                  std::string_view archive_dsl);

// Replays serialization/source binding and validates the existing native theory
// against a host-owned context. The one source view must be the exact complete
// input bytes and provenance; registry definitions stay host-owned. No solver,
// empirical assessment or execution goal is introduced.
void validate_source_candidate(const Object& source, const Object& candidate,
                                const ProblemContext& context, ProblemLimits limits = {},
                                ProblemSelectorMode mode = ProblemSelectorMode::OffsetV1);

struct SourceKnowledgeLimits {
    // Context and module count too; the caller already owns the raw Evidence.
    std::size_t objects = 512;
    std::size_t encoded_bytes = 16 * 1024 * 1024;
};
struct CompiledSourceKnowledge {
    Object context, module;
    // Claim: binding/equation; Rule: signed finite-Horn rule. These are scoped
    // pointers into the complete module, never standalone index-bound tuples.
    std::vector<Object> members;
};

CompiledSourceKnowledge materialize_source(const Object& source, const Object& candidate,
    const ProblemContext& context, SourceKnowledgeLimits budget = {},
    ProblemLimits limits = {}, ProblemSelectorMode mode = ProblemSelectorMode::OffsetV1);

// Returns an existing native tuple, still scoped to the supplied full module
// and context. Verifies the locator, context binding and native representation.
// Content/representation validity is not source fidelity or empirical truth.
std::string source_member_json(const Object& module, const Object& context, const Object& member);

// Admit requested work against exactly one archived source scope, using the
// existing parsed-theory/ground-atom contract. The registry/context is supplied
// independently by the host, not made authoritative by archive metadata.
// This neither adds a goal to the knowledge archive nor executes reasoning,
// merges identically named entities across scopes, or certifies source truth.
ProblemRequest source_query_request(const Object& source, const Object& module, const Object& context,
    const ProblemContext& host_context, const Atom& goal, ProblemLimits limits = {});
ProblemRequest source_query_request(const Object& source, const Object& module, const Object& context,
    const ProblemContext& host_context, const ProblemGoal& goal, ProblemLimits limits = {});
// Goal-free native theory for bounded structural lowering. The proposal's goal
// is unused; callers must supply and admit actual host-requested work separately.
ProblemProposal source_theory_proposal(const Object& source, const Object& module, const Object& context,
    const ProblemContext& host_context, ProblemLimits limits = {});

} // namespace axiom::knowledge
