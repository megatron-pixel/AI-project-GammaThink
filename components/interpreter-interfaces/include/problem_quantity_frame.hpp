#pragma once

#include "problem_transaction.hpp"

namespace axiom {

struct QuantityFrameSpan {
    std::string first;
    std::optional<std::string> last;
};
enum class QuantityFrameScopeKind { Global, Qualified, Unresolved };
struct QuantityFrameQualifier {
    QuantityFrameSpan key, value;
};
struct QuantityFrameScope {
    QuantityFrameScopeKind kind = QuantityFrameScopeKind::Unresolved;
    std::vector<QuantityFrameQualifier> qualifiers;
};
struct QuantityFrameIdentity {
    // Reusing a canonical occurrence is an explicit identity choice. Equal text
    // at different occurrences is not an identity link.
    QuantityFrameSpan subject, property;
    QuantityFrameScope scope;
    std::string unit;
    std::vector<QuantityFrameSpan> supports;
};
struct QuantityFrameObservation {
    QuantityFrameIdentity quantity;
    QuantityFrameSpan numeral;
    std::optional<QuantityFrameSpan> unit_span;
    QuantityPrecision precision = QuantityPrecision::Unspecified;
    std::optional<QuantityFrameSpan> precision_span;
};
struct ProblemQuantityFrame {
    std::string snapshot;
    std::vector<QuantityFrameObservation> observations;
    QuantityFrameIdentity target;
};

// Restricted opt-in quantity retrieval component, not a general interpretation
// mode. No equations, identity inference, missing-scope defaults or execution.
// Citation existence and ordinary admission do not certify English entailment.
CompiledProblemTransaction compile_problem_quantity_frame(
    const ProblemQuantityFrame& frame, const ProblemSourceCatalog& catalog);

} // namespace axiom
