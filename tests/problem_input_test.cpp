#include "problem_fixture.hpp"

#include <iostream>
#include <stdexcept>

namespace {
std::size_t checks = 0;
void check(bool value) {
    ++checks;
    if (!value) throw std::runtime_error("Problem input assertion " + std::to_string(checks));
}
template<class Action> void rejects(Action action) {
    ++checks;
    bool rejected = false;
    try { action(); } catch (const std::exception&) { rejected = true; }
    if (!rejected) throw std::runtime_error("Problem input rejection " + std::to_string(checks));
}
}
int main() {
    try {
        using namespace axiom;
        check(evidence_digest("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
        for (const auto& [input, expected] : std::vector<std::pair<std::string,Rational>>{
                {"1.2e3",1200}, {"2.5",Rational(5,2)}, {"0.4",Rational(2,5)},
                {"-5.3e4",-53000}, {".5",Rational(1,2)}, {"1.",1},
                {"1.495 978 707 * 10 11",Rational(mpz_class("149597870700"))},
                {"5.15 \xc3\x97 10 18",Rational(mpz_class("5150000000000000000"))},
                {"1.2" "\xc3\x97" "10" "\xc2\xb3",1200},
                {"\xe2\x88\x92" "5.3e4",-53000}}) {
            check(parse_evidence_number(input) == expected);
        }
        mpz_class denominator;
        mpz_ui_pow_ui(denominator.get_mpz_t(),10,42);
        check(parse_evidence_number("6.626 070 15 * 10 -34")
              == checked_problem_rational(Rational(mpz_class(662607015),denominator)));
        for (const auto* input : {"","1e","1e-","1e999999","1e1234","1/2","12 34",
                                 "1.2.3","1e2e3","NaN","1*103","1-2","1.2e3;system","two"}) {
            rejects([&] { (void)parse_evidence_number(input); });
        }
        const auto original = problem_fixture::request();
        const auto admitted = admit_problem(original);
        check(admitted.lexical_numbers == std::vector<Rational>({1200,Rational(5,2),Rational(2,5)}));
        check(admitted.canonical_numbers == std::vector<Rational>({1200,Rational(1,400),Rational(2,5)}));
        check(admitted.node_dimensions[4] == original.context.units[0].dimension);
        check(admitted.admission_work > 0);
        auto altered = original;
        altered.context.views[0].text[0] = 'X';
        rejects([&] { (void)admit_problem(altered); });
        altered = original;
        altered.proposal.numbers[0].numeral.offset = UINT32_MAX;
        rejects([&] { (void)admit_problem(altered); });
        altered = original;
        altered.proposal.numbers[0].numeral.length = 1;
        rejects([&] { (void)admit_problem(altered); });
        altered = original;
        altered.proposal.numbers[0].numeral.length = 3;
        rejects([&] { (void)admit_problem(altered); });
        for (const auto& sign : {std::string("-"),std::string("+"),std::string("\xe2\x88\x92"),
                                std::string("- "),std::string("+ ")}) {
            altered = original;
            auto& view = altered.context.views[0];
            view.text = "In run R, chamber A contains fluid F. Its density is exactly "
                + sign + "1.2e3 kg/m^3.";
            view.digest = evidence_digest(view.text);
            auto& numeric = altered.proposal.numbers[0];
            numeric.numeral = problem_fixture::span(altered,0,"1.2e3");
            numeric.unit_span = problem_fixture::span(altered,0,"kg/m^3");
            numeric.qualifier = problem_fixture::span(altered,0,"exactly");
            altered.proposal.bindings[0].origin = problem_fixture::origin(altered,0,"signed_density");
            altered.proposal.logic_origins[0] = problem_fixture::origin(altered,0,"containment_assertion");
            rejects([&] { (void)admit_problem(altered); });
            numeric.numeral.offset -= sign.size();
            numeric.numeral.length += sign.size();
            check(admit_problem(altered).lexical_numbers[0] == (sign.front() == '+' ? 1200 : -1200));
        }
        altered = original;
        altered.proposal.numbers[0].numeral.offset += 2;
        altered.proposal.numbers[0].numeral.length = 3;
        rejects([&] { (void)admit_problem(altered); });
        altered = original;
        altered.proposal.numbers[1].unit = 5;
        rejects([&] { (void)admit_problem(altered); });
        altered = original;
        altered.context.units[2].dimension = altered.context.units[5].dimension;
        rejects([&] { (void)admit_problem(altered); });
        altered = original;
        altered.context.units[2].scale = 0;
        rejects([&] { (void)admit_problem(altered); });
        altered = original;
        altered.context.units[1].spellings.push_back("kg");
        rejects([&] { (void)admit_problem(altered); });
        altered = original;
        altered.proposal.quantities.push_back(altered.proposal.quantities.back());
        rejects([&] { (void)admit_problem(altered); });
        altered = original;
        altered.proposal.quantities[1].scope.qualifiers[0].second = "S";
        rejects([&] { (void)admit_problem(altered); });
        altered = original;
        std::get<ProblemBinaryNode>(altered.proposal.nodes[2]).left = 2;
        rejects([&] { (void)admit_problem(altered); });
        altered = original;
        altered.proposal.bindings[0].origin.basis = PremiseBasis::Formal;
        rejects([&] { (void)admit_problem(altered); });
        altered = original;
        altered.proposal.equations[0].guard->terms[0].kind = TermKind::Variable;
        rejects([&] { (void)admit_problem(altered); });
        altered = original;
        altered.context.views[0].locators.push_back(
            {LocatorRole::Paragraph,0,static_cast<std::uint32_t>(altered.context.views[0].text.size()),{},{},{},{}});
        rejects([&] { (void)admit_problem(altered); });
        altered = original;
        altered.context.views[0].text = "\xce\xbc" + altered.context.views[0].text;
        altered.context.views[0].digest = evidence_digest(altered.context.views[0].text);
        altered.proposal.numbers[0].numeral = {0,1,1,no_step};
        rejects([&] { (void)admit_problem(altered); });
        altered = original;
        for (auto& quantity : altered.proposal.quantities) quantity.scope.resolved = false;
        altered.proposal.equations[0].scope.resolved = false;
        check(admit_problem(altered).canonical_numbers == admitted.canonical_numbers);
        altered = original;
        altered.limits.work = 1;
        check(admit_problem(altered).admission_work > altered.limits.work);
        std::cout << "PROBLEM_INPUT_TEST_OK checks=" << checks << '\n';
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
