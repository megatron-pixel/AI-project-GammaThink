#include "goal_planner.hpp"

#include <algorithm>

namespace axiom::knowledge {
namespace {

void identifier(std::string_view value) {
    validate_text(value, 128);
    if (value.empty()) throw Error(ErrorCode::Schema, "Missing goal identifier");
}
void conditions(const GoalConditions& values) {
    if (values.size() > maximum_items) throw Error(ErrorCode::Budget, "Too many goal conditions");
    for (const auto& [key, value] : values) {
        identifier(key);
        if (const auto* text = std::get_if<std::string>(&value)) validate_text(*text, 4096);
    }
}
bool matches(const GoalConditions& required, const GoalConditions& actual) {
    return std::all_of(required.begin(), required.end(), [&](const auto& entry) {
        const auto found = actual.find(entry.first);
        return found != actual.end() && found->second == entry.second;
    });
}
bool matches(std::span<const GoalPredicate> required, const GoalConditions& actual) {
    return std::all_of(required.begin(), required.end(), [&](const auto& predicate) {
        const auto found = actual.find(predicate.field);
        if (found == actual.end() || found->second.index() != predicate.value.index()) return false;
        switch (predicate.comparison) {
        case GoalComparison::Equal: return found->second == predicate.value;
        case GoalComparison::NotEqual: return found->second != predicate.value;
        case GoalComparison::Less: return std::get<std::int64_t>(found->second) < std::get<std::int64_t>(predicate.value);
        case GoalComparison::LessEqual: return std::get<std::int64_t>(found->second) <= std::get<std::int64_t>(predicate.value);
        case GoalComparison::Greater: return std::get<std::int64_t>(found->second) > std::get<std::int64_t>(predicate.value);
        case GoalComparison::GreaterEqual: return std::get<std::int64_t>(found->second) >= std::get<std::int64_t>(predicate.value);
        }
        throw Error(ErrorCode::Schema, "Unknown goal comparison");
    });
}

} // namespace

void validate_goal_predicates(std::span<const GoalPredicate> predicates) {
    if (predicates.size() > maximum_items) throw Error(ErrorCode::Budget, "Too many typed goal predicates");
    for (const auto& predicate : predicates) {
        identifier(predicate.field);
        if (predicate.comparison < GoalComparison::Equal || predicate.comparison > GoalComparison::GreaterEqual)
            throw Error(ErrorCode::Schema, "Unknown goal comparison");
        if (const auto* value = std::get_if<std::string>(&predicate.value)) validate_text(*value, 4096);
        if (predicate.comparison != GoalComparison::Equal && predicate.comparison != GoalComparison::NotEqual
            && !std::holds_alternative<std::int64_t>(predicate.value))
            throw Error(ErrorCode::Schema, "Ordered goal comparisons require integers");
    }
}

GoalGraph decompose_goals(std::string_view root, const std::vector<GoalIR>& definitions,
                         GoalPlanLimits limits) {
    identifier(root);
    if (!limits.nodes || limits.nodes > maximum_items || !limits.edges ||
        limits.edges > maximum_items * 16 || definitions.size() > maximum_items) {
        throw Error(ErrorCode::Budget, "Invalid goal graph limits");
    }
    std::map<std::string, const GoalIR*> registry;
    std::size_t declared_edges = 0;
    for (const auto& definition : definitions) {
        identifier(definition.id);
        conditions(definition.constraints);
        conditions(definition.success_conditions);
        validate_goal_predicates(definition.requirements);
        validate_goal_predicates(definition.success_requirements);
        if (definition.methods.size() > maximum_items || definition.assumptions.size() > maximum_items ||
            definition.sources.size() > maximum_items) {
            throw Error(ErrorCode::Budget, "Oversized goal definition");
        }
        if (!registry.emplace(definition.id, &definition).second) {
            throw Error(ErrorCode::Schema, "Duplicate goal definition");
        }
        for (const auto& method : definition.methods) {
            if (method.size() > limits.edges - declared_edges) {
                throw Error(ErrorCode::Budget, "Goal declaration edge budget exceeded");
            }
            declared_edges += method.size();
            for (const auto& name : method) identifier(name);
        }
    }
    GoalGraph graph;
    std::map<std::string, std::size_t> indices;
    const auto add = [&](const std::string& name) {
        const auto known = indices.find(name);
        if (known != indices.end()) return known->second;
        if (graph.nodes.size() == limits.nodes) {
            throw Error(ErrorCode::Budget, "Goal node budget exceeded");
        }
        const auto index = graph.nodes.size();
        indices.emplace(name, index);
        const auto definition = registry.find(name);
        graph.nodes.push_back({name, definition == registry.end()
            ? std::nullopt : std::optional<GoalIR>(*definition->second), {}});
        return index;
    };
    graph.root = add(std::string(root));
    for (std::size_t current = 0; current < graph.nodes.size(); ++current) {
        if (!graph.nodes[current].definition) continue;
        // Adding shared nodes may reallocate graph.nodes. Do not borrow a
        // node or its methods across add().
        const auto methods = graph.nodes[current].definition->methods;
        std::vector<std::vector<std::size_t>> expanded;
        for (const auto& method : methods) {
            std::vector<std::size_t> required;
            for (const auto& name : method) required.push_back(add(name));
            expanded.push_back(std::move(required));
        }
        graph.nodes[current].methods = std::move(expanded);
    }
    // Iterative DFS reports cycles without recursion or declaring goals false.
    std::vector<std::uint8_t> colors(graph.nodes.size(), 0);
    struct Visit { std::size_t node, method = 0, edge = 0; };
    for (std::size_t start = 0; start < graph.nodes.size(); ++start) {
        if (colors[start]) continue;
        std::vector<Visit> stack{{start}};
        colors[start] = 1;
        while (!stack.empty()) {
            auto& visit = stack.back();
            const auto& methods = graph.nodes[visit.node].methods;
            if (visit.method == methods.size()) {
                colors[visit.node] = 2;
                stack.pop_back();
            } else if (visit.edge == methods[visit.method].size()) {
                ++visit.method;
                visit.edge = 0;
            } else {
                const auto child = methods[visit.method][visit.edge++];
                if (colors[child] == 1) graph.cycle_edges.emplace_back(visit.node, child);
                if (colors[child] == 0) {
                    colors[child] = 1;
                    stack.push_back({child});
                }
            }
        }
    }
    return graph;
}

GoalEvaluation evaluate_goals(const GoalGraph& graph,
    const std::map<std::string, GoalOutcome>& outcomes, const GoalConditions& environment,
    const std::set<ObjectId>& accepted_assumptions, std::span<const GoalPredicate> active_constraints) {
    validate_goal_predicates(active_constraints);
    if (graph.nodes.empty() || graph.nodes.size() > maximum_items || graph.root >= graph.nodes.size()) {
        throw Error(ErrorCode::Schema, "Invalid goal graph");
    }
    conditions(environment);
    std::set<std::string> names;
    std::size_t edges = 0;
    GoalEvaluation result;
    result.states.assign(graph.nodes.size(), GoalState::Pending);
    for (std::size_t i = 0; i < graph.nodes.size(); ++i) {
        const auto& node = graph.nodes[i];
        identifier(node.id);
        if (!names.insert(node.id).second || (node.definition && node.definition->id != node.id)) {
            throw Error(ErrorCode::Schema, "Inconsistent goal graph identity");
        }
        if ((!node.definition && !node.methods.empty()) ||
            (node.definition && node.methods.size() != node.definition->methods.size())) {
            throw Error(ErrorCode::Schema, "Goal methods differ from their source definition");
        }
        if (node.definition) {
            conditions(node.definition->constraints);
            conditions(node.definition->success_conditions);
            validate_goal_predicates(node.definition->requirements);
            validate_goal_predicates(node.definition->success_requirements);
            if (node.definition->assumptions.size() > maximum_items) {
                throw Error(ErrorCode::Budget, "Too many goal assumptions");
            }
            for (std::size_t method = 0; method < node.methods.size(); ++method) {
                const auto& declared = node.definition->methods[method];
                if (node.methods[method].size() != declared.size()) {
                    throw Error(ErrorCode::Schema, "Goal method lost prerequisites");
                }
                for (std::size_t edge = 0; edge < declared.size(); ++edge) {
                    const auto child = node.methods[method][edge];
                    if (child >= graph.nodes.size() || graph.nodes[child].id != declared[edge]) {
                        throw Error(ErrorCode::Schema, "Goal dependency differs from declared scope");
                    }
                }
            }
        }
        for (const auto& method : node.methods) {
            if (method.size() > maximum_items * 16 - edges) {
                throw Error(ErrorCode::Budget, "Goal evaluation edge budget exceeded");
            }
            edges += method.size();
            for (const auto child : method) {
                if (child >= graph.nodes.size()) throw Error(ErrorCode::Schema, "Invalid goal dependency");
            }
        }
        if (!node.definition) {
            result.states[i] = GoalState::MissingRequirement;
        } else if (!matches(node.definition->constraints, environment)
            || !matches(node.definition->requirements, environment) || !matches(active_constraints, environment)) {
            result.states[i] = GoalState::ConstraintBlocked;
        } else if (!std::all_of(node.definition->assumptions.begin(), node.definition->assumptions.end(),
                               [&](const auto& id) { return accepted_assumptions.contains(id); })) {
            result.states[i] = GoalState::AssumptionBlocked;
        }
    }
    bool changed = true;
    while (changed) {
        changed = false;
        for (std::size_t i = 0; i < graph.nodes.size(); ++i) {
            if (result.states[i] != GoalState::Pending && result.states[i] != GoalState::Ready) continue;
            const auto& node = graph.nodes[i];
            const bool ready = node.methods.empty() ||
                std::any_of(node.methods.begin(), node.methods.end(), [&](const auto& method) {
                    return std::all_of(method.begin(), method.end(),
                        [&](auto child) { return result.states[child] == GoalState::Satisfied; });
                });
            if (!ready) continue;
            auto state = GoalState::Ready;
            const auto found = outcomes.find(node.id);
            if (found != outcomes.end()) {
                const auto& outcome = found->second;
                conditions(outcome.observed);
                if (!outcome.verified) {
                    state = GoalState::UnresolvedOutcome;
                } else if (!outcome.succeeded) {
                    state = GoalState::Failed;
                } else if (!matches(node.definition->success_conditions, outcome.observed)
                    || !matches(node.definition->success_requirements, outcome.observed)) {
                    const bool missing = std::any_of(node.definition->success_conditions.begin(),
                        node.definition->success_conditions.end(),
                        [&](const auto& condition) { return !outcome.observed.contains(condition.first); });
                    const bool missing_typed = std::any_of(node.definition->success_requirements.begin(),
                        node.definition->success_requirements.end(),
                        [&](const auto& condition) { return !outcome.observed.contains(condition.field); });
                    state = missing || missing_typed ? GoalState::UnresolvedOutcome : GoalState::Failed;
                } else {
                    state = GoalState::Satisfied;
                }
            }
            if (result.states[i] != state) {
                result.states[i] = state;
                changed = true;
            }
        }
    }
    result.root_satisfied = result.states[graph.root] == GoalState::Satisfied;
    return result;
}

} // namespace axiom::knowledge
