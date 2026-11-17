#include "problem_fixture.hpp"
#include "problem_semantic.hpp"

#include <fstream>
#include <iostream>
#include <iterator>
#include <set>
#include <source_location>
#include <stdexcept>

namespace {
using namespace axiom;
std::size_t checks = 0;
void check(bool value, const std::source_location where = std::source_location::current()) {
    ++checks;
    if (!value) throw std::runtime_error("Registry assertion at line "
        + std::to_string(where.line()));
}
template<class Action> void rejects(Action action) {
    ++checks;
    bool rejected = false;
    try { action(); } catch (const std::runtime_error&) { rejected = true; }
    if (!rejected) throw std::runtime_error("Registry rejection failed");
}
std::uint32_t unit(const ProblemContext& context, std::string_view id) {
    for (std::uint32_t i = 0; i < context.units.size(); ++i)
        if (context.units[i].id == id) return i;
    throw std::runtime_error("Missing candidate unit");
}
struct Conversion {
    const char *from, *to, *numeral, *expected;
};
ProblemRequest binding(const ProblemContext& context, const Conversion& conversion) {
    ProblemRequest request;
    request.context = context;
    const auto from = unit(context,conversion.from), to = unit(context,conversion.to);
    const std::string prefix = "Calibration reads exactly ";
    const std::string numeral = conversion.numeral, symbol = context.units[from].symbol;
    request.context.views = {make_evidence_view("calibration",PremiseBasis::Source,
        prefix + numeral + " " + symbol + ".", "synthetic_registry",
        "independent_calibration", "literal-v1")};
    request.proposal.entities = {"specimen"};
    request.proposal.quantities = {{"calibration",0,{},from}};
    request.proposal.numbers = {{
        {0,static_cast<std::uint32_t>(prefix.size()),
            static_cast<std::uint32_t>(numeral.size()),no_step},
        EvidenceSpan{0,static_cast<std::uint32_t>(prefix.size()+numeral.size()+1),
            static_cast<std::uint32_t>(symbol.size()),no_step},
        from,QuantityPrecision::Exact,problem_fixture::span(request,0,"exactly")
    }};
    request.proposal.bindings = {{0,0,problem_fixture::origin(request,0,"calibration")}};
    request.proposal.goal = ProblemQuantityGoal{0,to};
    return request;
}
struct Product {
    const char *left, *left_value, *right, *right_value, *output, *expected;
};
void product(const ProblemContext& context, const Product& value) {
    auto request = binding(context,{value.left,value.left,value.left_value,value.left_value});
    auto second = binding(context,{value.right,value.right,value.right_value,value.right_value});
    request.context.views.push_back(second.context.views[0]);
    request.context.views.back().id = "second_calibration";
    auto number = second.proposal.numbers[0];
    number.numeral.view = 1;
    number.unit_span->view = 1;
    number.qualifier->view = 1;
    request.proposal.numbers.push_back(number);
    request.proposal.quantities.push_back({"second",0,{},unit(context,value.right)});
    request.proposal.quantities.push_back({"product",0,{},unit(context,value.output)});
    request.proposal.bindings.push_back({1,1,problem_fixture::origin(request,1,"second")});
    request.proposal.nodes = {
        ProblemQuantityNode{0},ProblemQuantityNode{1},
        ProblemBinaryNode{Code::Multiply,0,1},ProblemQuantityNode{2}
    };
    request.proposal.equations = {{3,2,{},std::nullopt,
        {PremiseBasis::Formal,"calibration_product",{}}}};
    request.proposal.goal = ProblemQuantityGoal{2,unit(context,value.output)};
    const auto result = execute_problem(request);
    check(result.status == ProblemStatus::Checked && result.answer == Rational(value.expected));
    check(verify_problem_result(request,result));
}
}

int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::runtime_error("Supply a candidate context path");
        std::ifstream input(argv[1],std::ios::binary);
        if (!input) throw std::runtime_error("Cannot read candidate context");
        const std::string json{std::istreambuf_iterator<char>(input),{}};
        const auto context = parse_problem_context(json);
        const Conversion conversions[] = {
            {"scalar","scalar","7","7"}, {"percent","scalar","37","37/100"},
            {"kg","g","3.125","3125"}, {"g","kg","125","1/8"},
            {"mg","g","875","7/8"}, {"m","cm","2.75","275"},
            {"cm","m","125","5/4"}, {"mm","m","375","3/8"},
            {"km","m","0.625","625"}, {"m2","cm2","1.5","15000"},
            {"cm2","m2","22500","9/4"}, {"m3","L","0.375","375"},
            {"cm3","mL","19","19"}, {"L","m3","125","1/8"},
            {"mL","L","875","7/8"}, {"s","min","75","5/4"},
            {"min","s","2.75","165"}, {"h","min","1.25","75"},
            {"day","h","2.5","60"}, {"week","day","1.5","21/2"},
            {"m_per_s","km_per_h","2.5","9"},
            {"km_per_h","m_per_s","81","45/2"},
            {"L_per_min","L_per_min","17","17"},
            {"mg_per_L","g_per_cm3","2500","1/400"},
            {"g_per_cm3","mg_per_L","0.0125","12500"},
            {"W","W","240","240"}, {"J","J","375","375"},
            {"A","A","0.125","1/8"}, {"Ah","C","0.75","2700"},
            {"C","Ah","900","1/4"}, {"V","V","9","9"},
            {"K","K","278.5","557/2"}
        };
        std::set<std::string> covered;
        for (const auto& conversion : conversions) {
            const auto request = binding(context,conversion);
            const auto result = execute_problem(request);
            check(result.status == ProblemStatus::Checked);
            check(result.answer == Rational(conversion.expected));
            check(result.answer_unit == unit(context,conversion.to));
            check(verify_problem_result(request,result));
            auto forged = result;
            *forged.answer += 1;
            check(!verify_problem_result(request,forged));
            covered.insert(conversion.from);
        }
        check(covered.size() == context.units.size());
        for (const auto& registered : context.units) check(covered.contains(registered.id));
        for (const Product& value : {
            Product{"L_per_min","7","s","150","L","35/2"},
            Product{"mg_per_L","8","mL","375","mg","3"},
            Product{"g_per_cm3","1.6","cm3","25","g","40"},
            Product{"W","240","min","2.5","J","36000"},
            Product{"A","0.125","h","3","Ah","3/8"},
            Product{"V","9","A","0.5","W","9/2"}
        }) product(context,value);

        auto invalid = binding(context,{"g","kg","125","1/8"});
        invalid.proposal.goal = ProblemQuantityGoal{0,unit(context,"m")};
        rejects([&] { (void)admit_problem(invalid); });
        invalid = binding(context,{"g","kg","125","1/8"});
        invalid.proposal.quantities[0].unit = unit(context,"m");
        rejects([&] { (void)admit_problem(invalid); });
        invalid = binding(context,{"g","kg","125","1/8"});
        invalid.proposal.numbers[0].unit_span.reset();
        rejects([&] { (void)admit_problem(invalid); });
        invalid = binding(context,{"g","kg","125","1/8"});
        invalid.proposal.numbers[0].unit = unit(context,"kg");
        rejects([&] { (void)admit_problem(invalid); });
        invalid = binding(context,{"g","kg","125","1/8"});
        invalid.context.units[0].spellings.push_back("kg");
        rejects([&] { (void)admit_problem(invalid); });
        invalid = binding(context,{"g","kg","125","1/8"});
        invalid.context.units[unit(context,"g")].scale = 0;
        rejects([&] { (void)admit_problem(invalid); });
        invalid = binding(context,{"g","kg","125","1/8"});
        invalid.context.units[0].id = invalid.context.units[1].id;
        rejects([&] { (void)admit_problem(invalid); });
        std::cout << "Registry integration: " << checks << " checks passed; "
                  << covered.size() << " units, 6 dimensional products\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
