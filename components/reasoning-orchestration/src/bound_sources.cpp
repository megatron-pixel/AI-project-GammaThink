#include "bound_sources.hpp"
#include "problem_wire.hpp"

#include <algorithm>
#include <functional>
#include <type_traits>

namespace axiom::knowledge {
namespace {
void require(bool value, ErrorCode code, const char* message) {
    if (!value) throw Error(code, message);
}
struct Scope {
    const SourceBindingScope* source;
    ProblemProposal theory;
    std::uint32_t views = 0, numbers = 0, nodes = 0;
};
}

BoundSourceRequest::BoundSourceRequest(ProblemRequest request, Object record)
    : request_(std::move(request)), record_(std::move(record)) {}

BoundSourceRequest admit_bound_sources(std::span<const SourceBindingScope> inputs,
    const ActiveSourceBindings& bindings, const BoundSourceGoal& goal, ProblemLimits limits) try {
    require(!inputs.empty() && inputs.size() <= 16, ErrorCode::Budget, "Bound source scope limit exceeded");
    std::map<ObjectId, Scope> scopes;
    std::map<SourceSymbol, SourceSymbol> parents;
    std::set<ObjectId> expected_modules;
    for (const auto& row : bindings.bindings()) {
        expected_modules.insert(row.left.module); expected_modules.insert(row.right.module);
    }
    ProblemRequest output;
    output.limits = limits;
    for (const auto& input : inputs) {
        const auto id = identify(input.module);
        require(expected_modules.contains(id), ErrorCode::Schema, "Source scope is outside accepted binding contract");
        auto theory = source_theory_proposal(input.source, input.module, input.context, input.host_context, limits);
        require(scopes.emplace(id, Scope{&input, std::move(theory)}).second,
                ErrorCode::Schema, "Duplicate combined source scope");
    }
    require(scopes.size() == expected_modules.size() && scopes.contains(goal.module),
            ErrorCode::Missing, "Combined source request lacks a required scope");
    output.context = scopes.begin()->second.source->host_context;
    output.context.views.clear();
    for (auto& [id, scope] : scopes) {
        auto registry = scope.source->host_context;
        registry.views.clear();
        require(registry == output.context, ErrorCode::Schema, "Combined sources require the same host unit registry");
        const auto add = [&](SourceSymbolKind kind, std::size_t count) {
            for (std::uint32_t i = 0; i < count; ++i) {
                SourceSymbol symbol{id, kind, i}; parents.emplace(symbol, symbol);
            }
        };
        add(SourceSymbolKind::Entity, scope.theory.entities.size());
        add(SourceSymbolKind::Predicate, scope.theory.logic.predicates.size());
        add(SourceSymbolKind::Quantity, scope.theory.quantities.size());
    }
    const auto root = [&](SourceSymbol symbol) {
        require(parents.contains(symbol), ErrorCode::Schema, "Accepted binding endpoint is outside its source scope");
        while (parents.at(symbol) != symbol) symbol = parents.at(symbol);
        return symbol;
    };
    for (const auto& row : bindings.bindings()) {
        const auto left = root(row.left), right = root(row.right);
        if (left != right) parents.at(std::max(left, right)) = std::min(left, right);
    }
    std::map<SourceSymbol, std::uint32_t> indices;
    auto& merged = output.proposal;
    std::map<SourceSymbol, std::uint32_t> representatives;
    for (const auto& [symbol, parent] : parents) {
        (void)parent;
        if (symbol.kind == SourceSymbolKind::Quantity) continue;
        const auto representative = root(symbol);
        auto found = representatives.find(representative);
        if (found == representatives.end()) {
            const auto index = static_cast<std::uint32_t>(symbol.kind == SourceSymbolKind::Entity
                ? merged.entities.size() : merged.logic.predicates.size());
            require(index < 64, ErrorCode::Budget, "Combined source symbol limit exceeded");
            if (symbol.kind == SourceSymbolKind::Entity) merged.entities.push_back("entity_" + std::to_string(index));
            else merged.logic.predicates.push_back({"predicate_" + std::to_string(index),
                scopes.at(symbol.module).theory.logic.predicates.at(symbol.index).arity});
            found = representatives.emplace(representative, index).first;
        }
        indices.emplace(symbol, found->second);
    }
    for (const auto& [symbol, parent] : parents) {
        (void)parent;
        if (symbol.kind != SourceSymbolKind::Quantity) continue;
        const auto representative = root(symbol);
        auto found = representatives.find(representative);
        if (found == representatives.end()) {
            require(merged.quantities.size() < 64, ErrorCode::Budget, "Combined source quantity limit exceeded");
            auto quantity = scopes.at(symbol.module).theory.quantities.at(symbol.index);
            quantity.entity = indices.at({symbol.module, SourceSymbolKind::Entity, quantity.entity});
            quantity.property = "quantity_" + std::to_string(merged.quantities.size());
            found = representatives.emplace(representative, static_cast<std::uint32_t>(merged.quantities.size())).first;
            merged.quantities.push_back(std::move(quantity));
        }
        indices.emplace(symbol, found->second);
    }
    if (!merged.logic.predicates.empty()) {
        merged.logic.name = "explicit_bound_sources"; merged.logic.version = "1";
        merged.logic.constants = merged.entities;
    }
    std::size_t ordinal = 0;
    for (auto& [id, scope] : scopes) {
        auto& source = scope.theory;
        const auto prefix = "source_" + std::to_string(ordinal++) + ".";
        scope.views = static_cast<std::uint32_t>(output.context.views.size());
        scope.numbers = static_cast<std::uint32_t>(merged.numbers.size());
        scope.nodes = static_cast<std::uint32_t>(merged.nodes.size());
        for (std::size_t i = 0; i < scope.source->host_context.views.size(); ++i) {
            auto view = scope.source->host_context.views[i];
            view.id = prefix + "view_" + std::to_string(i);
            output.context.views.push_back(std::move(view));
        }
        const auto span = [&](EvidenceSpan& support) { support.view += scope.views; };
        const auto origin = [&](ProblemOrigin& value) {
            value.label = prefix + value.label;
            for (auto& support : value.supports) span(support);
        };
        const auto atom = [&](Atom value, const std::vector<std::uint32_t>& substitution) {
            value.predicate = indices.at({id, SourceSymbolKind::Predicate, value.predicate});
            for (auto& term : value.terms) {
                const auto constant = term.kind == TermKind::Constant ? term.index : substitution.at(term.index);
                term = {TermKind::Constant, indices.at({id, SourceSymbolKind::Entity, constant})};
            }
            return value;
        };
        for (auto number : source.numbers) {
            span(number.numeral);
            if (number.unit_span) span(*number.unit_span);
            if (number.qualifier) span(*number.qualifier);
            merged.numbers.push_back(std::move(number));
        }
        for (auto binding : source.bindings) {
            binding.quantity = indices.at({id, SourceSymbolKind::Quantity, binding.quantity});
            binding.number += scope.numbers; origin(binding.origin);
            merged.bindings.push_back(std::move(binding));
        }
        for (const auto& node : source.nodes) merged.nodes.push_back(std::visit([&](auto value) -> ProblemNode {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, ProblemQuantityNode>)
                value.quantity = indices.at({id, SourceSymbolKind::Quantity, value.quantity});
            else if constexpr (std::is_same_v<T, ProblemNumberNode>) value.number += scope.numbers;
            else if constexpr (std::is_same_v<T, ProblemNegateNode>) value.operand += scope.nodes;
            else { value.left += scope.nodes; value.right += scope.nodes; }
            return value;
        }, node));
        for (auto equation : source.equations) {
            equation.left += scope.nodes; equation.right += scope.nodes; origin(equation.origin);
            if (equation.guard) equation.guard = atom(*equation.guard, {});
            merged.equations.push_back(std::move(equation));
        }
        for (std::size_t i = 0; i < source.logic.rules.size(); ++i) {
            const auto& rule = source.logic.rules[i];
            std::size_t instances = 1;
            for (std::uint32_t variable = 0; variable < rule.variables; ++variable) {
                require(!source.entities.empty() && instances <= 128 / source.entities.size(),
                        ErrorCode::Budget, "Combined source finite grounding limit exceeded");
                instances *= source.entities.size();
            }
            require(instances <= 128 - merged.logic.rules.size(), ErrorCode::Budget,
                    "Combined source rule limit exceeded");
            for (std::size_t instance = 0; instance < instances; ++instance) {
                std::vector<std::uint32_t> substitution(rule.variables);
                auto value = instance;
                for (std::size_t variable = substitution.size(); variable > 0; --variable) {
                    substitution[variable - 1] = static_cast<std::uint32_t>(value % source.entities.size());
                    value /= source.entities.size();
                }
                RelationalRule grounded{0, atom(rule.head, substitution), {},
                    prefix + "rule_" + std::to_string(i) + "." + std::to_string(instance)};
                for (const auto& premise : rule.premises) grounded.premises.push_back(atom(premise, substitution));
                auto evidence = source.logic_origins.at(i);
                origin(evidence); evidence.label = grounded.provenance;
                merged.logic.rules.push_back(std::move(grounded)); merged.logic_origins.push_back(std::move(evidence));
            }
        }
    }
    require(merged.numbers.size() <= 128 && merged.bindings.size() <= 128 &&
            merged.nodes.size() <= 256 && merged.equations.size() <= 64,
            ErrorCode::Budget, "Combined source native theory bounds exceeded");
    const auto& selected = *scopes.at(goal.module).source;
    const auto checked_goal = source_query_request(selected.source, selected.module, selected.context,
                                                   selected.host_context, goal.goal, limits).proposal.goal;
    merged.goal = std::visit([&](auto value) -> ProblemGoal {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, ProblemQuantityGoal>)
            value.quantity = indices.at({goal.module, SourceSymbolKind::Quantity, value.quantity});
        else {
            value.atom.predicate = indices.at({goal.module, SourceSymbolKind::Predicate, value.atom.predicate});
            for (auto& term : value.atom.terms)
                term.index = indices.at({goal.module, SourceSymbolKind::Entity, term.index});
        }
        return value;
    }, checked_goal);
    (void)admit_problem(output);
    Object record;
    record.kind = ObjectKind::Goal; record.label = "Conditional explicitly bound source query";
    record.format = native_request_format; record.payload = encode_problem_request(output);
    record.links.push_back({Relationship::Requires, bindings.assumption()});
    std::set<ObjectId> prerequisites;
    for (const auto& [id, scope] : scopes) {
        prerequisites.insert(id); prerequisites.insert(identify(scope.source->source));
        prerequisites.insert(identify(scope.source->context));
        auto candidate = scope.source->module; candidate.links.pop_back();
        const auto mode = scope.source->context.format == "axiom.problem-context.offset-v1.json"
            ? ProblemSelectorMode::OffsetV1 : ProblemSelectorMode::QuoteV1;
        const auto compiled = materialize_source(scope.source->source, candidate, scope.source->host_context, {}, limits, mode);
        for (const auto& member : compiled.members) prerequisites.insert(identify(member));
        record.sources.insert(record.sources.end(), scope.source->source.sources.begin(), scope.source->source.sources.end());
    }
    for (const auto& id : prerequisites) record.links.push_back({Relationship::DependsOn, id});
    (void)encode_object(record);
    return BoundSourceRequest(std::move(output), std::move(record));
} catch (const Error&) {
    throw;
} catch (const std::runtime_error& error) {
    throw Error(ErrorCode::Schema, error.what());
}

BoundSourceRun run_bound_sources(UnifiedKnowledge& knowledge, Journal& journal,
    const ObjectId& binding, const std::map<ObjectId, ProblemContext>& host_contexts,
    const std::set<ObjectId>& accepted, const BoundSourceGoal& goal, KnowledgeRunLimits limits) {
    BoundSourceRun run;
    std::map<ObjectId, Object> records;
    std::set<ObjectId> missing, blocked;
    std::size_t bytes = 0;
    try {
        const auto active = activate_source_bindings(knowledge, journal, binding, host_contexts, accepted, limits.retrieval);
        const auto fetch = [&](const ObjectId& id) {
            const auto closure = knowledge.resolve(id, limits.retrieval);
            missing.insert(closure.unavailable.begin(), closure.unavailable.end());
            for (const auto& item : closure.objects) {
                const auto invalid = [](const std::optional<Assessment>& value) {
                    return value && (value->state == KnowledgeState::Invalidated || value->logical == LogicalValidity::Invalid);
                };
                if (invalid(item.record.assessment) || invalid(journal.assessment(item.id))) blocked.insert(item.id);
                if (records.contains(item.id)) continue;
                require(records.size() + missing.size() < limits.retrieval.objects, ErrorCode::Budget,
                        "Combined source object budget exceeded");
                const auto size = encode_object(item.record.object).size();
                require(size <= limits.retrieval.encoded_bytes - bytes, ErrorCode::Budget,
                        "Combined source byte budget exceeded");
                bytes += size; records.emplace(item.id, item.record.object);
            }
        };
        std::optional<BoundSourceRequest> lowered;
        journal.read_snapshot([&] {
            fetch(binding);
            if (!missing.empty() || !blocked.empty()) return;
            std::set<ObjectId> modules;
            for (const auto& row : active.bindings()) { modules.insert(row.left.module); modules.insert(row.right.module); }
            std::vector<SourceBindingScope> scopes;
            for (const auto& id : modules) {
                const auto& module = records.at(id);
                require(module.links.size() == 2 && records.contains(module.links[0].target) &&
                    records.contains(module.links[1].target) && host_contexts.contains(id),
                    ErrorCode::Integrity, "Combined source scope dependencies differ");
                scopes.push_back({records.at(module.links[0].target), module, records.at(module.links[1].target),
                                  host_contexts.at(id)});
            }
            run.scopes = scopes.size();
            lowered.emplace(admit_bound_sources(scopes, active, goal));
            for (const auto& link : lowered->record().links) fetch(link.target);
        });
        for (const auto& observation : active.observations()) {
            const auto current = journal.assessment(observation.object);
            if ((current ? current->version : 0) != observation.version) blocked.insert(observation.object);
        }
        run.outcome.object_lookups = records.size() + missing.size();
        if (!missing.empty() || !blocked.empty()) {
            run.outcome.status = blocked.empty() ? KnowledgeRunStatus::DependenciesUnavailable : KnowledgeRunStatus::RevalidationRequired;
            run.outcome.unavailable.assign(missing.begin(), missing.end());
            run.outcome.revalidation_required.assign(blocked.begin(), blocked.end());
            run.outcome.reason = blocked.empty() ? "Combined source dependencies are unavailable"
                                                : "Combined source dependencies changed or require revalidation";
            return run;
        }
        require(lowered.has_value(), ErrorCode::Integrity, "Combined source admission lost its request");
        auto executable = lowered->record();
        std::set<ObjectId> linked;
        for (const auto& link : executable.links) linked.insert(link.target);
        for (const auto& [id, object] : records) {
            if (linked.insert(id).second) executable.links.push_back({Relationship::DependsOn, id});
            if (object.format == "gammathink.source-member.v1") ++run.members;
        }
        require(records.size() < limits.retrieval.objects &&
                encode_object(executable).size() <= limits.retrieval.encoded_bytes - bytes,
                ErrorCode::Budget, "Combined source request exceeds aggregate record budget");
        std::size_t imported = 0;
        for (const auto& [id, object] : records)
            if (!journal.contains(id)) { journal.put(object); ++imported; }
        run.request = journal.put(executable);
        JournalArchive local(journal);
        UnifiedKnowledge current({&local});
        auto outcome = run_knowledge_goal(current, journal, *run.request, limits, accepted);
        outcome.imported_objects += imported; outcome.object_lookups += run.outcome.object_lookups;
        run.outcome = std::move(outcome);
        return run;
    } catch (const SourceBindingRevalidation& error) {
        run.outcome.status = KnowledgeRunStatus::RevalidationRequired;
        run.outcome.revalidation_required.push_back(error.object);
        run.outcome.reason = error.what();
        return run;
    } catch (const Error& error) {
        if (error.code == ErrorCode::Budget) run.outcome.status = KnowledgeRunStatus::ResourceLimited;
        else if (error.code == ErrorCode::Unavailable || error.code == ErrorCode::Missing)
            run.outcome.status = KnowledgeRunStatus::DependenciesUnavailable;
        else throw;
        run.outcome.reason = error.what();
        return run;
    }
}

} // namespace axiom::knowledge
