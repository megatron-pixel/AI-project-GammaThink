#include "problem_sources.hpp"
#include "problem_source_semantic.hpp"
#include "problem_semantic.hpp"

#include <json-c/json.h>

#include <iostream>
#include <memory>
#include <stdexcept>

namespace {
std::size_t checks = 0;
void check(bool value) {
    ++checks;
    if (!value) throw std::runtime_error("Source catalog assertion " + std::to_string(checks));
}
template<class Action> void rejects(Action action) {
    ++checks;
    try { action(); } catch (const std::exception&) { return; }
    throw std::runtime_error("Source catalog rejection " + std::to_string(checks));
}
axiom::ProblemContext context(std::string text) {
    axiom::ProblemContext result;
    result.unit_registry_id = "source-test";
    result.unit_registry_version = "1";
    result.units.push_back({"dimensionless", "1", {}, 1, {"1"}});
    result.views.push_back(axiom::make_evidence_view(
        "question", axiom::PremiseBasis::Question, std::move(text), "", "", "literal-v1"));
    return result;
}
std::string selected(const axiom::ProblemSourceCatalog& catalog, const axiom::EvidenceSpan& span) {
    return catalog.context().views[span.view].text.substr(span.offset, span.length);
}
std::string handle_at(const axiom::ProblemSourceCatalog& catalog, std::uint32_t offset,
                      std::uint32_t locator = axiom::no_step) {
    for (std::size_t i = 0; i < catalog.entries().size(); ++i) {
        const auto& entry = catalog.entries()[i];
        if (!entry.whole_region && entry.span.offset == offset && entry.span.locator == locator)
            return axiom::ProblemSourceCatalog::source_handle(i);
    }
    throw std::runtime_error("Missing test source handle");
}
std::string mutate(const std::string& json, auto action) {
    const auto object = std::unique_ptr<json_object, decltype(&json_object_put)>(
        json_tokener_parse(json.c_str()), json_object_put);
    if (!object) throw std::runtime_error("Invalid source test fixture");
    action(object.get());
    return json_object_to_json_string_ext(object.get(), JSON_C_TO_STRING_PLAIN);
}
}

int main() {
    try {
        using namespace axiom;
        auto original = context("Ada: -1.2e-3 kg; Ada: 4 kg.\n");
        const ProblemSourceCatalog catalog(original);
        original.views[0].text = "caller mutation";
        check(catalog.context().views[0].text != original.views[0].text);
        check(catalog.snapshot() == ProblemSourceCatalog(catalog.context()).snapshot());
        check(catalog.input() == ProblemSourceCatalog(catalog.context()).input());
        check(catalog.unit(catalog.snapshot(), "u0") == 0);
        check(catalog.input().find("\"unit_refs\":[[\"u0\",0]]") != std::string::npos);
        check(selected(catalog, catalog.resolve(catalog.snapshot(), "s0"))
            == "Ada: -1.2e-3 kg; Ada: 4 kg.\n");
        const auto first_ada = handle_at(catalog, 0), second_ada = handle_at(catalog, 17);
        check(catalog.resolve(catalog.snapshot(), first_ada).offset
            != catalog.resolve(catalog.snapshot(), second_ada).offset);
        check(selected(catalog, catalog.resolve(catalog.snapshot(), first_ada)) == "Ada");
        check(selected(catalog, catalog.resolve(catalog.snapshot(), second_ada)) == "Ada");
        check(selected(catalog, catalog.resolve(catalog.snapshot(), handle_at(catalog, 5), handle_at(catalog, 11)))
            == "-1.2e-3");
        std::string reconstructed;
        for (const auto& entry : catalog.entries()) {
            if (!entry.whole_region) reconstructed += selected(catalog, entry.span);
        }
        check(reconstructed == catalog.context().views[0].text);
        for (const auto bad : {"", "s", "u0", "s00", "s-1", "s+1", "s1x", "s4294967296", "s99999"}) {
            rejects([&] { (void)catalog.resolve(catalog.snapshot(), bad); });
        }
        for (const auto bad : {"s0", "u00", "u-1", "u1"}) {
            rejects([&] { (void)catalog.unit(catalog.snapshot(), bad); });
        }
        rejects([&] { (void)catalog.resolve("stale", first_ada); });
        rejects([&] { (void)catalog.unit("stale", "u0"); });
        rejects([&] { (void)catalog.resolve(catalog.snapshot(), second_ada, first_ada); });
        rejects([&] { (void)catalog.resolve(catalog.snapshot(), "s0", first_ada); });
        auto changed = catalog.context();
        changed.views[0].entry_path = "different-provenance";
        check(ProblemSourceCatalog(changed).snapshot() != catalog.snapshot());
        changed = catalog.context();
        changed.units[0].scale = 2;
        check(ProblemSourceCatalog(changed).snapshot() != catalog.snapshot());
        check(ProblemSourceCatalog(catalog.context(), {999999,256}).snapshot() != catalog.snapshot());
        rejects([&] { (void)ProblemSourceCatalog(catalog.context(), {0,256}); });

        auto table_context = context("Count\nAda 4\nAda 4");
        auto& locators = table_context.views[0].locators;
        locators.push_back({LocatorRole::TableHeader, 0, 5, "t", "", "count", {}});
        locators.push_back({LocatorRole::TableCell, 6, 5, "t", "one", "count", {0}});
        locators.push_back({LocatorRole::TableCell, 12, 5, "t", "two", "count", {0}});
        const ProblemSourceCatalog table(table_context);
        const auto row_one = handle_at(table, 6, 1), row_two = handle_at(table, 12, 2);
        check(table.resolve(table.snapshot(), row_one).locator == 1);
        check(table.resolve(table.snapshot(), row_two).locator == 2);
        rejects([&] { (void)table.resolve(table.snapshot(), row_one, row_two); });
        table_context.views[0].locators[1].row = "changed";
        check(ProblemSourceCatalog(table_context).snapshot() != table.snapshot());

        const std::string unicode = "\xce\xbc \xe2\x88\x92" "4\xe2\x80\xafkg \xf0\x9f\x8c\x8d";
        const ProblemSourceCatalog international(context(unicode));
        reconstructed.clear();
        for (const auto& entry : international.entries()) {
            if (!entry.whole_region) reconstructed += selected(international, entry.span);
        }
        check(reconstructed == unicode);
        check(selected(international, international.resolve(international.snapshot(), handle_at(international, 3)))
            == "\xe2\x88\x92");
        for (const auto& invalid : {std::string("\xc0\x80",2), std::string("\xed\xa0\x80",3),
                std::string("\xf4\x90\x80\x80",4), std::string("\xe2\x82",2), std::string("\x80",1)}) {
            auto malformed = context("valid");
            malformed.views[0].text = invalid;
            malformed.views[0].digest = evidence_digest(invalid);
            rejects([&] { (void)ProblemSourceCatalog(malformed); });
        }
        auto split_scalar = context(unicode);
        split_scalar.views[0].locators.push_back({LocatorRole::Paragraph, 1, 1, "", "", "", {}});
        rejects([&] { (void)ProblemSourceCatalog(split_scalar); });
        auto outside = context("abc");
        outside.views[0].locators.push_back({LocatorRole::Paragraph, 2, 2, "", "", "", {}});
        rejects([&] { (void)ProblemSourceCatalog(outside); });
        rejects([&] { (void)ProblemSourceCatalog(context(std::string(16385, '!'))); });
        auto overlap = context(std::string(65536, 'a'));
        for (std::size_t i = 0; i < 16; ++i)
            overlap.views[0].locators.push_back({LocatorRole::Paragraph, 0, 65536, "", "", "", {}});
        rejects([&] { (void)ProblemSourceCatalog(overlap); });

        auto count_context = context("Mara has exactly 4 items.");
        count_context.units[0] = {"item", "item", {}, 1, {"item", "items"}};
        const ProblemSourceCatalog counted(count_context);
        const auto proposal = "{\"snapshot\":\"" + counted.snapshot() + R"(","proposal":{
            "entities":["mara"],"quantities":[["count",0,[true,[]],"u0"]],
            "numbers":[[")" + handle_at(counted,17) + "\",\"" + handle_at(counted,19)
            + R"(","u0","exact",")" + handle_at(counted,9) + R"("]],
            "bindings":[[0,0,["s0"]]],"nodes":[],"equations":[],
            "predicates":[],"rules":[],"goal":["q",0,"u0"]}})";
        const auto parsed = parse_problem_source_proposal(proposal, counted);
        check(parsed.context == counted.context() && parsed.limits == counted.limits());
        check(parsed.proposal.numbers[0].numeral == counted.resolve(counted.snapshot(), handle_at(counted,17)));
        const auto result = execute_problem(parsed);
        check(result.status == ProblemStatus::Checked && result.answer == Rational(4));
        check(verify_problem_result(parsed, result));
        const auto schema_text = problem_source_proposal_schema(counted);
        const auto schema = std::unique_ptr<json_object, decltype(&json_object_put)>(
            json_tokener_parse(schema_text.c_str()), json_object_put);
        check(schema != nullptr);
        const auto definitions = json_object_object_get(schema.get(), "$defs");
        check(json_object_array_length(json_object_object_get(
            json_object_object_get(definitions, "source_handle"), "enum")) == counted.entries().size());
        check(std::string(json_object_get_string(json_object_object_get(
            json_object_object_get(json_object_object_get(schema.get(), "properties"), "snapshot"), "const")))
            == counted.snapshot());
        const auto bad_proposal = [&](auto action) {
            const auto invalid = mutate(proposal, action);
            rejects([&] { (void)parse_problem_source_proposal(invalid, counted); });
        };
        bad_proposal([](auto root) {
            json_object_object_add(root, "snapshot", json_object_new_string("stale"));
        });
        bad_proposal([](auto root) {
            json_object_object_add(root, "context", json_object_new_object());
        });
        bad_proposal([](auto root) {
            json_object_object_add(root, "proposal", nullptr);
        });
        bad_proposal([](auto root) {
            const auto body = json_object_object_get(root, "proposal");
            json_object_object_add(body, "answer", json_object_new_int(4));
        });
        bad_proposal([](auto root) {
            const auto goal = json_object_object_get(json_object_object_get(root, "proposal"), "goal");
            json_object_array_put_idx(goal, 1, json_object_new_int(999));
        });
        bad_proposal([](auto root) {
            const auto goal = json_object_object_get(json_object_object_get(root, "proposal"), "goal");
            json_object_array_put_idx(goal, 2, json_object_new_int(0));
        });
        bad_proposal([](auto root) {
            const auto numbers = json_object_object_get(json_object_object_get(root, "proposal"), "numbers");
            json_object_array_put_idx(json_object_array_get_idx(numbers, 0), 0,
                json_tokener_parse("[0,17,1,null]"));
        });
        bad_proposal([](auto root) {
            const auto numbers = json_object_object_get(json_object_object_get(root, "proposal"), "numbers");
            json_object_array_put_idx(json_object_array_get_idx(numbers, 0), 0,
                json_object_new_string("s99999"));
        });
        rejects([&] { (void)parse_problem_source_proposal(proposal + "{}", counted); });
        rejects([&] { (void)parse_problem_source_proposal(proposal.substr(0, proposal.size()-1), counted); });
        const auto decline = "{\"snapshot\":\"" + counted.snapshot()
            + R"(","proposal":{"status":"unsupported","reason":"scope quantification"}})";
        bool declined = false;
        try { (void)parse_problem_source_proposal(decline, counted); }
        catch (const ProblemDecline& error) { declined = error.kind == ProblemDeclineKind::Unsupported; }
        check(declined);
        std::cout << "Source catalog checks: " << checks << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
