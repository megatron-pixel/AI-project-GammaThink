#include "problem_fixture.hpp"
#include "problem_semantic.hpp"
#include "archive_dsl/codec.hpp"

#include <json-c/json.h>
#include <algorithm>
#include <iostream>
#include <source_location>

namespace {
using namespace axiom;
using Json = std::unique_ptr<json_object, decltype(&json_object_put)>;
std::size_t checks = 0;
void check(bool value, const std::source_location where = std::source_location::current()) {
    ++checks;
    if (!value) throw std::runtime_error("Quantified theory assertion at " + std::to_string(where.line()));
}
template<class F> void rejects(F operation) {
    ++checks;
    try { operation(); }
    catch (const std::runtime_error&) { return; }
    throw std::runtime_error("Expected quantified theory rejection");
}
Json document(const std::string& text) {
    Json value(json_tokener_parse(text.c_str()),json_object_put);
    if (!value) throw std::runtime_error("Invalid synthetic JSON fixture");
    return value;
}
std::string wire(json_object* value) {
    return json_object_to_json_string_ext(value,JSON_C_TO_STRING_PLAIN);
}
ProblemRequest fixture() {
    ProblemRequest result;
    result.context.unit_registry_id = "synthetic";
    result.context.unit_registry_version = "1";
    result.context.units = {{"one","1",{},1,{"1"}}};
    result.context.views = {make_evidence_view("law",PremiseBasis::Source,
        "For every eligible object, output equals 2 times input. A is eligible. A input is 3. B input is 7.",
        "synthetic","quantified-law","literal-v1")};
    auto& p = result.proposal;
    p.entities = {"A","B"};
    p.quantities = {{"input",0,{},0},{"output",0,{},0},{"input",1,{},0},{"output",1,{},0}};
    for (const auto* numeral : {"2","3","7"})
        p.numbers.push_back({problem_fixture::span(result,0,numeral),std::nullopt,0,
            QuantityPrecision::Exact,std::nullopt});
    p.bindings = {{0,1,problem_fixture::origin(result,0,"A-input")},
        {2,2,problem_fixture::origin(result,0,"B-input")}};
    p.logic.name = "eligibility";
    p.logic.version = "1";
    p.logic.constants = p.entities;
    p.logic.predicates = {{"eligible",1}};
    p.logic.rules = {{0,problem_fixture::atom(0,{0}),{},"eligible_A"}};
    p.logic_origins = {problem_fixture::origin(result,0,"eligible_A")};
    p.goal = ProblemQuantityGoal{1,0};
    return result;
}
Json theory(const ProblemRequest& request, ProblemSelectorMode mode) {
    auto result = document(serialize_problem_proposal(request,mode));
    json_object_object_del(result.get(),"goal");
    auto schema = document("{\"value\":[1,"
        "[[\"input\",[\"v\",0],[true,[]],0],[\"output\",[\"v\",0],[true,[]],0]],"
        "[[\"q\",1],[\"q\",0],[\"n\",0],[\"mul\",1,2]],0,3,[true,[]],"
        "[0,false,[[\"v\",0]]],[]]}");
    auto* declaration = json_object_object_get(schema.get(),"value");
    auto* supports = json_object_array_get_idx(declaration,7);
    auto* support = json_object_new_array();
    json_object_array_add(support,json_object_new_int(0));
    if (mode == ProblemSelectorMode::OffsetV1) {
        json_object_array_add(support,json_object_new_int(0));
        json_object_array_add(support,json_object_new_uint64(request.context.views[0].text.size()));
    } else {
        json_object_array_add(support,nullptr);
        json_object_array_add(support,json_object_new_string(request.context.views[0].text.c_str()));
    }
    json_object_array_add(support,nullptr);
    json_object_array_add(supports,support);
    auto* declarations = json_object_new_array();
    json_object_array_add(declarations,json_object_get(declaration));
    json_object_object_add(result.get(),"quantified_equations",declarations);
    return result;
}
std::string archive_roundtrip(json_object* value) {
    auto profile = document("{\"goals\":[]}");
    json_object_object_add(profile.get(),"theory",json_object_get(value));
    return gammathink::archive_dsl::decode_v2(gammathink::archive_dsl::encode_v2(wire(profile.get())));
}
}

int main() {
    try {
        const auto original = fixture();
        std::optional<QuantifiedEquation> first_schema;
        for (const auto mode : {ProblemSelectorMode::OffsetV1,ProblemSelectorMode::QuoteV1}) {
            auto input = theory(original,mode);
            const auto decoded = document(archive_roundtrip(input.get()));
            const auto parsed = parse_quantified_problem_theory_proposal(
                wire(json_object_object_get(decoded.get(),"theory")),original.context,{},mode);
            check(parsed.schemas.size() == 1);
            check(parsed.base.equations.empty() && parsed.base.nodes.empty());
            check(parsed.base.quantities == original.proposal.quantities);
            check(parsed.schemas[0].origin.basis == PremiseBasis::Source);
            check(parsed.schemas[0].origin.supports == problem_fixture::origin(original,0,"law").supports);
            if (first_schema) check(*first_schema == parsed.schemas[0]);
            else first_schema = parsed.schemas[0];
            auto expanded = instantiate_equations({original.context,parsed.base,{}},parsed.schemas);
            check(expanded.instances.size() == 2);
            expanded.request.proposal.goal = original.proposal.goal; // Host question, never source knowledge.
            const auto result = execute_problem(expanded.request);
            check(result.answer == Rational(6) && result.status == ProblemStatus::Checked);
            // The unchanged core counts the host's unit registry as formal.
            check(result.source_interpretations && result.formal_assumptions);
            check(std::all_of(expanded.request.proposal.equations.begin(),expanded.request.proposal.equations.end(),
                [](const auto& equation) { return equation.origin.basis == PremiseBasis::Source; }));
            check(std::all_of(parsed.base.logic_origins.begin(),parsed.base.logic_origins.end(),
                [](const auto& origin) { return origin.basis == PremiseBasis::Source; }));
            check(verify_problem_result(expanded.request,result));
            expanded.request.proposal.goal = ProblemQuantityGoal{3,0};
            const auto absent = execute_problem(expanded.request);
            check(absent.status == ProblemStatus::Unresolved && !absent.answer);
            check(verify_problem_result(expanded.request,absent));
            rejects([&] { (void)parse_problem_theory_proposal(wire(input.get()),original.context,{},mode); });
            auto* declarations = json_object_object_get(input.get(),"quantified_equations");
            auto* declaration = json_object_array_get_idx(declarations,0);
            json_object_array_add(declarations,json_object_get(declaration));
            const auto repeated = parse_quantified_problem_theory_proposal(wire(input.get()),original.context,{},mode);
            check(repeated.schemas.size() == 2);
            check(instantiate_equations({original.context,repeated.base,{}},repeated.schemas).instances.size() == 4);
        }
        auto invalid = theory(original,ProblemSelectorMode::OffsetV1);
        auto* declarations = json_object_object_get(invalid.get(),"quantified_equations");
        auto* declaration = json_object_array_get_idx(declarations,0);
        json_object_array_put_idx(declaration,7,json_object_new_array());
        rejects([&] { (void)parse_quantified_problem_theory_proposal(wire(invalid.get()),original.context); });
        invalid = theory(original,ProblemSelectorMode::OffsetV1);
        json_object_object_add(invalid.get(),"goal",json_tokener_parse("[\"q\",1,0]"));
        rejects([&] { (void)parse_quantified_problem_theory_proposal(wire(invalid.get()),original.context); });
        invalid = theory(original,ProblemSelectorMode::OffsetV1);
        declarations = json_object_object_get(invalid.get(),"quantified_equations");
        declaration = json_object_array_get_idx(declarations,0);
        auto* quantity = json_object_array_get_idx(json_object_array_get_idx(declaration,1),0);
        json_object_array_put_idx(quantity,1,json_tokener_parse("[\"v\",9]"));
        const auto unbound = parse_quantified_problem_theory_proposal(wire(invalid.get()),original.context);
        rejects([&] { (void)instantiate_equations({original.context,unbound.base,{}},unbound.schemas); });
        invalid = theory(original,ProblemSelectorMode::QuoteV1);
        declarations = json_object_object_get(invalid.get(),"quantified_equations");
        declaration = json_object_array_get_idx(declarations,0);
        auto* support = json_object_array_get_idx(json_object_array_get_idx(declaration,7),0);
        json_object_array_put_idx(support,2,json_object_new_string("not in the supplied source"));
        rejects([&] {
            (void)parse_quantified_problem_theory_proposal(wire(invalid.get()),original.context,{},
                ProblemSelectorMode::QuoteV1);
        });
        std::cout << "Quantified shared-theory checks passed: " << checks << '\n';
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
