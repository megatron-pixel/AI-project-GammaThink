#pragma once

#include "knowledge_reasoner.hpp"
#include "source_compiler.hpp"

#include <chrono>

namespace axiom::knowledge {

struct SourceLogicLimits {
    ResolutionLimits retrieval;
    NativeCoKLimits materialization;
    axiom::Limits search;
    std::size_t rule_queries = 128;
    std::uint64_t work = 1000000;
    std::chrono::milliseconds operation_time{30000};
};
struct SourceLogicRun {
    KnowledgeRun outcome;
    std::optional<ObjectId> request;
    std::size_t rule_queries = 0;
    std::uint64_t work = 0;
};

// One explicit archived source scope and one host-requested ground atom.
// The source/theory/context use the existing archive DSL and parsed theory.
// The backward C++ search consumes scoped rule-member lookups as needed,
// preserves ordered AND premises and distinct alternatives, and uses the
// original native relational certificate checker. No neural call, cross-scope
// name matching or imported authoritative assessment is involved.
// The current profile is finite Horn logic, not quantity/equation search.
SourceLogicRun run_source_logic(UnifiedKnowledge& knowledge, Journal& journal,
    const ObjectId& module, const ProblemContext& host_context, const Atom& goal,
    SourceLogicLimits limits = {});

struct SourceQuantityRun {
    KnowledgeRun outcome;
    std::optional<ObjectId> request;
    std::size_t member_queries = 0;
};
// Complete bounded module profile: retrieves all scoped bindings, equations and
// guard rules before executing the unchanged native quantity core/checker.
// Unlike finite-Horn backward search, this does not claim lazy equation search.
SourceQuantityRun run_source_quantity(UnifiedKnowledge& knowledge, Journal& journal,
    const ObjectId& module, const ProblemContext& host_context, const ProblemQuantityGoal& goal,
    KnowledgeRunLimits limits = {});

} // namespace axiom::knowledge
