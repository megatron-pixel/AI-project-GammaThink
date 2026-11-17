#pragma once

#include "problem_transaction.hpp"

#include <functional>

namespace axiom {

struct SignedDecisionRequest {
    // JSON schema covers the complete {"value":...} response. plan_so_far
    // includes the current, possibly incomplete item; selected_history includes
    // every earlier decision as {"role":...,"value":...}, including branch and
    // list stops. catalog is the unfiltered catalog.input(); question is the
    // public question views joined by newlines. JSON-valued members are strings.
    std::string role, question, response_schema, plan_so_far, catalog, selected_history;
};

// Initial transport-independent callback contract, NOT a trained
// SignedPolicyModel. Scripted training and inference use this same controller.
// Every call returns exactly {"value":...}; no solver feedback or retries.
using SignedDecisionPolicy = std::function<std::string(const SignedDecisionRequest&)>;

// branch: theory/clarify/unsupported; reason: nonempty text on decline.
// Every list uses <path>.next = append/stop. Each span uses <path>.first
// (source handle) and <path>.last (source handle or null for a single handle).
// Scalars use their field path: predicates.arity, scopes.kind,
// <atom-path>.predicate/scope/negative and <atom-path>.terms.kind/ref.
// Atom paths: clauses.head, clauses.premises, goals.atom.
// List paths: entities, predicates, scopes, clauses, goals; each one's supports;
// scopes.qualifiers; clauses.variables; clauses.premises; each atom's terms.
// Span paths: entities.anchor, predicates.anchor, <list>.supports,
// scopes.qualifiers.key/value, clauses.variables.
// All nested lists, spans, refs, signs, arities and kinds are atomic choices.
// Declaration references are zero-based append positions, never native IDs.
// Invalid selections abort immediately; unresolved scopes decline Clarify.
CompiledProblemBatch interpret_problem_signed_decisions(
    const ProblemSourceCatalog& catalog, const SignedDecisionPolicy& policy);

} // namespace axiom
