#include "problem.hpp"

#include <algorithm>
#include <functional>
#include <set>
#include <stdexcept>

namespace axiom {
namespace {
void require(bool value) {
    if (!value) throw std::runtime_error("Invalid problem certificate");
}
struct Form {
    Rational coefficient = 0;
    Rational constant = 0;
};
struct Inspection {
    std::set<std::uint32_t> quantities, numbers, nodes;
};
struct EquationState {
    ProblemIssue issue = ProblemIssue::None;
    std::set<std::uint32_t> unknown;
    std::vector<std::uint32_t> dependencies;
    std::set<std::uint32_t> numbers;
    Form residual;
};
}

bool verify_problem_result(const ProblemRequest& request, const ProblemResult& result) {
    try {
        require(request == result.request);
        const auto admitted = admit_problem(request);
        const auto& proposal = request.proposal;
        require(result.work <= request.limits.work && result.steps.size() <= request.limits.steps);
        require(result.status == ProblemStatus::Checked || result.status == ProblemStatus::Unresolved
            || result.status == ProblemStatus::Conflict || result.status == ProblemStatus::Unsupported
            || result.status == ProblemStatus::Limited);
        std::uint64_t visits = admitted.admission_work;
        const auto visit = [&](std::uint64_t count = 1) {
            require(count <= std::max<std::uint64_t>(1024, request.limits.work * 4)
                && visits <= std::max<std::uint64_t>(1024, request.limits.work * 4) - count);
            visits += count;
        };
        const auto bounded = [](Rational value) {
            return checked_problem_rational(std::move(value));
        };
        const auto canonical = [&](const Rational& value) {
            const auto normalized = bounded(value);
            require(normalized.get_num() == value.get_num() && normalized.get_den() == value.get_den());
        };
        std::vector<Atom> wanted_guards;
        const auto add_guard = [&](const Atom& goal) {
            if (std::find(wanted_guards.begin(), wanted_guards.end(), goal) == wanted_guards.end()) {
                wanted_guards.push_back(goal);
            }
        };
        for (const auto& equation : proposal.equations) if (equation.guard) add_guard(*equation.guard);
        if (const auto goal = std::get_if<ProblemAtomGoal>(&proposal.goal)) add_guard(goal->atom);
        require(result.guards.size() <= wanted_guards.size());
        if (result.status != ProblemStatus::Limited) require(result.guards.size() == wanted_guards.size());
        std::uint64_t minimum_work = admitted.admission_work;
        bool contradictory_guard = false;
        for (std::size_t i = 0; i < result.guards.size(); ++i) {
            const auto& guard = result.guards[i];
            require(guard.goal == wanted_guards[i]);
            const auto& certificate = guard.certificate;
            require(certificate.request.module == proposal.logic && certificate.request.goal == guard.goal
                && !certificate.request.gpu && certificate.request.limits.depth == 128
                && certificate.request.limits.events > 0 && certificate.request.limits.events <= 4096
                && certificate.request.limits.events <= request.limits.work);
            const auto power = [&](std::uint32_t exponent) {
                std::uint64_t value = 1;
                for (std::uint32_t j = 0; j < exponent; ++j) value *= proposal.entities.size();
                return value;
            };
            std::uint64_t cost = certificate.proof.history.size();
            for (const auto& predicate : proposal.logic.predicates) cost += power(predicate.arity);
            for (const auto& rule : proposal.logic.rules) {
                std::uint64_t terms = 2 + rule.variables + rule.head.terms.size();
                for (const auto& premise : rule.premises) terms += 1 + premise.terms.size();
                cost += power(rule.variables) * terms;
            }
            minimum_work += cost;
            visit(cost);
            require(verify_relational_result(certificate.request, certificate));
            if (result.status != ProblemStatus::Limited) require(!certificate.proof.limited);
            contradictory_guard |= certificate.proof.status == Status::Contradiction;
        }
        require(minimum_work <= request.limits.work || result.status == ProblemStatus::Limited);
        std::set<std::pair<ProblemPremiseKind,std::uint32_t>> used;
        const auto use = [&](ProblemPremiseKind kind, std::uint32_t index) { used.emplace(kind,index); };
        if (!result.guards.empty()) {
            for (std::uint32_t i = 0; i < proposal.logic.rules.size(); ++i) use(ProblemPremiseKind::Rule,i);
        }
        std::vector<std::uint32_t> values(proposal.quantities.size(), no_step);
        bool differing_values = false;
        const auto inspect = [&](std::uint32_t left, std::uint32_t right) {
            Inspection seen;
            std::vector<std::uint32_t> pending{left,right};
            while (!pending.empty()) {
                const auto index = pending.back(); pending.pop_back();
                if (!seen.nodes.insert(index).second) continue;
                visit();
                const auto& node = proposal.nodes[index];
                if (const auto quantity = std::get_if<ProblemQuantityNode>(&node)) {
                    seen.quantities.insert(quantity->quantity);
                } else if (const auto number = std::get_if<ProblemNumberNode>(&node)) {
                    seen.numbers.insert(number->number);
                } else if (const auto negate = std::get_if<ProblemNegateNode>(&node)) {
                    pending.push_back(negate->operand);
                } else {
                    const auto& binary = std::get<ProblemBinaryNode>(node);
                    pending.push_back(binary.left); pending.push_back(binary.right);
                }
            }
            return seen;
        };
        const auto guard_index = [&](std::uint32_t equation) -> std::optional<std::uint32_t> {
            const auto& goal = proposal.equations[equation].guard;
            if (!goal) return std::nullopt;
            const auto found = std::find(wanted_guards.begin(), wanted_guards.end(), *goal);
            require(found != wanted_guards.end());
            const auto index = static_cast<std::uint32_t>(found - wanted_guards.begin());
            require(index < result.guards.size());
            return index;
        };
        const auto analyse = [&](std::uint32_t equation_index,
                                 std::optional<std::uint32_t> target) {
            EquationState state;
            const auto& equation = proposal.equations[equation_index];
            if (!equation.scope.resolved) { state.issue = ProblemIssue::MissingPremise; return state; }
            const auto guard = guard_index(equation_index);
            if (guard && result.guards[*guard].certificate.proof.status != Status::Proved) {
                state.issue = ProblemIssue::MissingPremise;
                return state;
            }
            const auto seen = inspect(equation.left,equation.right);
            state.numbers = seen.numbers;
            std::set<std::uint32_t> dependencies;
            for (const auto quantity : seen.quantities) {
                if (target == quantity || values[quantity] == no_step) state.unknown.insert(quantity);
                else dependencies.insert(values[quantity]);
            }
            state.dependencies.assign(dependencies.begin(),dependencies.end());
            if (target && !seen.quantities.contains(*target)) {
                state.issue = ProblemIssue::MissingPremise;
                return state;
            }
            for (const auto number : seen.numbers) {
                if (proposal.numbers[number].precision != QuantityPrecision::Exact) {
                    state.issue = ProblemIssue::NonExact;
                    return state;
                }
            }
            if (state.unknown.size() > 1) { state.issue = ProblemIssue::MissingPremise; return state; }
            const auto unknown = state.unknown.empty()
                ? std::optional<std::uint32_t>{} : std::optional<std::uint32_t>{*state.unknown.begin()};
            std::vector<std::optional<Form>> cache(proposal.nodes.size());
            const std::function<Form(std::uint32_t)> evaluate = [&](std::uint32_t index) -> Form {
                if (cache[index]) return *cache[index];
                visit();
                const auto& node = proposal.nodes[index];
                Form answer;
                if (const auto quantity = std::get_if<ProblemQuantityNode>(&node)) {
                    if (unknown == quantity->quantity) answer.coefficient = 1;
                    else {
                        require(values[quantity->quantity] != no_step);
                        answer.constant = result.steps[values[quantity->quantity]].canonical_value;
                    }
                } else if (const auto number = std::get_if<ProblemNumberNode>(&node)) {
                    answer.constant = admitted.canonical_numbers[number->number];
                } else if (const auto negate = std::get_if<ProblemNegateNode>(&node)) {
                    const auto operand = evaluate(negate->operand);
                    answer = {bounded(-operand.coefficient),bounded(-operand.constant)};
                } else {
                    const auto& binary = std::get<ProblemBinaryNode>(node);
                    const auto left = evaluate(binary.left), right = evaluate(binary.right);
                    switch (binary.operation) {
                    case Code::Add:
                        answer = {bounded(left.coefficient + right.coefficient),
                                  bounded(left.constant + right.constant)};
                        break;
                    case Code::Subtract:
                        answer = {bounded(left.coefficient - right.coefficient),
                                  bounded(left.constant - right.constant)};
                        break;
                    case Code::Multiply:
                        if (left.coefficient != 0 && right.coefficient != 0) throw ProblemIssue::NonAffine;
                        answer = {bounded(left.coefficient * right.constant + right.coefficient * left.constant),
                                  bounded(left.constant * right.constant)};
                        break;
                    case Code::Divide:
                        if (right.coefficient != 0) throw ProblemIssue::NonAffine;
                        if (right.constant == 0) throw ProblemIssue::Undefined;
                        answer = {bounded(left.coefficient / right.constant),bounded(left.constant / right.constant)};
                        break;
                    default: throw std::runtime_error("Invalid admitted expression");
                    }
                }
                cache[index] = answer;
                return answer;
            };
            try {
                const auto left = evaluate(equation.left), right = evaluate(equation.right);
                state.residual = {bounded(left.coefficient - right.coefficient),
                                  bounded(left.constant - right.constant)};
            } catch (ProblemIssue issue) {
                state.issue = issue;
            }
            return state;
        };
        for (std::uint32_t i = 0; i < result.steps.size(); ++i) {
            const auto& step = result.steps[i];
            require(step.quantity < proposal.quantities.size() && proposal.quantities[step.quantity].scope.resolved);
            canonical(step.canonical_value);
            if (const auto binding = std::get_if<ProblemBindingStep>(&step.justification)) {
                require(binding->binding < proposal.bindings.size());
                const auto& input = proposal.bindings[binding->binding];
                require(input.quantity == step.quantity
                    && proposal.numbers[input.number].precision == QuantityPrecision::Exact);
                canonical(binding->lexical_value); canonical(binding->unit_scale);
                require(binding->lexical_value == admitted.lexical_numbers[input.number]
                    && binding->unit_scale == request.context.units[proposal.numbers[input.number].unit].scale
                    && step.canonical_value == admitted.canonical_numbers[input.number]);
                use(ProblemPremiseKind::Binding,binding->binding);
                use(ProblemPremiseKind::Number,input.number);
                ++minimum_work;
            } else {
                const auto& equation = std::get<ProblemEquationStep>(step.justification);
                require(equation.equation < proposal.equations.size());
                const auto state = analyse(equation.equation,step.quantity);
                require(state.issue == ProblemIssue::None && state.unknown.size() == 1
                    && *state.unknown.begin() == step.quantity && state.residual.coefficient != 0
                    && equation.dependencies == state.dependencies
                    && equation.guard == guard_index(equation.equation));
                require(step.canonical_value == bounded(-state.residual.constant / state.residual.coefficient));
                require(bounded(state.residual.coefficient * step.canonical_value + state.residual.constant) == 0);
                for (const auto dependency : equation.dependencies) require(dependency < i);
                use(ProblemPremiseKind::Equation,equation.equation);
                for (const auto number : state.numbers) use(ProblemPremiseKind::Number,number);
                minimum_work += inspect(proposal.equations[equation.equation].left,
                                        proposal.equations[equation.equation].right).nodes.size();
            }
            const auto prior = values[step.quantity];
            if (prior == no_step) values[step.quantity] = i;
            else differing_values |= result.steps[prior].canonical_value != step.canonical_value;
        }
        if (admitted.admission_work <= request.limits.work) require(result.work >= minimum_work);
        else require(result.status == ProblemStatus::Limited && result.guards.empty() && result.steps.empty());
        if (result.status == ProblemStatus::Conflict) {
            require(result.conflict.has_value() && result.issue == ProblemIssue::None);
            if (const auto pair = std::get_if<ProblemValueConflict>(&*result.conflict)) {
                require(pair->first < pair->second && pair->second < result.steps.size());
                const auto& first = result.steps[pair->first];
                const auto& second = result.steps[pair->second];
                require(first.quantity == second.quantity && first.canonical_value != second.canonical_value);
            } else if (const auto equation = std::get_if<ProblemEquationConflict>(&*result.conflict)) {
                require(equation->equation < proposal.equations.size());
                const auto state = analyse(equation->equation,std::nullopt);
                require(state.issue == ProblemIssue::None && state.residual.coefficient == 0
                    && state.residual.constant != 0 && equation->dependencies == state.dependencies
                    && equation->guard == guard_index(equation->equation));
                use(ProblemPremiseKind::Equation,equation->equation);
                for (const auto number : state.numbers) use(ProblemPremiseKind::Number,number);
            } else {
                const auto guard = std::get<ProblemGuardConflict>(*result.conflict).guard;
                require(guard < result.guards.size()
                    && result.guards[guard].certificate.proof.status == Status::Contradiction);
            }
        } else {
            require(!result.conflict && !differing_values);
            if (result.status != ProblemStatus::Limited) {
                require(!contradictory_guard);
                for (const auto& binding : proposal.bindings) {
                    if (!proposal.quantities[binding.quantity].scope.resolved
                        || proposal.numbers[binding.number].precision != QuantityPrecision::Exact) continue;
                    require(values[binding.quantity] != no_step
                        && result.steps[values[binding.quantity]].canonical_value
                           == admitted.canonical_numbers[binding.number]);
                }
                std::vector<ProblemPendingEquation> pending;
                for (std::uint32_t i = 0; i < proposal.equations.size(); ++i) {
                    const auto state = analyse(i,std::nullopt);
                    if (state.issue != ProblemIssue::None) pending.push_back({i,state.issue});
                    else if (!state.unknown.empty()) {
                        require(state.residual.coefficient == 0 && state.residual.constant == 0);
                        pending.push_back({i,ProblemIssue::MissingPremise});
                    } else require(state.residual.constant == 0);
                }
                require(pending == result.pending_equations);
            }
        }
        if (const auto goal = std::get_if<ProblemQuantityGoal>(&proposal.goal)) {
            require(!result.logical_goal_guard);
            if (result.status == ProblemStatus::Checked) {
                require(result.issue == ProblemIssue::None && result.goal_step && result.answer
                    && result.answer_unit == goal->unit && *result.goal_step < result.steps.size()
                    && values[goal->quantity] == *result.goal_step
                    && result.steps[*result.goal_step].quantity == goal->quantity);
                canonical(*result.answer);
                require(*result.answer == bounded(result.steps[*result.goal_step].canonical_value
                    / request.context.units[goal->unit].scale));
            } else {
                require(!result.goal_step && !result.answer && result.answer_unit == no_step);
                if (result.status != ProblemStatus::Conflict && result.status != ProblemStatus::Limited) {
                    require(values[goal->quantity] == no_step);
                }
            }
        } else {
            require(!result.answer && !result.goal_step && result.answer_unit == no_step);
            const auto& atom_goal = std::get<ProblemAtomGoal>(proposal.goal).atom;
            if (result.status != ProblemStatus::Limited) require(result.logical_goal_guard.has_value());
            if (result.logical_goal_guard) {
                require(*result.logical_goal_guard < result.guards.size()
                    && result.guards[*result.logical_goal_guard].goal == atom_goal);
                const auto status = result.guards[*result.logical_goal_guard].certificate.proof.status;
                if (result.status == ProblemStatus::Checked) {
                    require(result.issue == ProblemIssue::None
                        && (status == Status::Proved || status == Status::Disproved));
                } else if (result.status != ProblemStatus::Conflict && result.status != ProblemStatus::Limited) {
                    require(status == Status::Unknown);
                }
            }
        }
        if (result.status == ProblemStatus::Limited) {
            require(result.issue == ProblemIssue::WorkLimit || result.issue == ProblemIssue::WireLimit);
            for (const auto& pending : result.pending_equations) require(pending.equation < proposal.equations.size());
        } else if (result.status == ProblemStatus::Unresolved || result.status == ProblemStatus::Unsupported) {
            auto issue = ProblemIssue::MissingPremise;
            for (const auto& pending : result.pending_equations) {
                if (pending.issue != ProblemIssue::MissingPremise) { issue = pending.issue; break; }
            }
            if (const auto goal = std::get_if<ProblemQuantityGoal>(&proposal.goal)) {
                for (const auto& binding : proposal.bindings) {
                    if (binding.quantity == goal->quantity
                        && proposal.numbers[binding.number].precision != QuantityPrecision::Exact) {
                        issue = ProblemIssue::NonExact;
                    }
                }
            }
            require(result.issue == issue);
        }
        std::vector<ProblemPremiseRef> expected_used;
        bool formal = !request.context.units.empty(), question = false, source = false;
        const auto mark = [&](PremiseBasis value) {
            if (value == PremiseBasis::Formal) formal = true;
            else if (value == PremiseBasis::Question) question = true;
            else if (value == PremiseBasis::Source) source = true;
            else require(false);
        };
        const auto mark_span = [&](const EvidenceSpan& span) { mark(request.context.views[span.view].basis); };
        const auto mark_origin = [&](const ProblemOrigin& origin) {
            mark(origin.basis);
            for (const auto& support : origin.supports) mark_span(support);
        };
        for (const auto& [kind,index] : used) {
            expected_used.push_back({kind,index});
            switch (kind) {
            case ProblemPremiseKind::Number: {
                const auto& number = proposal.numbers[index];
                mark_span(number.numeral);
                if (number.unit_span) mark_span(*number.unit_span);
                if (number.qualifier) mark_span(*number.qualifier);
                break;
            }
            case ProblemPremiseKind::Binding: mark_origin(proposal.bindings[index].origin); break;
            case ProblemPremiseKind::Equation: mark_origin(proposal.equations[index].origin); break;
            case ProblemPremiseKind::Rule: mark_origin(proposal.logic_origins[index]); break;
            }
        }
        require(result.used == expected_used && result.formal_assumptions == formal
            && result.question_assumptions == question && result.source_interpretations == source);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}
} // namespace axiom
