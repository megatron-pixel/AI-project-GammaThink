#include "hypotheses.hpp"
#include "core.hpp"

#include <iostream>
#include <stdexcept>

namespace {
using namespace axiom;
using namespace axiom::knowledge;
std::size_t assertions = 0;
void check(bool value) {
    ++assertions;
    if (!value) throw std::runtime_error("Hypothesis assertion " + std::to_string(assertions));
}
template<class F> void rejects(F&& operation) {
    ++assertions;
    try { operation(); } catch (const std::runtime_error&) { return; }
    throw std::runtime_error("Expected hypothesis rejection");
}
Object goal(std::string_view statements, std::string query = "g") {
    Object result;
    result.kind = ObjectKind::Goal;
    result.format = native_request_format;
    result.payload = encode_request(compile_proof(
        parse_module("axioms 1 hypotheses 1\n" + std::string(statements)), query));
    return result;
}
std::string literal(const HypothesisReport& report, const ObjectId& id) {
    for (const auto& object : report.hypotheses) {
        if (identify(object) == id) {
            const auto request = decode_request(object.payload);
            return (request.goal % 2 ? "!" : "") + request.module.symbols.at(request.goal / 2);
        }
    }
    throw std::runtime_error("Missing hypothesis object");
}
}

int main() {
    try {
        const auto input = goal("fact a | given\nrule a & b -> g | missing-b\n");
        const auto before = encode_object(input);
        const auto report = propose_hypotheses(input);
        check(!report.formally_supported && !report.limited);
        check(report.alternatives.size() == 1 && report.hypotheses.size() == 1);
        check(literal(report, report.alternatives[0].jointly_required[0]) == "b");
        check(!report.alternatives[0].contradicts_known);
        check(encode_object(input) == before);
        Journal journal(":memory:");
        const auto request_id = journal.put(input);
        for (const auto& candidate : report.hypotheses) {
            check(candidate.kind == ObjectKind::Hypothesis && candidate.visibility == Visibility::Private);
            check(candidate.links == std::vector<Link>{{Relationship::DependsOn, request_id}});
            const auto id = journal.put(candidate);
            check(!journal.assessment(id));
            check(journal.by_kind(ObjectKind::Fact).objects.empty());
            rejects([&] { run_native(journal, id); });
        }
        check(decode_result(run_native(journal, request_id).reply).status == Status::Unknown);

        const auto joint = propose_hypotheses(goal("rule a & b -> g | joint\n"));
        check(joint.alternatives.size() == 1 && joint.alternatives[0].jointly_required.size() == 2);
        check(literal(joint, joint.alternatives[0].jointly_required[0]) == "a");
        check(literal(joint, joint.alternatives[0].jointly_required[1]) == "b");
        const auto alternatives = propose_hypotheses(goal(
            "rule a -> g | first\nrule b -> g | second\n"));
        check(alternatives.alternatives.size() == 2);
        check(literal(alternatives, alternatives.alternatives[0].jointly_required[0]) == "a");
        check(literal(alternatives, alternatives.alternatives[1].jointly_required[0]) == "b");
        const auto paths = propose_hypotheses(goal(
            "rule a -> p | path-one\nrule a -> q | path-two\n"
            "rule p -> g | first\nrule q -> g | second\n"));
        check(paths.alternatives.size() == 2 && paths.hypotheses.size() == 1);
        check(paths.alternatives[0].source_rules != paths.alternatives[1].source_rules);
        const auto conflict = propose_hypotheses(goal(
            "fact !a | contrary\nrule a -> g | candidate\n"));
        check(conflict.alternatives.size() == 1 && conflict.alternatives[0].contradicts_known);
        const auto internal_conflict = propose_hypotheses(goal("rule a & !a -> g | conflicting\n"));
        check(internal_conflict.alternatives[0].contradicts_known);
        const auto repetition = propose_hypotheses(goal("rule a & a -> g | repeated\n"));
        check(repetition.alternatives[0].jointly_required.size() == 2);
        check(repetition.alternatives[0].jointly_required[0] ==
              repetition.alternatives[0].jointly_required[1]);
        const auto cycle = propose_hypotheses(goal("rule g -> p | cycle-one\nrule p -> g | cycle-two\n"));
        check(cycle.alternatives.empty() && !cycle.formally_supported && !cycle.limited);
        const auto unsupported = propose_hypotheses(goal("fact a | unrelated\n"));
        check(unsupported.alternatives.empty() && unsupported.hypotheses.empty());
        const auto supported = propose_hypotheses(goal("fact g | given\n"));
        check(supported.formally_supported && supported.hypotheses.empty());
        const auto disputed = propose_hypotheses(goal("fact g | first\nfact !g | second\n"));
        check(disputed.formally_supported && disputed.existing_contradiction);
        HypothesisLimits limits;
        limits.work = 0;
        const auto zero = propose_hypotheses(input, limits);
        check(zero.limited && zero.work == 0 && zero.alternatives.empty());
        limits = {};
        limits.plans = 1;
        const auto truncated = propose_hypotheses(goal(
            "rule a -> g | first\nrule b -> g | second\n"), limits);
        check(truncated.limited && truncated.alternatives.size() == 1);
        limits = {};
        limits.hypotheses = 1;
        const auto no_partial_joint = propose_hypotheses(goal("rule a & b -> g | joint\n"), limits);
        check(no_partial_joint.limited && no_partial_joint.alternatives.empty());
        limits = {};
        limits.depth = 0;
        const auto shallow = propose_hypotheses(input, limits);
        check(shallow.limited && shallow.hypotheses.empty());
        limits = {};
        limits.frontier = maximum_items;
        limits.obligations = maximum_items;
        rejects([&] { propose_hypotheses(input, limits); });
        auto malformed = input;
        malformed.kind = ObjectKind::Fact;
        rejects([&] { propose_hypotheses(malformed); });
        malformed = input;
        malformed.payload = encode_request(compile({"eval", "2+3"}));
        rejects([&] { propose_hypotheses(malformed); });
        std::cout << "hypothesis tests passed: " << assertions << " deterministic assertions\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
