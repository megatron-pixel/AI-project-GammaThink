#include "problem_fixture.hpp"

#include <algorithm>
#include <iostream>
#include <source_location>
#include <stdexcept>

namespace {
std::size_t checks = 0;
void check(bool value, const std::source_location location = std::source_location::current()) {
    ++checks;
    if (!value) throw std::runtime_error("Problem integration assertion " + std::to_string(checks)
        + " at line " + std::to_string(location.line()));
}
template<class Action> void rejects(Action action) {
    ++checks;
    bool rejected = false;
    try { action(); } catch (const std::exception&) { rejected = true; }
    if (!rejected) throw std::runtime_error("Problem integration rejection " + std::to_string(checks));
}
void numbers(axiom::ProblemRequest& request, const std::string& density,
             const std::string& volume, const std::string& tare) {
    using namespace axiom;
    request.context.views[0] = make_evidence_view("source_a",PremiseBasis::Source,
        "Run R: chamber A contains F; density exactly " + density + " kg/m^3.",
        "synthetic_archive_a","density","synthetic-v1");
    request.context.views[1] = make_evidence_view("source_b",PremiseBasis::Source,
        "Run R: A calibrated; volume exactly " + volume + " L; tare mass exactly " + tare + " kg.",
        "synthetic_archive_b","volume_and_tare","synthetic-v1");
    using problem_fixture::span;
    request.proposal.numbers = {
        {span(request,0,density),span(request,0,"kg/m^3"),3,QuantityPrecision::Exact,span(request,0,"exactly")},
        {span(request,1,volume),span(request,1,"L"),2,QuantityPrecision::Exact,span(request,1,"exactly")},
        {span(request,1,tare,volume == tare ? 1 : 0),span(request,1,"kg"),0,
         QuantityPrecision::Exact,span(request,1,"exactly",1)}
    };
    request.proposal.bindings[0].origin = problem_fixture::origin(request,0,"density_assertion");
    request.proposal.bindings[1].origin = problem_fixture::origin(request,1,"volume_assertion");
    request.proposal.bindings[2].origin = problem_fixture::origin(request,1,"tare_assertion");
    request.proposal.logic_origins[0] = problem_fixture::origin(request,0,"containment_assertion");
    request.proposal.logic_origins[1] = problem_fixture::origin(request,1,"calibration_assertion");
}
axiom::ProblemRequest scalar_problem() {
    using namespace axiom;
    ProblemRequest request;
    request.context.unit_registry_id = "scalar";
    request.context.unit_registry_version = "1";
    request.context.units = {{"one","1",{},1,{"1"}}};
    request.context.views = {make_evidence_view("question",PremiseBasis::Question,
        "Given constants 0 and 1. Determine x.","","question","literal-v1")};
    request.proposal.entities = {"x"};
    request.proposal.quantities = {{"scalar",0,{},0}};
    request.proposal.numbers = {
        {problem_fixture::span(request,0,"0"),std::nullopt,0,QuantityPrecision::Exact,std::nullopt},
        {problem_fixture::span(request,0,"1"),std::nullopt,0,QuantityPrecision::Exact,std::nullopt}
    };
    request.proposal.nodes = {
        ProblemQuantityNode{0},ProblemNumberNode{0},ProblemNumberNode{1},
        ProblemBinaryNode{Code::Subtract,0,0}
    };
    request.proposal.equations = {{3,1,{},std::nullopt,{PremiseBasis::Formal,"given_equation",{}}}};
    request.proposal.goal = ProblemQuantityGoal{0,0};
    return request;
}
}
int main() {
    try {
        using namespace axiom;
        using problem_fixture::span;
        const auto request = problem_fixture::request();
        const auto result = execute_problem(request);
        check(result.status == ProblemStatus::Checked && result.answer == Rational(17,5));
        check(result.answer_unit == 0 && result.goal_step.has_value());
        check(result.source_interpretations && result.formal_assumptions);
        check(result.guards.size() == 1 && result.guards[0].certificate.proof.status == Status::Proved);
        check(verify_problem_result(request,result));
        check(std::get<ProblemEquationStep>(result.steps[*result.goal_step].justification).dependencies.size() == 3);
        auto changed = result;
        changed.answer = Rational(18,5);
        check(!verify_problem_result(request,changed));
        changed = result;
        changed.steps[0].canonical_value += 1;
        check(!verify_problem_result(request,changed));
        changed = result;
        std::get<ProblemBindingStep>(changed.steps[1].justification).unit_scale = Rational(1,100);
        check(!verify_problem_result(request,changed));
        changed = result;
        std::get<ProblemBindingStep>(changed.steps[0].justification).lexical_value = 1300;
        check(!verify_problem_result(request,changed));
        changed = result;
        std::get<ProblemEquationStep>(changed.steps[*changed.goal_step].justification).dependencies.pop_back();
        check(!verify_problem_result(request,changed));
        changed = result;
        std::get<ProblemEquationStep>(changed.steps[*changed.goal_step].justification).guard.reset();
        check(!verify_problem_result(request,changed));
        changed = result;
        changed.guards[0].certificate.instances.pop_back();
        check(!verify_problem_result(request,changed));
        changed = result;
        changed.request.proposal.numbers[0].numeral.offset += 1;
        check(!verify_problem_result(request,changed));
        changed = result;
        changed.request.proposal.quantities[0].scope.qualifiers[0].second = "S";
        check(!verify_problem_result(request,changed));
        changed = result;
        changed.source_interpretations = false;
        check(!verify_problem_result(request,changed));
        changed = result;
        changed.used.erase(changed.used.begin());
        check(!verify_problem_result(request,changed));

        auto scenario = request;
        scenario.proposal.goal = ProblemQuantityGoal{3,6};
        const auto grams = execute_problem(scenario);
        check(grams.answer == Rational(3400) && verify_problem_result(scenario,grams));
        scenario = request;
        scenario.proposal.logic.rules.erase(scenario.proposal.logic.rules.begin() + 1);
        scenario.proposal.logic_origins.erase(scenario.proposal.logic_origins.begin() + 1);
        const auto uncalibrated = execute_problem(scenario);
        check(uncalibrated.status == ProblemStatus::Unresolved && !uncalibrated.answer);
        check(verify_problem_result(scenario,uncalibrated));
        changed = uncalibrated;
        changed.status = ProblemStatus::Checked;
        changed.issue = ProblemIssue::None;
        changed.answer = Rational(17,5);
        changed.answer_unit = 0;
        check(!verify_problem_result(scenario,changed));
        scenario = request;
        scenario.proposal.numbers[0].precision = QuantityPrecision::Approximate;
        const auto approximate = execute_problem(scenario);
        check(approximate.status != ProblemStatus::Checked && !approximate.answer);
        check(verify_problem_result(scenario,approximate));
        scenario = request;
        for (auto& quantity : scenario.proposal.quantities) quantity.scope.resolved = false;
        scenario.proposal.equations[0].scope.resolved = false;
        const auto unscoped = execute_problem(scenario);
        check(unscoped.status == ProblemStatus::Unresolved && !unscoped.answer);
        check(verify_problem_result(scenario,unscoped));
        scenario = request;
        scenario.limits.work = 1;
        const auto limited = execute_problem(scenario);
        check(limited.status == ProblemStatus::Limited && !limited.answer);
        check(verify_problem_result(scenario,limited));
        scenario = request;
        scenario.limits.steps = 2;
        const auto short_proof = execute_problem(scenario);
        check(short_proof.status == ProblemStatus::Limited && !short_proof.answer);
        check(verify_problem_result(scenario,short_proof));

        scenario = request;
        scenario.context.views.push_back(make_evidence_view("other_density",PremiseBasis::Source,
            "In run R, fluid F has density exactly 1.3e3 kg/m^3.",
            "synthetic_archive_c","density","synthetic-v1"));
        scenario.proposal.numbers.push_back({span(scenario,3,"1.3e3"),span(scenario,3,"kg/m^3"),
            3,QuantityPrecision::Exact,span(scenario,3,"exactly")});
        scenario.proposal.bindings.push_back({0,3,problem_fixture::origin(scenario,3,"other_density")});
        const auto conflict = execute_problem(scenario);
        check(conflict.status == ProblemStatus::Conflict && !conflict.answer && conflict.conflict);
        check(verify_problem_result(scenario,conflict));
        changed = conflict;
        std::get<ProblemValueConflict>(*changed.conflict).second = 0;
        check(!verify_problem_result(scenario,changed));
        scenario.proposal.entities.push_back("S");
        scenario.proposal.logic.constants = scenario.proposal.entities;
        const auto run_position = scenario.context.views[3].text.find("run R");
        scenario.context.views[3].text.replace(run_position,5,"run S");
        scenario.context.views[3].digest = evidence_digest(scenario.context.views[3].text);
        scenario.proposal.quantities.push_back({"density",1,{true,{{"run","S"}}},3});
        scenario.proposal.bindings.back().quantity = 4;
        const auto separated = execute_problem(scenario);
        check(separated.status == ProblemStatus::Checked && separated.answer == Rational(17,5));
        check(verify_problem_result(scenario,separated));

        scenario = request;
        scenario.proposal.logic.rules.push_back({0,problem_fixture::atom(2,{0,2},true),{},"given_negative"});
        scenario.proposal.logic_origins.push_back({PremiseBasis::Formal,"given_negative",{}});
        const auto contradictory = execute_problem(scenario);
        check(contradictory.status == ProblemStatus::Conflict && !contradictory.answer);
        check(verify_problem_result(scenario,contradictory));
        scenario = request;
        auto& volume_view = scenario.context.views[1];
        const auto position = volume_view.text.find(" L");
        volume_view.text[position + 1] = 'm';
        volume_view.digest = evidence_digest(volume_view.text);
        scenario.proposal.numbers[1].unit = 5;
        rejects([&] { (void)execute_problem(scenario); });
        scenario = request;
        scenario.proposal.quantities.push_back({"fluid_mass",0,scenario.proposal.quantities[0].scope,0});
        scenario.proposal.nodes.push_back(ProblemQuantityNode{4});
        scenario.proposal.nodes.push_back(ProblemBinaryNode{Code::Add,6,3});
        scenario.proposal.equations[0].right = 7;
        scenario.proposal.equations.push_back({6,2,scenario.proposal.quantities[0].scope,
            scenario.proposal.equations[0].guard,{PremiseBasis::Formal,"given_fluid_mass",{}}});
        const auto chained = execute_problem(scenario);
        check(chained.answer == Rational(17,5) && chained.steps.size() == 5);
        check(chained.guards.size() == 1 && verify_problem_result(scenario,chained));

        scenario = request;
        scenario.context.views.push_back(make_evidence_view("given_total",PremiseBasis::Question,
            "The total mass is exactly 3.5 kg.","","given_total","literal-v1"));
        scenario.proposal.numbers.push_back({span(scenario,3,"3.5"),span(scenario,3,"kg"),0,
            QuantityPrecision::Exact,span(scenario,3,"exactly")});
        scenario.proposal.bindings.push_back({3,3,problem_fixture::origin(scenario,3,"given_total")});
        const auto inconsistent = execute_problem(scenario);
        check(inconsistent.status == ProblemStatus::Conflict && inconsistent.conflict
            && std::holds_alternative<ProblemEquationConflict>(*inconsistent.conflict));
        check(verify_problem_result(scenario,inconsistent));
        changed = result;
        changed.request = scenario;
        changed.work = scenario.limits.work;
        check(!verify_problem_result(scenario,changed));
        scenario.context.views[3] = make_evidence_view("given_total",PremiseBasis::Question,
            "The total mass is exactly 3.4 kg.","","given_total","literal-v1");
        scenario.proposal.numbers.back().numeral = span(scenario,3,"3.4");
        scenario.proposal.bindings.back().origin = problem_fixture::origin(scenario,3,"given_total");
        scenario.proposal.bindings.erase(scenario.proposal.bindings.begin() + 1);
        scenario.proposal.goal = ProblemQuantityGoal{1,2};
        const auto inverse = execute_problem(scenario);
        check(inverse.status == ProblemStatus::Checked && inverse.answer == Rational(5,2));
        check(verify_problem_result(scenario,inverse));
        scenario = request;
        scenario.proposal.equations.push_back({5,3,scenario.proposal.equations[0].scope,
            scenario.proposal.equations[0].guard,{PremiseBasis::Formal,"opposing_mass_equation",{}}});
        changed = result;
        changed.request = scenario;
        changed.work = scenario.limits.work;
        check(!verify_problem_result(scenario,changed));
        scenario = scalar_problem();
        const auto identity = execute_problem(scenario);
        check(identity.status == ProblemStatus::Unresolved && !identity.answer);
        check(verify_problem_result(scenario,identity));
        scenario.proposal.equations[0].right = 2;
        const auto impossible = execute_problem(scenario);
        check(impossible.status == ProblemStatus::Conflict);
        check(verify_problem_result(scenario,impossible));
        scenario = scalar_problem();
        scenario.proposal.nodes[3] = ProblemBinaryNode{Code::Add,0,2};
        const auto affine = execute_problem(scenario);
        check(affine.status == ProblemStatus::Checked && affine.answer == Rational(-1));
        check(verify_problem_result(scenario,affine));
        scenario = scalar_problem();
        scenario.proposal.nodes[3] = ProblemBinaryNode{Code::Multiply,0,0};
        scenario.proposal.equations[0].right = 2;
        const auto nonlinear = execute_problem(scenario);
        check(nonlinear.status != ProblemStatus::Checked && nonlinear.issue == ProblemIssue::NonAffine);
        check(verify_problem_result(scenario,nonlinear));
        scenario = scalar_problem();
        scenario.proposal.nodes[3] = ProblemBinaryNode{Code::Divide,2,1};
        scenario.proposal.equations[0].left = 0;
        scenario.proposal.equations[0].right = 3;
        const auto undefined = execute_problem(scenario);
        check(undefined.status != ProblemStatus::Checked && undefined.issue == ProblemIssue::Undefined);
        check(verify_problem_result(scenario,undefined));
        for (int density = 1; density <= 12; ++density) {
            for (int volume = 1; volume <= 9; ++volume) {
                for (int tare = 0; tare <= 3; ++tare) {
                    scenario = request;
                    numbers(scenario,std::to_string(density),std::to_string(volume),std::to_string(tare));
                    const auto generated = execute_problem(scenario);
                    Rational expected = Rational(density * volume,1000) + tare;
                    expected.canonicalize();
                    if (generated.status != ProblemStatus::Checked || generated.answer != expected) {
                        std::cerr << "quantity oracle density=" << density << " volume=" << volume
                                  << " tare=" << tare << " expected=" << expected
                                  << " actual=" << (generated.answer ? generated.answer->get_str() : "none") << '\n';
                    }
                    check(generated.status == ProblemStatus::Checked && generated.answer == expected);
                    check(verify_problem_result(scenario,generated));
                }
            }
        }
        scenario = request;
        scenario.proposal.goal = ProblemAtomGoal{problem_fixture::atom(1,{0,2})};
        const auto logical = execute_problem(scenario);
        check(logical.status == ProblemStatus::Checked && !logical.answer);
        check(verify_problem_result(scenario,logical));
        scenario.proposal.goal = ProblemAtomGoal{problem_fixture::atom(1,{0,2},true)};
        const auto negative = execute_problem(scenario);
        check(negative.status == ProblemStatus::Checked
            && negative.guards[*negative.logical_goal_guard].certificate.proof.status == Status::Disproved);
        check(verify_problem_result(scenario,negative));

        // Given a later review rejecting a previously checked interpretation,
        // recompute the corrected snapshot rather than retaining its old answer.
        auto mistaken = request;
        mistaken.proposal.nodes[4] = ProblemBinaryNode{Code::Subtract,2,3};
        ProblemCorrectionSession correction(mistaken,2);
        check(correction.attempts().size() == 1);
        check(correction.attempts()[0].result.answer == Rational(13,5));
        const auto old_work = correction.work();
        const auto rejected_step = correction.attempts()[0].result.goal_step;
        rejects([&] { correction.correct(request.proposal,""); });
        rejects([&] { correction.correct(mistaken.proposal,"No change"); });
        rejects([&] { correction.correct(request.proposal,"Invalid step",no_step); });
        auto different_goal = request.proposal;
        different_goal.goal = ProblemQuantityGoal{0,3};
        rejects([&] { correction.correct(different_goal,"Changed task"); });
        check(correction.attempts().size() == 1 && correction.work() == old_work);
        // When the reviewer supplies a corrected relation, both attempts remain
        // in the journal, but only the new snapshot supplies the current answer.
        correction.correct(request.proposal,"Tare mass was subtracted instead of added",rejected_step);
        const auto& attempts = correction.attempts();
        check(attempts.size() == 2);
        check(attempts[0].result.answer == Rational(13,5));
        check(attempts[1].result.answer == Rational(17,5));
        check(attempts[1].correction_reason == "Tare mass was subtracted instead of added");
        check(attempts[1].rejected_previous_step == rejected_step);
        check(correction.work() == old_work + attempts[1].result.work);
        check(attempts[1].result.request.context == request.context);
        check(verify_problem_result(attempts[1].result.request,attempts[1].result));
        check(!verify_problem_result(attempts[1].result.request,attempts[0].result));
        rejects([&] { correction.correct(mistaken.proposal,"Third attempt exceeds cap"); });
        rejects([&] { ProblemCorrectionSession invalid(request,0); });
        rejects([&] { ProblemCorrectionSession invalid(request,9); });

        auto bounded_correction = mistaken;
        bounded_correction.limits.work = old_work + 1;
        ProblemCorrectionSession bounded_session(bounded_correction);
        bounded_session.correct(request.proposal,"Retry shares the work budget");
        check(bounded_session.work() == bounded_correction.limits.work);
        check(bounded_session.attempts().back().result.status == ProblemStatus::Limited);
        check(!bounded_session.attempts().back().result.answer);
        rejects([&] { bounded_session.correct(mistaken.proposal,"Budget cannot reset"); });

        std::cout << "PROBLEM_INTEGRATION_TEST_OK checks=" << checks
                  << " independent_quantity_oracles=432\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
