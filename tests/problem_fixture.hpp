#pragma once

#include "problem.hpp"

#include <stdexcept>

namespace problem_fixture {
inline axiom::EvidenceSpan span(const axiom::ProblemRequest& request, std::uint32_t view,
                                const std::string& selected, unsigned occurrence = 0) {
    const auto& text = request.context.views.at(view).text;
    std::size_t position = 0;
    for (unsigned i = 0; i <= occurrence; ++i) {
        position = text.find(selected, position);
        if (position == std::string::npos) throw std::runtime_error("Invalid problem fixture span");
        if (i != occurrence) position += selected.size();
    }
    return {view, static_cast<std::uint32_t>(position),
            static_cast<std::uint32_t>(selected.size()), axiom::no_step};
}
inline axiom::ProblemOrigin origin(const axiom::ProblemRequest& request, std::uint32_t view,
                                   const std::string& label) {
    return {request.context.views.at(view).basis, label,
        {{view, 0, static_cast<std::uint32_t>(request.context.views.at(view).text.size()), axiom::no_step}}};
}
inline axiom::Atom atom(std::uint32_t predicate, std::initializer_list<std::uint32_t> constants,
                       bool negative = false) {
    axiom::Atom result;
    result.predicate = predicate;
    result.negative = negative;
    for (const auto constant : constants) result.terms.push_back({axiom::TermKind::Constant, constant});
    return result;
}
inline axiom::ProblemRequest request() {
    using namespace axiom;
    ProblemRequest result;
    auto& context = result.context;
    auto& proposal = result.proposal;
    context.unit_registry_id = "synthetic_si";
    context.unit_registry_version = "1";
    context.units = {
        {"kilogram", "kg", {1,0,0,0,0,0,0}, 1, {"kg"}},
        {"cubic_metre", "m^3", {0,3,0,0,0,0,0}, 1, {"m^3"}},
        {"litre", "L", {0,3,0,0,0,0,0}, Rational(1,1000), {"L"}},
        {"density", "kg/m^3", {1,-3,0,0,0,0,0}, 1, {"kg/m^3"}},
        {"one", "1", {}, 1, {"1"}},
        {"metre", "m", {0,1,0,0,0,0,0}, 1, {"m"}},
        {"gram", "g", {1,0,0,0,0,0,0}, Rational(1,1000), {"g"}}
    };
    context.views = {
        make_evidence_view("source_a", PremiseBasis::Source,
            "In run R, chamber A contains fluid F. Its density is exactly 1.2e3 kg/m^3.",
            "synthetic_archive_a", "density", "synthetic-v1"),
        make_evidence_view("source_b", PremiseBasis::Source,
            "In run R, chamber A is calibrated. It contains exactly 2.5 L and has tare mass exactly 0.4 kg.",
            "synthetic_archive_b", "volume_and_tare", "synthetic-v1"),
        make_evidence_view("question", PremiseBasis::Question,
            "Find the total mass of chamber A in run R in kg.",
            "", "question", "literal-v1")
    };
    proposal.entities = {"A", "F", "R"};
    const ProblemScope scope{true, {{"run", "R"}}};
    proposal.quantities = {
        {"density", 1, scope, 3}, {"volume", 0, scope, 1},
        {"tare_mass", 0, scope, 0}, {"total_mass", 0, scope, 0}
    };
    proposal.numbers = {
        {span(result,0,"1.2e3"), span(result,0,"kg/m^3"), 3, QuantityPrecision::Exact, span(result,0,"exactly")},
        {span(result,1,"2.5"), span(result,1,"L"), 2, QuantityPrecision::Exact, span(result,1,"exactly")},
        {span(result,1,"0.4"), span(result,1,"kg"), 0, QuantityPrecision::Exact, span(result,1,"exactly",1)}
    };
    proposal.bindings = {
        {0,0,origin(result,0,"density_assertion")},
        {1,1,origin(result,1,"volume_assertion")},
        {2,2,origin(result,1,"tare_assertion")}
    };
    proposal.nodes = {
        ProblemQuantityNode{0}, ProblemQuantityNode{1},
        ProblemBinaryNode{Code::Multiply,0,1}, ProblemQuantityNode{2},
        ProblemBinaryNode{Code::Add,2,3}, ProblemQuantityNode{3}
    };
    proposal.equations = {{5,4,scope,atom(2,{0,2}),
                           {PremiseBasis::Formal,"given_mass_balance",{}}}};
    proposal.goal = ProblemQuantityGoal{3,0};
    proposal.logic.name = "eligibility";
    proposal.logic.version = "1";
    proposal.logic.constants = proposal.entities;
    proposal.logic.predicates = {{"contains",3},{"calibrated",2},{"eligible",2}};
    Atom rule_head{2,false,{{TermKind::Variable,0},{TermKind::Variable,2}}};
    Atom contains{0,false,{{TermKind::Variable,0},{TermKind::Variable,1},{TermKind::Variable,2}}};
    Atom calibrated{1,false,{{TermKind::Variable,0},{TermKind::Variable,2}}};
    proposal.logic.rules = {
        {0,atom(0,{0,1,2}),{},"source_a"},
        {0,atom(1,{0,2}),{},"source_b"},
        {3,rule_head,{contains,calibrated},"given_eligibility"}
    };
    proposal.logic_origins = {
        origin(result,0,"containment_assertion"), origin(result,1,"calibration_assertion"),
        {PremiseBasis::Formal,"given_eligibility_rule",{}}
    };
    return result;
}
} // namespace problem_fixture
