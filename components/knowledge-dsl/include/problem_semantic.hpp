#pragma once

#include "problem.hpp"
#include "quantified_equations.hpp"

#include <stdexcept>

namespace axiom {

enum class ProblemDeclineKind { Clarify, Unsupported };
enum class ProblemSelectorMode { OffsetV1, QuoteV1 };
struct ProblemSelectorError : std::runtime_error {
    using std::runtime_error::runtime_error;
};
struct ProblemDecline : std::runtime_error {
    ProblemDeclineKind kind;
    ProblemDecline(ProblemDeclineKind value, std::string reason)
        : std::runtime_error(std::move(reason)), kind(value) {}
};

// A proposal cannot supply context, units, limits, answers, proof steps or formal origins.
// Successful parsing certifies representation only, not English meaning.
ProblemRequest parse_problem_proposal(const std::string& json, const ProblemContext& context,
    ProblemLimits limits = {}, ProblemSelectorMode mode = ProblemSelectorMode::OffsetV1);
// Exactly the existing eight theory arrays, without a goal or status envelope.
// Uses the same tuple, citation, unit, scope and finite-rule admission checks.
// Does not return an executable request or certify source-meaning fidelity.
void validate_problem_theory_proposal(const std::string& json, const ProblemContext& context,
    ProblemLimits limits = {}, ProblemSelectorMode mode = ProblemSelectorMode::OffsetV1);
// Same admitted eight-array theory, with no requested goal supplied by input.
// The returned proposal's goal is unused until the host explicitly supplies one.
ProblemProposal parse_problem_theory_proposal(const std::string& json, const ProblemContext& context,
    ProblemLimits limits = {}, ProblemSelectorMode mode = ProblemSelectorMode::OffsetV1);
struct ParsedQuantifiedTheory {
    ProblemProposal base;
    std::vector<QuantifiedEquation> schemas;
};
// Explicit version-2 shared theory: the same eight arrays plus
// quantified_equations. Input comes from the strict version-2 archive decoder.
// Resolves citations using the existing host selectors, without inventing a
// goal or instantiating/solving a declaration. The resulting schemas still
// require instantiate_equations for binder, dimension and ground admission.
ParsedQuantifiedTheory parse_quantified_problem_theory_proposal(const std::string& json,
    const ProblemContext& context, ProblemLimits limits = {},
    ProblemSelectorMode mode = ProblemSelectorMode::OffsetV1);
std::string serialize_problem_proposal(const ProblemRequest& request,
    ProblemSelectorMode mode = ProblemSelectorMode::OffsetV1);
ProblemContext parse_problem_context(const std::string& json);
std::string serialize_problem_context(const ProblemContext& context);
std::string problem_semantic_input(const ProblemContext& context);
std::string problem_proposal_schema(ProblemSelectorMode mode = ProblemSelectorMode::OffsetV1);
std::string interpret_problem_json(const ProblemContext& context,
    ProblemSelectorMode mode = ProblemSelectorMode::OffsetV1);

inline constexpr std::string_view problem_semantic_instructions = R"(Interpret the supplied question and evidence into an unsolved problem proposal, not an answer or a proof plan. All offsets and lengths are UTF-8 bytes in the host evidence views. Units are host indices; never redefine them. Numerals must select complete literal numbers in evidence; never calculate or invent a numeral. Mark approximate, bounded or unspecified precision honestly. Distinguish entities and sorted scope qualifiers (including time, run and population); unresolved scope is not a wildcard. Cite supporting spans for every binding, equation and rule. A citation records your interpretation, not proven source truth.
Return exactly one JSON object. Proposal keys are entities, quantities, numbers, bindings, nodes, equations, predicates, rules, goal.
Tuples: span=[view,offset,length,locator-or-null]; scope=[resolved,[[key,value],...]]; quantity=[property,entity,scope,unit]; number=[numeral-span,unit-span-or-null,unit,precision,qualifier-span-or-null], precision is exact/approximate/bound/unspecified; binding=[quantity,number,[support-span,...]].
Nodes are ["q",quantity], ["n",number], ["neg",earlier-node], or ["add"/"sub"/"mul"/"div",earlier-left,earlier-right]. Equation=[left-node,right-node,scope,guard-atom-or-null,[support-span,...]]. Do not evaluate the nodes.
Predicate=[name,arity]. Atom=[predicate,negative,[term,...]], term=["c",entity] or ["v",variable]. Rule=[variable-count,head-atom,[premise-atom,...],[support-span,...]]. Variables must be range-restricted. Constants are the entities array. All assertions and rules must be supported by the supplied question or sources. Goal=["q",quantity,unit] or ["atom",ground-atom].
If missing information prevents interpretation return {"status":"clarify","reason":"name the actual missing information"}. If the finite equations and Horn rules cannot express the task return {"status":"unsupported","reason":"name the actual unsupported feature"}. Do not replace a difficult question with unrelated matrices, widths, facts or calculations.)";

inline std::string problem_semantic_prompt(ProblemSelectorMode mode) {
    std::string result(problem_semantic_instructions);
    if (mode == ProblemSelectorMode::OffsetV1) return result;
    if (mode != ProblemSelectorMode::QuoteV1) throw std::runtime_error("Unknown problem selector mode");
    const std::string offsets = "All offsets and lengths are UTF-8 bytes in the host evidence views.";
    result.replace(result.find(offsets),offsets.size(),"Copy exact evidence quotations; C++ locates their bytes.");
    const std::string span = "span=[view,offset,length,locator-or-null]";
    result.replace(result.find(span),span.size(),"span=[view,locator-or-null,exact-quote,enclosing-quote-or-null]");
    result += "\nquote-v1: each quotation must match exactly once in the chosen locator or view. "
        "If the selected text repeats, supply an enclosing quotation that matches exactly once, "
        "and select text occurring exactly once inside it. Copy bytes exactly; no normalization, "
        "occurrence numbers or offsets. The enclosure only disambiguates location; it is not an extra premise.";
    return result;
}

} // namespace axiom
