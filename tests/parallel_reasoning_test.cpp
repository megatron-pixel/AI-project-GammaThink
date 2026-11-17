#include "parallel_reasoning.hpp"
#include "binary_dispatch.hpp"
#include "relational.hpp"

#include <iostream>

namespace {
using namespace axiom::knowledge;
std::size_t checks = 0;
void check(bool value) {
    ++checks;
    if (!value) throw std::runtime_error("Parallel reasoning assertion " + std::to_string(checks));
}
template<class F> void rejects(ErrorCode code, F&& operation) {
    ++checks;
    try { operation(); }
    catch (const Error& error) {
        if (error.code == code) return;
        throw;
    }
    throw std::runtime_error("Expected parallel reasoning rejection");
}
Bytes proof(std::string source, std::string goal) {
    return axiom::encode_request(axiom::compile_proof(axiom::parse_module(source), goal));
}
void equivalence() {
    const auto joint = proof(
        "axioms 1 parallel 1\nfact a | source-a\nfact b | source-b\n"
        "rule a & a & b -> c | ordered-joint\n", "c");
    const auto conflict = proof("axioms 1 conflict 1\nfact a | one\nfact !a | two\n", "a");
    const auto unknown = proof("axioms 1 unknown 1\nrule a -> a | cycle\n", "a");
    const auto relational = axiom::encode_relational_request(axiom::compile_relational(
        axiom::load_relational_module("modules/relations.axioms"), "ancestor(ana,cy)"));
    const std::vector<Bytes> requests{joint, unknown, conflict, joint, relational, unknown};
    const auto serial = run_proof_batch(requests);
    ProofBatchLimits limits;
    limits.workers = 4;
    const auto parallel = run_proof_batch(requests, limits);
    check(serial.workers_started == 1 && parallel.workers_started == 4);
    check(parallel.executions == 4 && parallel.memo_hits == 2);
    check(parallel.replies.size() == requests.size());
    check(parallel.peak_active_workers > 0 && parallel.peak_active_workers <= 4);
    check(parallel.reserved_search_events == 4 * axiom::Limits{}.events);
    for (std::size_t index = 0; index < requests.size(); ++index) {
        const auto& reply = parallel.replies[index];
        check(reply.reply == serial.replies[index].reply);
        check(reply.reply == axiom::run_request_binary(requests[index]));
        check(reply.check.valid && check_native_reply(requests[index], reply.reply).valid);
        check(reply.check.conclusive == serial.replies[index].check.conclusive);
        check(reply.check.conflict == serial.replies[index].check.conflict);
    }
    check(parallel.replies[3].producer == 0 && parallel.replies[5].producer == 1);
    check(!parallel.replies[1].check.conclusive && !parallel.replies[1].check.conflict);
    check(parallel.replies[2].check.conclusive && parallel.replies[2].check.conflict);
    const auto decoded = axiom::decode_result(parallel.replies[0].reply);
    check(decoded.proof.at(decoded.positive_root).dependencies.size() == 3);
    check(decoded.proof.at(decoded.positive_root).dependencies[0] ==
          decoded.proof.at(decoded.positive_root).dependencies[1]);
    auto changed = axiom::decode_request(joint);
    changed.module.version = "2";
    const auto provenance = run_proof_batch(std::vector<Bytes>{joint, axiom::encode_request(changed)});
    check(provenance.executions == 2 && provenance.memo_hits == 0);
    check(!check_native_reply(joint, provenance.replies[1].reply).valid);
    auto bounded = axiom::decode_request(joint);
    bounded.limits.events = 1;
    const auto limited = run_proof_batch(std::vector<Bytes>{axiom::encode_request(bounded)});
    check(limited.replies[0].check.valid && !limited.replies[0].check.conclusive);
    check(axiom::decode_result(limited.replies[0].reply).limited);
    const auto empty = run_proof_batch({});
    check(empty.replies.empty() && empty.executions == 0 && empty.workers_started == 0);
}
void budgets() {
    const std::vector<Bytes> requests{proof("axioms 1 budget 1\nfact a | given\n", "a")};
    ProofBatchLimits limits;
    limits.workers = 0;
    rejects(ErrorCode::Budget, [&] { run_proof_batch(requests, limits); });
    limits.workers = 5;
    rejects(ErrorCode::Budget, [&] { run_proof_batch(requests, limits); });
    limits = {};
    limits.input_bytes = requests[0].size() - 1;
    rejects(ErrorCode::Budget, [&] { run_proof_batch(requests, limits); });
    limits = {};
    limits.output_bytes = axiom::max_wire_bytes - 1;
    rejects(ErrorCode::Budget, [&] { run_proof_batch(requests, limits); });
    limits = {};
    limits.search_events = axiom::Limits{}.events - 1;
    rejects(ErrorCode::Budget, [&] { run_proof_batch(requests, limits); });
    limits = {};
    limits.search_depth = axiom::Limits{}.depth - 1;
    rejects(ErrorCode::Budget, [&] { run_proof_batch(requests, limits); });
    limits = {};
    limits.requests = 1;
    rejects(ErrorCode::Budget, [&] { run_proof_batch(std::vector<Bytes>{requests[0], requests[0]}, limits); });
    rejects(ErrorCode::Schema, [&] { run_proof_batch(std::vector<Bytes>{Bytes{1, 2, 3}}); });
    rejects(ErrorCode::Schema, [&] {
        run_proof_batch(std::vector<Bytes>{axiom::encode_request(axiom::compile({"eval", "2+3"}))});
    });
    auto gpu = axiom::decode_request(requests[0]);
    gpu.gpu = true;
    rejects(ErrorCode::Permission, [&] {
        run_proof_batch(std::vector<Bytes>{axiom::encode_request(gpu)});
    });
}
} // namespace

int main() {
    try {
        equivalence();
        budgets();
        std::cout << "parallel reasoning tests passed: " << checks << " deterministic checks\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
