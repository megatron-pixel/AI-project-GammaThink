#pragma once

#include "source_compiler.hpp"
#include "retrieval.hpp"

#include <map>
#include <set>

namespace axiom::knowledge {

enum class SourceSymbolKind { Entity = 1, Predicate, Quantity };
struct SourceSymbol {
    ObjectId module;
    SourceSymbolKind kind = SourceSymbolKind::Entity;
    std::uint32_t index = 0;
    auto operator<=>(const SourceSymbol&) const = default;
};
struct SourceIdentityBinding {
    SourceSymbol left, right;
    bool operator==(const SourceIdentityBinding&) const = default;
};
struct SourceBindingScope {
    Object source, module, context;
    ProblemContext host_context;
};
struct SourceBindingRevalidation final : Error {
    ObjectId object;
    explicit SourceBindingRevalidation(ObjectId id)
        : Error(ErrorCode::Unavailable, "Source binding dependencies require revalidation"), object(id) {}
};

// Explicit conditional equivalences, never a lexical join. The resulting
// private Assumption is unassessed; constructing or storing it does not accept it.
// Quantity identity also requires explicit entity identity in this same batch,
// matching resolved scopes and compatible host-owned unit dimensions.
Object propose_source_bindings(std::span<const SourceBindingScope> scopes,
    std::span<const SourceIdentityBinding> bindings, std::string reason);

class ActiveSourceBindings {
public:
    const ObjectId& assumption() const { return assumption_; }
    const std::vector<SourceIdentityBinding>& bindings() const { return bindings_; }
    const std::vector<ObservedAssessment>& observations() const { return observations_; }
private:
    ObjectId assumption_;
    std::vector<SourceIdentityBinding> bindings_;
    std::vector<ObservedAssessment> observations_;
    ActiveSourceBindings(ObjectId assumption, std::vector<SourceIdentityBinding> bindings,
                         std::vector<ObservedAssessment> observations);
    friend ActiveSourceBindings activate_source_bindings(UnifiedKnowledge&, Journal&, const ObjectId&,
        const std::map<ObjectId, ProblemContext>&, const std::set<ObjectId>&, ResolutionLimits);
};

// Reopens the full immutable contract and source scopes. The host independently
// supplies each registry/context and explicitly accepted assumption identity.
// No semantic truth, native proof, assessment, import or publication is produced.
// A subsequent combined native request must retain this assumption and its
// observed prerequisites; activation is not a permanent execution capability.
ActiveSourceBindings activate_source_bindings(UnifiedKnowledge& knowledge, Journal& journal,
    const ObjectId& binding, const std::map<ObjectId, ProblemContext>& host_contexts,
    const std::set<ObjectId>& accepted_assumptions, ResolutionLimits limits = {});

} // namespace axiom::knowledge
