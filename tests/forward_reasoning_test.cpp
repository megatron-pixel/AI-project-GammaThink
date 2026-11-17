#include "forward_reasoning.hpp"
#include "forward_cases.hpp"
#include "reasoning.hpp"
#include "relational.hpp"
#include "problem_fixture.hpp"
#include "problem_wire.hpp"

#include <iostream>
#include <stdexcept>

namespace {
std::size_t assertions = 0;
void check(bool value) {
    ++assertions;
    if (!value) throw std::runtime_error("Forward reasoning assertion " + std::to_string(assertions));
}
template<class F> void rejects(F&& operation) {
    ++assertions;
    try { operation(); } catch (const std::runtime_error&) { return; }
    throw std::runtime_error("Expected forward rejection");
}
}

int main() {
    try {
        using namespace axiom;
        using namespace axiom::knowledge;
        for (const auto& fixture : forward_fixture::cases) {
            auto input = compile_proof(parse_module(std::string(fixture.module)), std::string(fixture.goal));
            input.limits = {fixture.events, fixture.depth};
            const auto run = reason_forward(input, fixture.work);
            check(run.reply.status == fixture.status);
            check(run.reply.limited == fixture.limited);
            check(run.work <= fixture.work && run.reply.history.size() <= fixture.events);
            check(verify_proof(run.reply.module, run.reply.proof));
            check(check_native_reply(encode_request(input), encode_result(run.reply)).valid);
        }
        // Reverse packing forces multiple passes; this is not a topologically
        // sorted example that could pass with a single accidental traversal.
        Module module;
        module.name = "bounded_chain";
        module.version = "1";
        for (unsigned i = 0; i < 24; ++i) module.symbols.push_back("p" + std::to_string(i));
        for (unsigned i = 23; i > 0; --i) {
            module.rules.push_back({i * 2, {(i - 1) * 2}, "synthetic chain"});
        }
        module.rules.push_back({0, {}, "synthetic given"});
        const auto request = compile_proof(module, "p23");
        const auto full = reason_forward(request, 100000);
        check(full.reply.status == Status::Proved && !full.reply.limited);
        check(full.work > module.rules.size() && full.work <= 100000);
        check(verify_proof(full.reply.module, full.reply.proof));
        check(check_native_reply(encode_request(request), encode_result(full.reply)).conclusive);
        check(execute(request).status == full.reply.status);
        for (const auto budget : {0U, 1U, 10U, 50U}) {
            const auto limited = reason_forward(request, budget);
            check(limited.work <= budget && limited.reply.limited);
            check(limited.reply.status == Status::Unknown);
            check(limited.reply.history.size() <= request.limits.events);
            check(check_native_reply(encode_request(request), encode_result(limited.reply)).valid);
        }
        auto depth_bound = request;
        depth_bound.limits.depth = 2;
        const auto shallow = reason_forward(depth_bound, 100000);
        check(shallow.reply.status == Status::Unknown && shallow.reply.limited);
        auto event_bound = request;
        event_bound.limits.events = 1;
        const auto brief = reason_forward(event_bound, 100000);
        check(brief.reply.status == Status::Unknown && brief.reply.history.size() == 1);
        auto alternate = compile_proof(parse_module(
            "axioms 1 shortest 1\nfact seed | given\n"
            "rule seed -> a | long-one\nrule a -> b | long-two\n"
            "rule b -> p | long-three\nrule p -> target | finish\n"
            "rule seed -> p | short-alternative\n"), "target");
        alternate.limits.depth = 3;
        const auto shortest = reason_forward(alternate, 10000);
        check(shortest.reply.status == Status::Proved && !shortest.reply.limited);
        check(verify_proof(shortest.reply.module, shortest.reply.proof));
        rejects([&] { reason_forward(compile({"eval", "2+3"}), 100); });
        auto gpu = request;
        gpu.gpu = true;
        rejects([&] { reason_forward(gpu, 100); });

        Journal journal(":memory:");
        Object goal;
        goal.kind = ObjectKind::Goal;
        goal.format = native_request_format;
        goal.payload = encode_request(request);
        const auto id = journal.put(goal);
        NativeRunLimits limits;
        limits.strategy = NativeStrategy::Forward;
        limits.forward_work = 100000;
        const auto cold = run_native(journal, id, limits);
        const auto warm = run_native(journal, id, limits);
        check(cold.check.conclusive && !cold.reused && cold.producer_work == full.work);
        check(warm.reused && warm.reply == cold.reply && !warm.producer_work);
        const auto backward_hit = run_native(journal, id);
        check(backward_hit.reused && backward_hit.check.conclusive);
        const auto relational = compile_relational(
            load_relational_module("modules/relations.axioms"), "ancestor(ana,cy)");
        goal.payload = encode_relational_request(relational);
        const auto relational_id = journal.put(goal);
        const auto relational_run = run_native(journal, relational_id, limits);
        check(relational_run.check.conclusive && relational_run.producer_work.has_value());
        check(decode_relational_result(relational_run.reply).proof.status == Status::Proved);
        for (const auto& incompatible : {encode_request(compile({"eval", "2+3"})),
                                        encode_problem_request(problem_fixture::request())}) {
            goal.payload = incompatible;
            const auto other_id = journal.put(goal);
            check(run_native(journal, other_id).check.conclusive);
            rejects([&] { run_native(journal, other_id, limits); });
        }
        std::cout << "forward reasoning tests passed: " << assertions
                  << " deterministic assertions\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
