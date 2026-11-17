#include "quantified_equations.hpp"

#include <algorithm>
#include <type_traits>

namespace axiom {
namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void capacity(bool condition, const char* message) {
    if (!condition) throw QuantifiedResourceLimit(message);
}
}

QuantifiedExpansion instantiate_equations(const ProblemRequest& base,
    const std::vector<QuantifiedEquation>& schemas, std::uint64_t work_limit) {
    require(work_limit > 0 && work_limit <= 4000000 && schemas.size() <= 64,
        "Invalid quantified expansion budget");
    (void)admit_problem_theory(base);
    require(schemas.empty() || !base.proposal.entities.empty(), "Quantified expansion requires declared entities");
    std::uint64_t total_work = 0;
    std::size_t equation_count = base.proposal.equations.size(), node_count = base.proposal.nodes.size();
    std::vector<std::uint64_t> counts;
    for (const auto& schema : schemas) {
        require(schema.variables > 0 && schema.variables <= 4 &&
            schema.quantities.size() <= 64 && schema.nodes.size() <= 256 &&
            schema.left < schema.nodes.size() && schema.right < schema.nodes.size(),
            "Invalid quantified equation bounds");
        const auto scope_bound = [](const ProblemScope& scope) {
            require(scope.qualifiers.size() <= 16, "Quantified scope exceeds its bound");
            for (const auto& [key, value] : scope.qualifiers)
                require(key.size() <= 128 && value.size() <= 128, "Quantified qualifier exceeds its bound");
        };
        scope_bound(schema.scope);
        require(schema.origin.label.size() <= 1024 && schema.origin.supports.size() <= 16,
            "Quantified origin exceeds its bound");
        const auto term = [&](const Term& value) {
            require((value.kind == TermKind::Variable && value.index < schema.variables) ||
                (value.kind == TermKind::Constant && value.index < base.proposal.entities.size()),
                "Unbound quantified equation term");
        };
        for (const auto& quantity : schema.quantities) {
            term(quantity.entity);
            scope_bound(quantity.scope);
            require(quantity.property.size() <= 128,
                "Quantified quantity declaration exceeds its bound");
        }
        if (schema.guard) {
            require(schema.guard->predicate < base.proposal.logic.predicates.size() &&
                schema.guard->terms.size() == base.proposal.logic.predicates[schema.guard->predicate].arity,
                "Invalid quantified guard");
            for (const auto& value : schema.guard->terms) term(value);
        }
        for (std::size_t i = 0; i < schema.nodes.size(); ++i) {
            std::visit([&](const auto& node) {
                using Node = std::decay_t<decltype(node)>;
                if constexpr (std::is_same_v<Node, ProblemQuantityNode>)
                    require(node.quantity < schema.quantities.size(), "Invalid quantified quantity node");
                else if constexpr (std::is_same_v<Node, ProblemNumberNode>)
                    require(node.number < base.proposal.numbers.size(), "Invalid quantified number node");
                else if constexpr (std::is_same_v<Node, ProblemNegateNode>)
                    require(node.operand < i, "Invalid quantified expression order");
                else
                    require(node.left < i && node.right < i, "Invalid quantified expression order");
            }, schema.nodes[i]);
        }
        const auto width = 1 + schema.quantities.size() + schema.nodes.size() +
            (schema.guard ? schema.guard->terms.size() : 0);
        std::uint64_t instances = 1;
        for (std::uint32_t i = 0; i < schema.variables; ++i) {
            capacity(instances <= work_limit / base.proposal.entities.size(),
                "Quantified substitution budget exceeded");
            instances *= base.proposal.entities.size();
        }
        capacity(instances <= (work_limit - total_work) / width, "Quantified expansion work exceeded");
        capacity(instances <= 64 - equation_count,
            "Quantified equation count exceeded");
        capacity(instances <= (256 - node_count) / schema.nodes.size(),
            "Quantified node count exceeded");
        equation_count += instances;
        node_count += instances * schema.nodes.size();
        total_work += instances * width;
        counts.push_back(instances);
    }
    QuantifiedExpansion result{base, {}, total_work};
    auto& output = result.request.proposal;
    for (std::size_t schema_index = 0; schema_index < schemas.size(); ++schema_index) {
        const auto& schema = schemas[schema_index];
        for (std::uint64_t instance = 0; instance < counts[schema_index]; ++instance) {
            EquationInstantiation mapping;
            mapping.schema = static_cast<std::uint32_t>(schema_index);
            mapping.substitution.resize(schema.variables);
            auto remainder = instance;
            for (std::size_t v = schema.variables; v > 0; --v) {
                mapping.substitution[v - 1] = static_cast<std::uint32_t>(remainder % output.entities.size());
                remainder /= output.entities.size();
            }
            const auto entity = [&](const Term& term) {
                return term.kind == TermKind::Variable ? mapping.substitution[term.index] : term.index;
            };
            for (const auto& prototype : schema.quantities) {
                ProblemQuantity quantity{prototype.property, entity(prototype.entity), prototype.scope, prototype.unit};
                const auto found = std::find_if(output.quantities.begin(), output.quantities.end(),
                    [&](const auto& current) {
                        return current.property == quantity.property && current.entity == quantity.entity &&
                            current.scope == quantity.scope;
                    });
                if (found != output.quantities.end()) {
                    require(found->unit == quantity.unit, "Quantified quantity slot has a different unit");
                    mapping.quantities.push_back(static_cast<std::uint32_t>(found - output.quantities.begin()));
                } else {
                    capacity(output.quantities.size() < 64, "Quantified quantity count exceeded");
                    mapping.quantities.push_back(static_cast<std::uint32_t>(output.quantities.size()));
                    output.quantities.push_back(std::move(quantity));
                }
            }
            for (const auto& prototype : schema.nodes) {
                capacity(output.nodes.size() < 256, "Quantified node count exceeded");
                const auto node = std::visit([&](const auto& value) -> ProblemNode {
                    using Node = std::decay_t<decltype(value)>;
                    if constexpr (std::is_same_v<Node, ProblemQuantityNode>)
                        return ProblemQuantityNode{mapping.quantities[value.quantity]};
                    else if constexpr (std::is_same_v<Node, ProblemNumberNode>) return value;
                    else if constexpr (std::is_same_v<Node, ProblemNegateNode>)
                        return ProblemNegateNode{mapping.nodes[value.operand]};
                    else
                        return ProblemBinaryNode{value.operation, mapping.nodes[value.left], mapping.nodes[value.right]};
                }, prototype);
                mapping.nodes.push_back(static_cast<std::uint32_t>(output.nodes.size()));
                output.nodes.push_back(node);
            }
            auto guard = schema.guard;
            if (guard)
                for (auto& term : guard->terms) term = {TermKind::Constant, entity(term)};
            capacity(output.equations.size() < 64, "Quantified equation count exceeded");
            mapping.equation = static_cast<std::uint32_t>(output.equations.size());
            output.equations.push_back({mapping.nodes[schema.left], mapping.nodes[schema.right],
                schema.scope, std::move(guard), schema.origin});
            result.instances.push_back(std::move(mapping));
        }
    }
    // Reuse host-owned dimensions, source validation, scope and ground-guard
    // checks. In particular, no generated origin becomes a proved fact.
    (void)admit_problem_theory(result.request);
    return result;
}

bool verify_equation_expansion(const ProblemRequest& base,
    const std::vector<QuantifiedEquation>& schemas, const QuantifiedExpansion& expansion,
    std::uint64_t work_limit) {
    try {
        return instantiate_equations(base, schemas, work_limit) == expansion;
    } catch (const std::runtime_error&) {
        return false;
    }
}

} // namespace axiom
