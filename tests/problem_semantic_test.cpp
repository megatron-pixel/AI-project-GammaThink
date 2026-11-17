#include "problem_fixture.hpp"
#include "problem_semantic.hpp"

#include <json-c/json.h>

#include <iostream>
#include <memory>
#include <stdexcept>

namespace {
std::size_t checks = 0;
void check(bool condition) {
    ++checks;
    if (!condition) throw std::runtime_error("Problem semantic assertion " + std::to_string(checks));
}
template<class Action> void rejects(Action action) {
    ++checks;
    try { action(); } catch (const std::exception&) { return; }
    throw std::runtime_error("Problem semantic rejection " + std::to_string(checks));
}
std::string mutate(const std::string& json, auto action) {
    const auto root = std::unique_ptr<json_object,decltype(&json_object_put)>(
        json_tokener_parse(json.c_str()),json_object_put);
    if (!root) throw std::runtime_error("Invalid test mutation fixture");
    action(root.get());
    return json_object_to_json_string_ext(root.get(),JSON_C_TO_STRING_PLAIN);
}
json_object* item(json_object* root, const char* key, std::size_t index) {
    return json_object_array_get_idx(json_object_object_get(root,key),index);
}
}
int main() {
    try {
        using namespace axiom;
        auto request = problem_fixture::request();
        rejects([&] { (void)serialize_problem_proposal(request); });
        auto& question = request.context.views[2];
        question.text += " For this problem, use total mass = density * volume + tare mass. "
            "If a chamber contains a fluid in a run and is calibrated in that run, it is eligible in that run.";
        question.digest = evidence_digest(question.text);
        request.proposal.equations[0].origin = problem_fixture::origin(request,2,"given_mass_balance");
        request.proposal.logic_origins[2] = problem_fixture::origin(request,2,"given_eligibility_rule");
        const auto json = serialize_problem_proposal(request);
        check(parse_problem_context(serialize_problem_context(request.context)) == request.context);
        const auto parsed = parse_problem_proposal(json,request.context,request.limits);
        const auto quoted = serialize_problem_proposal(parsed,ProblemSelectorMode::QuoteV1);
        const auto from_quotes = parse_problem_proposal(quoted,request.context,request.limits,
                                                       ProblemSelectorMode::QuoteV1);
        check(serialize_problem_proposal(from_quotes) == json);
        check(serialize_problem_proposal(from_quotes,ProblemSelectorMode::QuoteV1) == quoted);
        rejects([&] { (void)parse_problem_proposal(quoted,request.context); });
        rejects([&] { (void)parse_problem_proposal(json,request.context,{},ProblemSelectorMode::QuoteV1); });
        check(parsed.context == request.context && parsed.limits == request.limits);
        check(serialize_problem_proposal(parsed) == json);
        const auto result = execute_problem(parsed);
        check(result.status == ProblemStatus::Checked && result.answer == Rational(17,5));
        check(result.source_interpretations && result.question_assumptions);
        check(verify_problem_result(parsed,result));
        auto numerical_only = parsed;
        numerical_only.proposal.logic = {};
        numerical_only.proposal.logic_origins.clear();
        numerical_only.proposal.equations[0].guard.reset();
        for (const auto mode : {ProblemSelectorMode::OffsetV1, ProblemSelectorMode::QuoteV1}) {
            const auto numerical_json = serialize_problem_proposal(numerical_only,mode);
            const auto numerical = parse_problem_proposal(numerical_json,numerical_only.context,{},mode);
            check(numerical.proposal.logic.name.empty() && numerical.proposal.logic.constants.empty()
                && numerical.proposal.logic.predicates.empty() && numerical.proposal.logic.rules.empty());
            const auto numerical_result = execute_problem(numerical);
            check(numerical_result.status == ProblemStatus::Checked
                && numerical_result.answer == Rational(17,5));
            check(verify_problem_result(numerical,numerical_result));
            const auto missing_theory = mutate(numerical_json,[](auto root) {
                json_object_object_add(root,"goal",json_tokener_parse("[\"atom\",[0,false,[[\"c\",0]]]]"));
            });
            rejects([&] { (void)parse_problem_proposal(missing_theory,numerical.context,{},mode); });
        }
        const auto input = problem_semantic_input(request.context);
        check(input.find("1.2e3") != std::string::npos && input.find("2.5") != std::string::npos);
        check(input.find("17/5") == std::string::npos && json.find("17/5") == std::string::npos);
        for (const auto key : {"answer","context","units","limits","steps","formal","proof","work"}) {
            const auto invalid = mutate(json,[&](auto root) {
                json_object_object_add(root,key,json_object_new_int(1));
            });
            rejects([&] { (void)parse_problem_proposal(invalid,request.context); });
        }
        for (const auto invalid : {"","[]","null","{}","{} {}",R"({"status":"clarify"})",
                R"({"status":"execute","reason":"x"})",R"({"status":"unsupported","reason":1})"}) {
            rejects([&] { (void)parse_problem_proposal(invalid,request.context); });
        }
        const auto bad = [&](auto action) {
            const auto invalid = mutate(json,action);
            rejects([&] { (void)parse_problem_proposal(invalid,request.context); });
        };
        const auto bad_quote = [&](auto action) {
            const auto invalid = mutate(quoted,action);
            rejects([&] { (void)parse_problem_proposal(invalid,request.context,{},ProblemSelectorMode::QuoteV1); });
        };
        bad_quote([](auto root) {
            json_object_array_put_idx(item(root,"numbers",1),4,json_tokener_parse("[1,null,\"exactly\",null]"));
        });
        bad_quote([](auto root) {
            json_object_array_put_idx(item(root,"numbers",1),4,
                json_tokener_parse("[1,null,\"exactly\",\"exactly\"]"));
        });
        bad_quote([](auto root) {
            json_object_array_put_idx(item(root,"numbers",0),0,json_tokener_parse("[0,null,\"absent\",null]"));
        });
        bad_quote([](auto root) {
            json_object_array_put_idx(item(root,"numbers",0),0,json_tokener_parse("[99,null,\"1.2e3\",null]"));
        });
        bad_quote([](auto root) {
            json_object_array_put_idx(item(root,"numbers",0),0,json_tokener_parse("[0,99,\"1.2e3\",null]"));
        });
        bad_quote([](auto root) {
            json_object_array_put_idx(item(root,"numbers",0),0,json_tokener_parse("[0,null,\"1.2e3\",\"\"]"));
        });
        auto overlapping = request.context;
        overlapping.views.push_back(make_evidence_view("overlapping",PremiseBasis::Source,
            "aaa","synthetic","overlapping","literal-v1"));
        const auto overlap_json = mutate(quoted,[](auto root) {
            json_object_array_put_idx(item(root,"bindings",0),2,json_tokener_parse("[[3,null,\"aa\",null]]"));
        });
        bool selector_error = false;
        try { (void)parse_problem_proposal(overlap_json,overlapping,{},ProblemSelectorMode::QuoteV1); }
        catch (const ProblemSelectorError&) { selector_error = true; }
        check(selector_error);
        auto budget = request.context;
        budget.views.push_back(make_evidence_view("bounded_search",PremiseBasis::Source,
            std::string(64000,'a')+"b","synthetic","bounded_search","literal-v1"));
        const auto budget_json = mutate(quoted,[](auto root) {
            auto selector = json_object_new_array();
            json_object_array_add(selector,json_object_new_int(3));
            json_object_array_add(selector,nullptr);
            json_object_array_add(selector,json_object_new_string((std::string(4095,'a')+"b").c_str()));
            json_object_array_add(selector,nullptr);
            auto supports = json_object_new_array();
            json_object_array_add(supports,selector);
            json_object_array_put_idx(item(root,"bindings",0),2,supports);
        });
        rejects([&] { (void)parse_problem_proposal(budget_json,budget,{},ProblemSelectorMode::QuoteV1); });
        auto unicode = parsed;
        const std::string prefix = "\xce\xbc \"marker\" \\\n";
        unicode.context.views[0].text = prefix+unicode.context.views[0].text;
        unicode.context.views[0].digest = evidence_digest(unicode.context.views[0].text);
        const auto shift = [&](EvidenceSpan& value) { if (value.view == 0) value.offset += prefix.size(); };
        for (auto& numeric : unicode.proposal.numbers) {
            shift(numeric.numeral);
            if (numeric.unit_span) shift(*numeric.unit_span);
            if (numeric.qualifier) shift(*numeric.qualifier);
        }
        for (auto& binding : unicode.proposal.bindings) for (auto& support : binding.origin.supports) shift(support);
        for (auto& origin : unicode.proposal.logic_origins) for (auto& support : origin.supports) shift(support);
        unicode.proposal.bindings[0].origin.supports = {{0,0,
            static_cast<std::uint32_t>(unicode.context.views[0].text.size()),no_step}};
        const auto unicode_json = serialize_problem_proposal(unicode,ProblemSelectorMode::QuoteV1);
        check(serialize_problem_proposal(parse_problem_proposal(unicode_json,unicode.context,{},
            ProblemSelectorMode::QuoteV1)) == serialize_problem_proposal(unicode));
        auto table = parsed;
        auto& table_view = table.context.views[0];
        table_view.locators = {
            {LocatorRole::TableHeader,static_cast<std::uint32_t>(table_view.text.find("R")),1,"table","","run",{}},
            {LocatorRole::TableCell,0,static_cast<std::uint32_t>(table_view.text.size()),"table","R","density",{0}}
        };
        const auto locate = [](EvidenceSpan& value) { if (value.view == 0) value.locator = 1; };
        for (auto& numeric : table.proposal.numbers) {
            locate(numeric.numeral);
            if (numeric.unit_span) locate(*numeric.unit_span);
            if (numeric.qualifier) locate(*numeric.qualifier);
        }
        for (auto& binding : table.proposal.bindings) for (auto& support : binding.origin.supports) locate(support);
        for (auto& origin : table.proposal.logic_origins) for (auto& support : origin.supports) locate(support);
        const auto table_json = serialize_problem_proposal(table,ProblemSelectorMode::QuoteV1);
        check(serialize_problem_proposal(parse_problem_proposal(table_json,table.context,{},
            ProblemSelectorMode::QuoteV1)) == serialize_problem_proposal(table));
        const auto wrong_cell = mutate(table_json,[](auto root) {
            auto selector = json_object_array_get_idx(item(root,"numbers",0),0);
            json_object_array_put_idx(selector,1,json_object_new_int(0));
        });
        rejects([&] { (void)parse_problem_proposal(wrong_cell,table.context,{},ProblemSelectorMode::QuoteV1); });
        bad([](auto root) { json_object_array_put_idx(item(root,"quantities",0),3,json_object_new_int(999)); });
        bad([](auto root) { json_object_array_put_idx(item(root,"quantities",0),1,json_object_new_int(-1)); });
        bad([](auto root) { json_object_array_put_idx(item(root,"numbers",0),3,json_object_new_string("formal")); });
        bad([](auto root) { json_object_array_put_idx(item(root,"numbers",0),0,json_object_new_int(1200)); });
        bad([](auto root) {
            auto span = json_object_array_get_idx(item(root,"numbers",0),0);
            json_object_array_put_idx(span,1,json_object_new_int(99999));
        });
        bad([](auto root) { json_object_array_put_idx(item(root,"bindings",0),2,json_object_new_array()); });
        bad([](auto root) { json_object_array_put_idx(item(root,"nodes",2),1,json_object_new_int(200)); });
        bad([](auto root) { json_object_array_put_idx(item(root,"nodes",2),0,json_object_new_string("answer")); });
        bad([](auto root) {
            auto terms = json_object_array_get_idx(json_object_array_get_idx(item(root,"rules",0),1),2);
            json_object_array_put_idx(terms,0,json_tokener_parse("[\"v\",0]"));
        });
        auto formal = request.context;
        formal.views[0].basis = PremiseBasis::Formal;
        rejects([&] { (void)parse_problem_proposal(json,formal); });
        rejects([&] { (void)problem_semantic_input(formal); });
        for (const auto& [status,kind] : {std::pair{"clarify",ProblemDeclineKind::Clarify},
                                        std::pair{"unsupported",ProblemDeclineKind::Unsupported}}) {
            bool declined = false;
            try {
                (void)parse_problem_proposal(std::string("{\"status\":\"")+status+"\",\"reason\":\"x\"}",
                                             request.context);
            } catch (const ProblemDecline& error) { declined = error.kind == kind; }
            check(declined);
        }
        auto approximate = parsed;
        approximate.proposal.numbers[0].precision = QuantityPrecision::Approximate;
        const auto roundtrip = parse_problem_proposal(serialize_problem_proposal(approximate),request.context);
        check(roundtrip.proposal.numbers[0].precision == QuantityPrecision::Approximate);
        const auto unresolved = execute_problem(roundtrip);
        check(unresolved.status == ProblemStatus::Unresolved && !unresolved.answer);
        check(verify_problem_result(roundtrip,unresolved));
        const auto schema = std::unique_ptr<json_object,decltype(&json_object_put)>(
            json_tokener_parse(problem_proposal_schema().c_str()),json_object_put);
        check(schema != nullptr);
        const auto definitions = json_object_object_get(schema.get(),"$defs");
        check(definitions != nullptr);
        const auto scope_schema = json_object_object_get(definitions,"scope");
        check(scope_schema && json_object_get_int(json_object_object_get(scope_schema,"minItems")) == 2);
        check(json_object_get_int(json_object_object_get(scope_schema,"maxItems")) == 2);
        const auto qualifier = json_object_object_get(definitions,"qualifier");
        const auto qualifier_fields = json_object_object_get(qualifier,"prefixItems");
        check(json_object_array_length(qualifier_fields) == 2);
        check(std::string(json_object_get_string(json_object_object_get(
            json_object_array_get_idx(qualifier_fields,1),"$ref"))) == "#/$defs/text");
        const auto quantities = json_object_object_get(definitions,"quantity");
        check(json_object_get_int(json_object_object_get(quantities,"minItems")) == 4);
        check(json_object_get_int(json_object_object_get(quantities,"maxItems")) == 4);
        const auto offset_span = json_object_object_get(definitions,"span");
        check(std::string(json_object_get_string(json_object_object_get(
            item(offset_span,"prefixItems",1),"$ref"))) == "#/$defs/index");
        const auto quote_schema = std::unique_ptr<json_object,decltype(&json_object_put)>(
            json_tokener_parse(problem_proposal_schema(ProblemSelectorMode::QuoteV1).c_str()),json_object_put);
        check(quote_schema != nullptr);
        const auto quote_defs = json_object_object_get(quote_schema.get(),"$defs");
        const auto quote_span = json_object_object_get(quote_defs,"span");
        check(std::string(json_object_get_string(json_object_object_get(
            item(quote_span,"prefixItems",2),"$ref"))) == "#/$defs/quote");
        check(std::string(json_object_get_string(json_object_object_get(
            item(quote_span,"prefixItems",1),"$ref"))) == "#/$defs/nullable_index");
        rejects([&] { (void)problem_proposal_schema(static_cast<ProblemSelectorMode>(99)); });
        std::cout << "PROBLEM_SEMANTIC_TEST_OK checks=" << checks << '\n';
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
