#include "quantified_equations.hpp"
#include "problem_fixture.hpp"

#include <iostream>
#include <source_location>

namespace {
using namespace axiom;
std::size_t checks = 0;
void check(bool value, const std::source_location where = std::source_location::current()) {
    ++checks;
    if (!value) throw std::runtime_error("Quantified equation assertion at " + std::to_string(where.line()));
}
template<class F> void rejects(F operation) {
    ++checks;
    try { operation(); }
    catch (const std::runtime_error&) { return; }
    throw std::runtime_error("Expected quantified equation rejection");
}
ProblemRequest base() {
    ProblemRequest result;
    result.context.unit_registry_id = "synthetic";
    result.context.unit_registry_version = "1";
    result.context.units = {{"one", "1", {}, 1, {"1"}}};
    result.context.views = {make_evidence_view("law", PremiseBasis::Source,
        "For every eligible object, output equals 2 times input. A input is exactly 3; B input is exactly 7.",
        "synthetic", "conditional-law", "literal-v1")};
    auto& proposal = result.proposal;
    proposal.entities = {"A", "B"};
    proposal.quantities = {{"input", 0, {}, 0}, {"output", 0, {}, 0},
        {"input", 1, {}, 0}, {"output", 1, {}, 0}};
    for (const auto* numeral : {"2", "3", "7"})
        proposal.numbers.push_back({problem_fixture::span(result, 0, numeral),
            std::nullopt, 0, QuantityPrecision::Exact, std::nullopt});
    proposal.bindings = {{0, 1, problem_fixture::origin(result, 0, "input-A")},
        {2, 2, problem_fixture::origin(result, 0, "input-B")}};
    proposal.goal = ProblemQuantityGoal{1, 0};
    proposal.logic.name = "synthetic_eligibility";
    proposal.logic.version = "1";
    proposal.logic.constants = proposal.entities;
    proposal.logic.predicates = {{"eligible", 1}};
    proposal.logic.rules = {{0, problem_fixture::atom(0, {0}), {}, "formal-eligibility"}};
    proposal.logic_origins = {{PremiseBasis::Formal, "A-is-eligible", {}}};
    return result;
}
QuantifiedEquation law(const ProblemRequest& request) {
    QuantifiedEquation result;
    result.variables = 1;
    result.quantities = {{"input", {TermKind::Variable, 0}, {}, 0},
        {"output", {TermKind::Variable, 0}, {}, 0}};
    result.nodes = {ProblemQuantityNode{1}, ProblemQuantityNode{0},
        ProblemNumberNode{0}, ProblemBinaryNode{Code::Multiply, 1, 2}};
    result.left = 0;
    result.right = 3;
    result.guard = Atom{0, false, {{TermKind::Variable, 0}}};
    result.origin = problem_fixture::origin(request, 0, "universal-law");
    return result;
}
}

int main() {
    try {
        const auto original = base();
        const auto schema = law(original);
        const auto expanded = instantiate_equations(original, {schema});
        check(expanded.instances.size() == 2 && expanded.work == 16);
        check(expanded.instances[0].substitution == std::vector<std::uint32_t>{0});
        check(expanded.instances[1].substitution == std::vector<std::uint32_t>{1});
        check(expanded.instances[0].quantities == std::vector<std::uint32_t>({0, 1}));
        check(expanded.instances[1].quantities == std::vector<std::uint32_t>({2, 3}));
        check(expanded.request.context == original.context);
        check(expanded.request.proposal.goal == original.proposal.goal);
        check(expanded.request.proposal.logic == original.proposal.logic);
        check(expanded.request.proposal.bindings == original.proposal.bindings);
        check(expanded.request.proposal.numbers == original.proposal.numbers);
        check(expanded.request.proposal.quantities == original.proposal.quantities);
        check(original.proposal.equations.empty());
        check(expanded.request.proposal.equations[0].origin == schema.origin);
        check(expanded.request.proposal.equations[1].guard == problem_fixture::atom(0, {1}));
        check(verify_equation_expansion(original, {schema}, expanded));
        const auto answer = execute_problem(expanded.request);
        check(answer.status == ProblemStatus::Checked && answer.answer == Rational(6));
        check(answer.source_interpretations && answer.formal_assumptions);
        check(verify_problem_result(expanded.request, answer));
        auto corrupted_answer = answer;
        corrupted_answer.answer = Rational(14);
        check(!verify_problem_result(expanded.request, corrupted_answer));

        auto other = expanded.request;
        other.proposal.goal = ProblemQuantityGoal{3, 0};
        const auto unproved_guard = execute_problem(other);
        check(unproved_guard.status == ProblemStatus::Unresolved && !unproved_guard.answer);
        check(verify_problem_result(other, unproved_guard));
        auto unguarded = schema;
        unguarded.guard.reset();
        auto all_objects = instantiate_equations(original, {unguarded});
        all_objects.request.proposal.goal = ProblemQuantityGoal{3, 0};
        const auto all_answer = execute_problem(all_objects.request);
        check(all_answer.answer == Rational(14) && verify_problem_result(all_objects.request, all_answer));

        auto changed = expanded;
        changed.instances[0].substitution[0] = 1;
        check(!verify_equation_expansion(original, {schema}, changed));
        changed = expanded;
        changed.request.proposal.equations[0].guard.reset();
        check(!verify_equation_expansion(original, {schema}, changed));
        changed = expanded;
        changed.request.proposal.equations[0].origin.label = "different-source";
        check(!verify_equation_expansion(original, {schema}, changed));
        changed = expanded;
        changed.instances.pop_back();
        check(!verify_equation_expansion(original, {schema}, changed));
        changed = expanded;
        --changed.work;
        check(!verify_equation_expansion(original, {schema}, changed));
        check(instantiate_equations(original, {schema}, 16) == expanded);
        rejects([&] { (void)instantiate_equations(original, {schema}, 15); });
        rejects([&] { (void)instantiate_equations(original, {schema}, 0); });
        check(!verify_equation_expansion(original, {schema}, expanded, 15));

        auto bad = schema;
        bad.quantities[0].entity.index = 1;
        rejects([&] { (void)instantiate_equations(original, {bad}); });
        bad = schema;
        bad.guard->terms[0].index = 1;
        rejects([&] { (void)instantiate_equations(original, {bad}); });
        bad = schema;
        bad.nodes[1] = ProblemNegateNode{1};
        rejects([&] { (void)instantiate_equations(original, {bad}); });
        bad = schema;
        bad.origin.supports.clear();
        rejects([&] { (void)instantiate_equations(original, {bad}); });
        bad = schema;
        bad.quantities[0].unit = 1;
        rejects([&] { (void)instantiate_equations(original, {bad}); });
        bad = schema;
        bad.quantities[0].scope.qualifiers = {{"run", "different"}};
        rejects([&] { (void)instantiate_equations(original, {bad}); });
        auto dimensioned_base = original;
        dimensioned_base.context.units.push_back({"metre", "m", {0, 1, 0, 0, 0, 0, 0}, 1, {"m"}});
        bad = schema;
        bad.quantities[1].property = "dimensioned_output";
        bad.quantities[1].unit = 1;
        rejects([&] { (void)instantiate_equations(dimensioned_base, {bad}); });
        bad = schema;
        bad.variables = 0;
        rejects([&] { (void)instantiate_equations(original, {bad}); });
        bad = schema;
        bad.variables = 5;
        rejects([&] { (void)instantiate_equations(original, {bad}); });
        rejects([&] { (void)instantiate_equations(original, std::vector<QuantifiedEquation>(33, schema)); });
        auto no_entities = original;
        no_entities.proposal.entities.clear();
        no_entities.proposal.quantities.clear();
        no_entities.proposal.bindings.clear();
        no_entities.proposal.logic = {};
        no_entities.proposal.logic_origins.clear();
        rejects([&] { (void)instantiate_equations(no_entities, {schema}); });

        auto pairs_base = original;
        pairs_base.proposal.logic.predicates.push_back({"paired", 2});
        auto pairs = schema;
        pairs.variables = 2;
        pairs.quantities[1].entity.index = 1;
        pairs.guard = Atom{1, false, {{TermKind::Variable, 0}, {TermKind::Variable, 1}}};
        const auto cartesian = instantiate_equations(pairs_base, {pairs});
        check(cartesian.instances.size() == 4);
        check(cartesian.instances[0].substitution == std::vector<std::uint32_t>({0, 0}));
        check(cartesian.instances[1].substitution == std::vector<std::uint32_t>({0, 1}));
        check(cartesian.instances[2].substitution == std::vector<std::uint32_t>({1, 0}));
        check(cartesian.instances[3].substitution == std::vector<std::uint32_t>({1, 1}));
        check(cartesian.request.proposal.equations[1].guard == problem_fixture::atom(1, {0, 1}));
        check(verify_equation_expansion(pairs_base, {pairs}, cartesian));

        auto signed_base = original;
        signed_base.proposal.logic.rules.push_back({0, problem_fixture::atom(0, {1}, true), {}, "negative-B"});
        signed_base.proposal.logic_origins.push_back({PremiseBasis::Formal, "B-explicitly-ineligible", {}});
        auto negative = schema;
        negative.guard->negative = true;
        auto signed_expansion = instantiate_equations(signed_base, {negative});
        signed_expansion.request.proposal.goal = ProblemQuantityGoal{3, 0};
        const auto negative_answer = execute_problem(signed_expansion.request);
        check(negative_answer.answer == Rational(14));
        check(verify_problem_result(signed_expansion.request, negative_answer));
        check(signed_expansion.request.proposal.equations[1].guard->negative);

        const auto repeated = instantiate_equations(original, {schema, schema});
        check(repeated.instances.size() == 4 && repeated.request.proposal.equations.size() == 4);
        check(repeated.instances[2].schema == 1 && repeated.instances[2].substitution == std::vector<std::uint32_t>{0});
        auto constant = schema;
        constant.quantities[0].entity = {TermKind::Constant, 0};
        const auto anchored = instantiate_equations(original, {constant});
        check(anchored.instances[0].quantities[0] == 0 && anchored.instances[1].quantities[0] == 0);
        const auto empty = instantiate_equations(original, {});
        check(empty.request == original && empty.instances.empty() && empty.work == 0);
        auto extended = original;
        extended.proposal.entities.push_back("C");
        extended.proposal.logic.constants = extended.proposal.entities;
        const auto later = instantiate_equations(extended, {schema});
        check(later.instances.size() == 3 && later.instances[2].substitution == std::vector<std::uint32_t>{2});
        check(later.request.proposal.equations[2].guard == problem_fixture::atom(0, {2}));
        check(verify_equation_expansion(extended, {schema}, later));
        check(!verify_equation_expansion(original, {schema}, later));
        std::cout << "Quantified equation checks passed: " << checks << '\n';
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
