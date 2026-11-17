#include "problem.hpp"

#include <algorithm>
#include <functional>
#include <set>
#include <stdexcept>
#include <string_view>

namespace axiom {

ProblemResult execute_problem(const ProblemRequest& request) {
    // Admission errors are caller errors, not evidence of an unresolved goal.
    const auto admitted = admit_problem(request);
    const auto& proposal = request.proposal;
    ProblemResult result;
    result.request = request;
    result.formal_assumptions = !request.context.units.empty();
    result.work = std::min(admitted.admission_work, request.limits.work);

    const auto charge = [&](std::uint64_t amount) {
        if (amount > request.limits.work - result.work) {
            result.work = request.limits.work;
            throw ProblemIssue::WorkLimit;
        }
        result.work += amount;
    };
    const auto bounded = [](Rational value) {
        try {
            return checked_problem_rational(std::move(value));
        } catch (const std::runtime_error&) {
            throw ProblemIssue::WireLimit;
        }
    };
    const auto use = [&](ProblemPremiseKind kind, std::uint32_t index) {
        result.used.push_back({kind, index});
    };
    const auto basis = [&](PremiseBasis value) {
        switch (value) {
        case PremiseBasis::Formal: result.formal_assumptions = true; break;
        case PremiseBasis::Question: result.question_assumptions = true; break;
        case PremiseBasis::Source: result.source_interpretations = true; break;
        }
    };
    const auto span_basis = [&](const EvidenceSpan& span) {
        basis(request.context.views[span.view].basis);
    };
    const auto origin_basis = [&](const ProblemOrigin& origin) {
        basis(origin.basis);
        for (const auto& span : origin.supports) {
            span_basis(span);
        }
    };
    const auto finish = [&]() {
        std::sort(result.used.begin(), result.used.end(), [](const auto& a, const auto& b) {
            return std::pair{a.kind, a.index} < std::pair{b.kind, b.index};
        });
        result.used.erase(std::unique(result.used.begin(), result.used.end()), result.used.end());
        for (const auto& ref : result.used) {
            switch (ref.kind) {
            case ProblemPremiseKind::Number: {
                const auto& number = proposal.numbers[ref.index];
                span_basis(number.numeral);
                if (number.unit_span) span_basis(*number.unit_span);
                if (number.qualifier) span_basis(*number.qualifier);
                break;
            }
            case ProblemPremiseKind::Binding:
                origin_basis(proposal.bindings[ref.index].origin);
                break;
            case ProblemPremiseKind::Equation:
                origin_basis(proposal.equations[ref.index].origin);
                break;
            case ProblemPremiseKind::Rule:
                origin_basis(proposal.logic_origins[ref.index]);
                break;
            }
        }
        return result;
    };
    std::vector<std::uint32_t> values(proposal.quantities.size(), no_step);
    const auto append = [&](ProblemStep step) {
        if (result.steps.size() >= request.limits.steps) {
            throw ProblemIssue::WorkLimit;
        }
        const auto index = static_cast<std::uint32_t>(result.steps.size());
        result.steps.push_back(std::move(step));
        return index;
    };
    const auto scalar = [&](const Request& scalar_request) {
        charge(scalar_request.expression.size());
        try {
            return execute(scalar_request);
        } catch (const std::runtime_error& error) {
            const std::string_view message(error.what());
            if (message == "nonlinear multiplication unsupported" ||
                message == "variable denominator unsupported") {
                throw ProblemIssue::NonAffine;
            }
            if (message == "division by zero") throw ProblemIssue::Undefined;
            if (message == "rational bit limit exceeded") throw ProblemIssue::WireLimit;
            throw;
        }
    };

    try {
        if (admitted.admission_work > request.limits.work) throw ProblemIssue::WorkLimit;

        // A guard's cost is its finite grounding (atoms, substitutions and
        // grounded term visits), followed by the v2 search-event visits.
        const auto guard_for = [&](const Atom& goal) {
            for (std::uint32_t i = 0; i < result.guards.size(); ++i) {
                if (result.guards[i].goal == goal) return i;
            }
            const auto power = [&](std::uint32_t exponent) {
                std::uint64_t count = 1;
                while (exponent-- != 0) count *= proposal.logic.constants.size();
                return count;
            };
            std::uint64_t grounding_work = 0;
            for (const auto& predicate : proposal.logic.predicates) {
                grounding_work += power(predicate.arity);
            }
            for (const auto& rule : proposal.logic.rules) {
                std::uint64_t visits = 1 + rule.variables + 1 + rule.head.terms.size();
                for (const auto& premise : rule.premises) visits += 1 + premise.terms.size();
                grounding_work += power(rule.variables) * visits;
            }
            charge(grounding_work);
            const auto remaining = request.limits.work - result.work;
            if (remaining == 0) throw ProblemIssue::WorkLimit;
            RelationalRequest query;
            query.module = proposal.logic;
            query.goal = goal;
            query.limits.events = static_cast<std::uint32_t>(
                std::min<std::uint64_t>(4096, remaining));
            query.limits.depth = 128;
            RelationalResult certificate;
            try {
                certificate = execute_relational(query);
            } catch (const std::runtime_error& error) {
                const std::string_view message(error.what());
                if (message == "worst-case relational result exceeds 1 MiB" ||
                    message == "relational size limit exceeded") {
                    throw ProblemIssue::WireLimit;
                }
                throw;
            }
            charge(certificate.proof.history.size());
            const auto index = static_cast<std::uint32_t>(result.guards.size());
            result.guards.push_back({goal, std::move(certificate)});
            for (std::uint32_t i = 0; i < proposal.logic.rules.size(); ++i) {
                use(ProblemPremiseKind::Rule, i);
            }
            if (result.guards.back().certificate.proof.limited) throw ProblemIssue::WorkLimit;
            return index;
        };
        std::vector<std::optional<std::uint32_t>> equation_guards(proposal.equations.size());
        for (std::uint32_t i = 0; i < proposal.equations.size(); ++i) {
            if (proposal.equations[i].guard) {
                equation_guards[i] = guard_for(*proposal.equations[i].guard);
            }
        }
        if (const auto* goal = std::get_if<ProblemAtomGoal>(&proposal.goal)) {
            result.logical_goal_guard = guard_for(goal->atom);
        }
        for (std::uint32_t i = 0; i < result.guards.size(); ++i) {
            if (result.guards[i].certificate.proof.status == Status::Contradiction) {
                result.status = ProblemStatus::Conflict;
                result.issue = ProblemIssue::None;
                result.conflict = ProblemGuardConflict{i};
                return finish();
            }
        }

        for (std::uint32_t i = 0; i < proposal.bindings.size(); ++i) {
            charge(1);
            const auto& binding = proposal.bindings[i];
            const auto& number = proposal.numbers[binding.number];
            if (number.precision != QuantityPrecision::Exact ||
                !proposal.quantities[binding.quantity].scope.resolved) continue;
            const auto value = bounded(admitted.canonical_numbers[binding.number]);
            const auto previous = values[binding.quantity];
            if (previous != no_step && result.steps[previous].canonical_value == value) continue;
            const auto index = append({binding.quantity, value,
                ProblemBindingStep{i, admitted.lexical_numbers[binding.number],
                                   request.context.units[number.unit].scale}});
            use(ProblemPremiseKind::Number, binding.number);
            use(ProblemPremiseKind::Binding, i);
            if (previous != no_step) {
                result.status = ProblemStatus::Conflict;
                result.issue = ProblemIssue::None;
                result.conflict = ProblemValueConflict{previous, index};
                return finish();
            }
            values[binding.quantity] = index;
        }

        bool changed = true;
        while (changed) {
            changed = false;
            result.pending_equations.clear();
            for (std::uint32_t i = 0; i < proposal.equations.size(); ++i) {
                charge(1);
                const auto& equation = proposal.equations[i];
                const auto guard = equation_guards[i];
                if (!equation.scope.resolved ||
                    (guard && result.guards[*guard].certificate.proof.status != Status::Proved)) {
                    result.pending_equations.push_back({i, ProblemIssue::MissingPremise});
                    continue;
                }

                std::set<std::uint32_t> unknowns, dependencies, numbers, visited;
                bool nonexact = false;
                const std::function<void(std::uint32_t)> inspect = [&](std::uint32_t node) {
                    if (!visited.insert(node).second) return;
                    charge(1);
                    const auto& entry = proposal.nodes[node];
                    if (const auto* quantity = std::get_if<ProblemQuantityNode>(&entry)) {
                        const auto step = values[quantity->quantity];
                        if (step == no_step) unknowns.insert(quantity->quantity);
                        else dependencies.insert(step);
                    } else if (const auto* number = std::get_if<ProblemNumberNode>(&entry)) {
                        numbers.insert(number->number);
                        nonexact = nonexact ||
                            proposal.numbers[number->number].precision != QuantityPrecision::Exact;
                    } else if (const auto* negate = std::get_if<ProblemNegateNode>(&entry)) {
                        inspect(negate->operand);
                    } else {
                        const auto& binary = std::get<ProblemBinaryNode>(entry);
                        inspect(binary.left);
                        inspect(binary.right);
                    }
                };
                inspect(equation.left);
                inspect(equation.right);
                if (nonexact || unknowns.size() > 1) {
                    result.pending_equations.push_back(
                        {i, nonexact ? ProblemIssue::NonExact : ProblemIssue::MissingPremise});
                    continue;
                }

                Request math;
                math.operation = unknowns.empty() ? Operation::Evaluate : Operation::Solve;
                const auto emit = [&](Instruction instruction) {
                    if (math.expression.size() == 512) throw ProblemIssue::WireLimit;
                    math.expression.push_back(std::move(instruction));
                };
                const std::function<void(std::uint32_t)> lower = [&](std::uint32_t node) {
                    charge(1);
                    const auto& entry = proposal.nodes[node];
                    if (const auto* quantity = std::get_if<ProblemQuantityNode>(&entry)) {
                        const auto step = values[quantity->quantity];
                        if (step == no_step) emit({Code::Variable, 0});
                        else emit({Code::Constant, result.steps[step].canonical_value});
                    } else if (const auto* number = std::get_if<ProblemNumberNode>(&entry)) {
                        emit({Code::Constant, admitted.canonical_numbers[number->number]});
                    } else if (const auto* negate = std::get_if<ProblemNegateNode>(&entry)) {
                        lower(negate->operand);
                        emit({Code::Negate, 0});
                    } else {
                        const auto& binary = std::get<ProblemBinaryNode>(entry);
                        lower(binary.left);
                        lower(binary.right);
                        emit({binary.operation, 0});
                    }
                };
                lower(equation.left);
                lower(equation.right);
                emit({Code::Subtract, 0});
                Result solved;
                try {
                    solved = scalar(math);
                } catch (ProblemIssue issue) {
                    if (issue != ProblemIssue::NonAffine && issue != ProblemIssue::Undefined) throw;
                    result.pending_equations.push_back({i, issue});
                    continue;
                }
                const std::vector<std::uint32_t> prior(dependencies.begin(), dependencies.end());
                const auto use_equation = [&]() {
                    use(ProblemPremiseKind::Equation, i);
                    for (auto number : numbers) use(ProblemPremiseKind::Number, number);
                };
                if (solved.status == Status::Inconsistent ||
                    (unknowns.empty() && solved.values.front() != 0)) {
                    use_equation();
                    result.status = ProblemStatus::Conflict;
                    result.issue = ProblemIssue::None;
                    result.conflict = ProblemEquationConflict{i, prior, guard};
                    return finish();
                }
                if (unknowns.empty()) continue;
                if (solved.status == Status::Infinite) {
                    result.pending_equations.push_back({i, ProblemIssue::MissingPremise});
                    continue;
                }
                const auto quantity = *unknowns.begin();
                const auto index = append({quantity, bounded(solved.values.front()),
                                          ProblemEquationStep{i, prior, guard}});
                use_equation();
                values[quantity] = index;
                changed = true;
            }
        }

        if (const auto* goal = std::get_if<ProblemQuantityGoal>(&proposal.goal)) {
            const auto index = values[goal->quantity];
            if (index != no_step) {
                Request conversion;
                conversion.expression = {{Code::Constant, result.steps[index].canonical_value},
                    {Code::Constant, request.context.units[goal->unit].scale}, {Code::Divide, 0}};
                const auto converted = scalar(conversion);
                result.answer = bounded(converted.values.front());
                result.answer_unit = goal->unit;
                result.goal_step = index;
                result.status = ProblemStatus::Checked;
                result.issue = ProblemIssue::None;
            }
        } else if (result.guards[*result.logical_goal_guard].certificate.proof.status == Status::Proved
                   || result.guards[*result.logical_goal_guard].certificate.proof.status == Status::Disproved) {
            result.status = ProblemStatus::Checked;
            result.issue = ProblemIssue::None;
        }
        if (result.status != ProblemStatus::Checked) {
            for (const auto& pending : result.pending_equations) {
                if (pending.issue != ProblemIssue::MissingPremise) {
                    result.issue = pending.issue;
                    break;
                }
            }
            if (const auto* goal = std::get_if<ProblemQuantityGoal>(&proposal.goal)) {
                for (const auto& binding : proposal.bindings) {
                    if (binding.quantity == goal->quantity &&
                        proposal.numbers[binding.number].precision != QuantityPrecision::Exact) {
                        result.issue = ProblemIssue::NonExact;
                    }
                }
            }
        }
    } catch (ProblemIssue issue) {
        result.status = ProblemStatus::Limited;
        result.issue = issue;
        result.answer.reset();
        result.answer_unit = no_step;
        result.goal_step.reset();
    }
    return finish();
}

ProblemCorrectionSession::ProblemCorrectionSession(const ProblemRequest& initial, std::uint32_t attempts)
    : initial_(initial), limit_(attempts) {
    if (attempts == 0 || attempts > 8) {
        throw std::runtime_error("Correction attempts must be 1..8");
    }
    append(initial.proposal, {}, std::nullopt);
}

void ProblemCorrectionSession::correct(const ProblemProposal& proposal, std::string reason,
                                      std::optional<std::uint32_t> rejected_previous_step) {
    if (reason.empty() || reason.size() > 1024 || reason.find('\0') != std::string::npos) {
        throw std::runtime_error("Correction requires a bounded reason");
    }
    if (rejected_previous_step &&
        *rejected_previous_step >= attempts_.back().result.steps.size()) {
        throw std::runtime_error("Correction names an invalid previous step");
    }
    if (proposal == attempts_.back().result.request.proposal) {
        throw std::runtime_error("Correction must change the proposal");
    }
    if (proposal.goal != initial_.proposal.goal ||
        proposal.entities != initial_.proposal.entities ||
        proposal.quantities != initial_.proposal.quantities ||
        proposal.logic.constants != initial_.proposal.logic.constants ||
        proposal.logic.predicates != initial_.proposal.logic.predicates) {
        throw std::runtime_error("Correction must preserve the declared task vocabulary and goal");
    }
    append(proposal, std::move(reason), rejected_previous_step);
}

void ProblemCorrectionSession::append(const ProblemProposal& proposal, std::string reason,
                                     std::optional<std::uint32_t> rejected_previous_step) {
    if (attempts_.size() == limit_ || work_ == initial_.limits.work) {
        throw std::runtime_error("Correction session budget exhausted");
    }
    auto request = initial_;
    request.proposal = proposal;
    request.limits.work -= work_;
    auto result = execute_problem(request);
    if (!verify_problem_result(request, result)) {
        throw std::runtime_error("Correction attempt failed independent verification");
    }
    work_ += result.work;
    attempts_.push_back({std::move(result), std::move(reason), rejected_previous_step});
}

} // namespace axiom
