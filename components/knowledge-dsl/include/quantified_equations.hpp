#pragma once

#include "problem.hpp"

#include <stdexcept>

namespace axiom {

class QuantifiedResourceLimit final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct QuantifiedQuantity {
    std::string property;
    Term entity;
    ProblemScope scope;
    std::uint32_t unit = 0;
    bool operator==(const QuantifiedQuantity&) const = default;
};

// All declared variables are universally bound. Quantity nodes index this
// schema's quantities; number nodes index the unchanged input theory's numbers.
// A guard is an antecedent, not an assertion that the guard holds.
struct QuantifiedEquation {
    std::uint32_t variables = 0;
    std::vector<QuantifiedQuantity> quantities;
    std::vector<ProblemNode> nodes;
    std::uint32_t left = 0, right = 0;
    ProblemScope scope;
    std::optional<Atom> guard;
    ProblemOrigin origin;
    bool operator==(const QuantifiedEquation&) const = default;
};

struct EquationInstantiation {
    std::uint32_t schema = 0, equation = 0;
    std::vector<std::uint32_t> substitution, quantities, nodes;
    bool operator==(const EquationInstantiation&) const = default;
};

struct QuantifiedExpansion {
    ProblemRequest request;
    std::vector<EquationInstantiation> instances;
    std::uint64_t work = 0;
    bool operator==(const QuantifiedExpansion&) const = default;
};

// Sound universal elimination over the explicitly declared current entities,
// not a claim that this finite catalogue exhausts the universe. Schemas must
// remain available for instantiation when additional entities are introduced.
// This compiles declarations only: no guard proof, solving or goal creation.
QuantifiedExpansion instantiate_equations(const ProblemRequest& base,
    const std::vector<QuantifiedEquation>& schemas, std::uint64_t work_limit = 4096);

// Replays exact schema/substitution/ground-request identity. Native numerical
// results still require the existing independent verify_problem_result check.
bool verify_equation_expansion(const ProblemRequest& base,
    const std::vector<QuantifiedEquation>& schemas, const QuantifiedExpansion& expansion,
    std::uint64_t work_limit = 4096);

} // namespace axiom
