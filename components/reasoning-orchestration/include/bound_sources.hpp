#pragma once

#include "source_bindings.hpp"
#include "knowledge_reasoner.hpp"

namespace axiom::knowledge {

struct BoundSourceGoal {
    ObjectId module;
    ProblemGoal goal;
};
class BoundSourceRequest {
public:
    const ProblemRequest& request() const { return request_; }
    const Object& record() const { return record_; }
private:
    ProblemRequest request_;
    Object record_;
    BoundSourceRequest(ProblemRequest request, Object record);
    friend BoundSourceRequest admit_bound_sources(std::span<const SourceBindingScope>,
        const ActiveSourceBindings&, const BoundSourceGoal&, ProblemLimits);
};

// Structural lowering under explicitly accepted identity assumptions, never
// lexical joining. Each original finite variable domain is preserved by bounded
// grounding before combination. The original native IR/core/checker are reused.
// This profile requires the same complete host unit registry across scopes.
BoundSourceRequest admit_bound_sources(std::span<const SourceBindingScope> scopes,
    const ActiveSourceBindings& bindings, const BoundSourceGoal& goal, ProblemLimits limits = {});

struct BoundSourceRun {
    KnowledgeRun outcome;
    std::optional<ObjectId> request;
    std::size_t scopes = 0, members = 0;
};
BoundSourceRun run_bound_sources(UnifiedKnowledge& knowledge, Journal& journal,
    const ObjectId& binding, const std::map<ObjectId, ProblemContext>& host_contexts,
    const std::set<ObjectId>& accepted_assumptions, const BoundSourceGoal& goal,
    KnowledgeRunLimits limits = {});

} // namespace axiom::knowledge
