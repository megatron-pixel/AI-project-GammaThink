#include "core.hpp"

#include <algorithm>
#include <array>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {
std::size_t checks = 0;

void check(bool condition) {
    ++checks;
    if (!condition) {
        throw std::runtime_error("check failed at assertion " + std::to_string(checks));
    }
}

template<class F> void rejects(F&& operation) {
    ++checks;
    try {
        operation();
    } catch (const std::exception&) {
        return;
    }
    throw std::runtime_error("expected rejection at assertion " + std::to_string(checks));
}

axiom::Result run(const std::vector<std::string>& args) {
    const auto request = axiom::encode_request(axiom::compile(args));
    check(axiom::encode_request(axiom::decode_request(request)) == request);
    const auto result = axiom::run_binary(request);
    check(axiom::encode_result(axiom::decode_result(result)) == result);
    return axiom::decode_result(result);
}

axiom::Request proof_request(const std::string& body, const std::string& goal) {
    return axiom::compile_proof(axiom::parse_module("axioms 1 tests 1.0\n" + body), goal);
}

axiom::Result prove(const std::string& body, const std::string& goal) {
    const auto request = proof_request(body, goal);
    return axiom::decode_result(axiom::run_binary(axiom::encode_request(request)));
}

std::vector<std::uint8_t> scalar_matcher(const std::vector<std::uint64_t>& masks, std::uint64_t known) {
    std::vector<std::uint8_t> result;
    for (auto mask : masks) {
        result.push_back(static_cast<std::uint8_t>((mask & known) == mask));
    }
    return result;
}

void mathematics() {
    check(run({"eval", "1/3+1/6"}).values == std::vector<axiom::Rational>{axiom::Rational(1, 2)});
    check(run({"evaluate", "-(2+3)*4/5+7"}).values.front() == 3);
    check(run({"eval", "1000000000000000000000000000000/3*3"}).values.front() ==
          axiom::Rational("1000000000000000000000000000000"));
    check(run({"eval", "8/4/2"}).values.front() == 1);
    check(run({"solve", "2*x+3=11"}).values.front() == 4);
    check(run({"solve", "3*(x-1)/2=2"}).values.front() == axiom::Rational(7, 3));
    check(run({"solve", "x=x"}).status == axiom::Status::Infinite);
    check(run({"solve", "x=x+1"}).status == axiom::Status::Inconsistent);
    check(run({"solve", "0*x=0"}).status == axiom::Status::Infinite);
    for (const auto& expression : {"x*x=1", "x/x=1", "1/(x-1)=0", "2x=4", "x^2=4",
                                   "x==2", "=2", "x=", "sqrt(2)=x"}) {
        rejects([&] { static_cast<void>(run({"solve", expression})); });
    }
    for (const auto& expression : {"1/0", "1/(2-2)", "1+", "2 3", "0.5", "x", "()",
                                   "(1", "1)", "2**3", "arbitrary English"}) {
        rejects([&] { static_cast<void>(run({"eval", expression})); });
    }
    rejects([] { static_cast<void>(run({"eval", std::string(129, '(') + "1" + std::string(129, ')')})); });
    rejects([] { static_cast<void>(run({"eval", std::string(1300, '9')})); });
    std::string many = "1";
    for (int i = 0; i < 300; ++i) {
        many += "+1";
    }
    rejects([&] { static_cast<void>(run({"eval", many})); });
    rejects([] { static_cast<void>(run({"eval", std::string(1000, '9') + "*" + std::string(1000, '9')})); });

    check(run({"det", "[1,2;3,4]"}).values.front() == -2);
    check(run({"det", "[1,2,3;0,4,5;1,0,6]"}).values.front() == 22);
    check(run({"det", "[0,1;1,0]"}).values.front() == -1);
    check(run({"det", "[2/3]"}).values.front() == axiom::Rational(2, 3));
    check(run({"det", "[1,2;2,4]"}).values.front() == 0);
    rejects([] { static_cast<void>(run({"det", "[1,2,3;4,5,6]"})); });
    rejects([] { static_cast<void>(run({"det", "[1,2;3]"})); });
    rejects([] { static_cast<void>(run({"det", "[]"})); });
    std::string big_matrix = "[";
    for (int i = 0; i < 17; ++i) {
        big_matrix += (i ? ";" : "") + std::string("1");
    }
    big_matrix += "]";
    rejects([&] { static_cast<void>(run({"det", big_matrix})); });

    const auto unique = run({"linear", "[1,2;3,4]", "[5;11]"});
    check(unique.status == axiom::Status::Ok);
    check(unique.values == std::vector<axiom::Rational>({1, 2}));
    const auto singular = run({"linear", "[1,2;2,4]", "[3;6]"});
    check(singular.status == axiom::Status::Infinite);
    check(singular.values == std::vector<axiom::Rational>({3, 0}));
    check(singular.basis == axiom::Matrix({{-2, 1}}));
    check(run({"linear", "[1,2;2,4]", "[3;7]"}).status == axiom::Status::Inconsistent);
    const auto rectangular = run({"linear", "[1,2,0;0,1,1]", "[5;3]"});
    check(rectangular.status == axiom::Status::Infinite);
    check(rectangular.values == std::vector<axiom::Rational>({-1, 3, 0}));
    check(rectangular.basis == axiom::Matrix({{2, -1, 1}}));
    check(run({"linear", "[1;2;3]", "[2;4;6]"}).values.front() == 2);
    check(run({"linear", "[1;2;3]", "[2;4;7]"}).status == axiom::Status::Inconsistent);
    const auto zero = run({"linear", "[0,0;0,0]", "[0;0]"});
    check(zero.status == axiom::Status::Infinite && zero.basis == axiom::Matrix({{1, 0}, {0, 1}}));
    rejects([] { static_cast<void>(run({"linear", "[1,2;3,4]", "[1,2]"})); });
    rejects([] { static_cast<void>(run({"linear", "[1,2;3,4]", "[1]"})); });

    const auto quantum = run({"born", "[3/5,0;0,4/5]"});
    check(quantum.values == std::vector<axiom::Rational>({axiom::Rational(9, 25), axiom::Rational(16, 25)}));
    check(quantum.values[0] + quantum.values[1] == 1);
    check(run({"born", "[0,1;0,0]"}).values == std::vector<axiom::Rational>({1, 0}));
    check(run({"born", "[-3/5,-4/5]"}).values.front() == 1);
    rejects([] { static_cast<void>(run({"born", "[1,0;1,0]"})); });
    rejects([] { static_cast<void>(run({"born", "[0,0]"})); });
    rejects([] { static_cast<void>(run({"born", "[1]"})); });
}

void logic() {
    const std::string branching =
        "rule detour -> destination | cyclic branch\n"
        "rule destination -> detour | cyclic branch\n"
        "rule missing -> destination | failing branch\n"
        "rule start & bridge -> destination | valid branch\n"
        "fact start | premise one\n"
        "fact bridge | premise two\n";
    const auto result = prove(branching, "destination");
    check(result.status == axiom::Status::Proved && !result.limited);
    check(axiom::verify_proof(result.module, result.proof));
    check(std::any_of(result.history.begin(), result.history.end(),
                      [](const auto& event) { return event.kind == axiom::EventKind::Cycle; }));
    check(std::any_of(result.history.begin(), result.history.end(),
                      [](const auto& event) { return event.kind == axiom::EventKind::Backtrack; }));
    check(result.proof[result.positive_root].dependencies.size() == 2);
    check(prove(branching, "unmentioned").status == axiom::Status::Unknown);
    check(prove("rule a -> b | cycle\nrule b -> a | cycle\n", "a").status == axiom::Status::Unknown);
    check(prove("fact !a | negative premise\n", "a").status == axiom::Status::Disproved);
    check(prove("fact !a | negative premise\nrule !a -> b | explicit negative premise\n", "b").status ==
          axiom::Status::Proved);
    check(prove("fact a | positive premise\n", "!a").status == axiom::Status::Disproved);
    const auto contradictory = prove("fact a | source A\nfact !a | source B\n", "a");
    check(contradictory.status == axiom::Status::Contradiction);
    check(contradictory.positive_root != axiom::no_step && contradictory.negative_root != axiom::no_step);
    check(prove("fact a | source A\nfact !a | source B\n", "unrelated").status == axiom::Status::Unknown);
    // A proved premise from a failed branch is safe; a missing premise is not.
    check(prove("rule p & missing -> g | dead\nrule p -> g | live\nfact p | seed\n", "g").status ==
          axiom::Status::Proved);
    // Failure in one active path must not become a global negative memo.
    check(prove("rule b -> a | cycle\nrule a -> b | cycle\nfact a | alternate\nrule a & b -> g | join\n",
                "g").status == axiom::Status::Proved);
    // Interleaved unrelated heads must not change branch order or rule IDs.
    const auto interleaved = prove(
        "fact unrelated | noise\nrule missing -> g | dead\nfact p | seed\n"
        "rule p -> g | live\nfact other | noise\nrule q -> g | unused\n"
        "fact !g | counterproof\n", "g");
    check(interleaved.status == axiom::Status::Contradiction);
    std::vector<std::uint32_t> attempted;
    for (const auto& event : interleaved.history) {
        if (event.kind == axiom::EventKind::TryRule) attempted.push_back(event.rule);
    }
    check(attempted == std::vector<std::uint32_t>({1, 3, 2, 6}));
    check(interleaved.proof[interleaved.positive_root].rule == 3);
    check(interleaved.proof[interleaved.negative_root].rule == 6);
    auto request = proof_request(branching, "destination");
    request.limits.events = 1;
    const auto limited = axiom::execute(request);
    check(limited.status == axiom::Status::Unknown && limited.limited);
    check(limited.history.size() == 1);
    request = proof_request("fact !g | counterproof\nrule b -> g | depth\nrule c -> b | depth\n"
                            "fact c | seed\n", "g");
    request.limits.depth = 1;
    const auto depth_limited = axiom::execute(request);
    check(depth_limited.status == axiom::Status::Unknown && depth_limited.limited);
    request.limits.events = 0;
    rejects([&] { static_cast<void>(axiom::execute(request)); });

    auto corrupt = result.proof;
    corrupt.back().dependencies[0] = static_cast<std::uint32_t>(corrupt.size() - 1);
    check(!axiom::verify_proof(result.module, corrupt));
    corrupt = result.proof;
    corrupt.back().literal ^= 1;
    check(!axiom::verify_proof(result.module, corrupt));
    corrupt = result.proof;
    corrupt.front().rule = axiom::no_step;
    check(!axiom::verify_proof(result.module, corrupt));
    auto bad_result = result;
    bad_result.positive_root = axiom::no_step;
    rejects([&] { static_cast<void>(axiom::encode_result(bad_result)); });
    bad_result = contradictory;
    bad_result.status = axiom::Status::Proved;
    bad_result.negative_root = axiom::no_step;
    rejects([&] { static_cast<void>(axiom::encode_result(bad_result)); });

    for (const auto& source : {
        "", "axioms 2 bad 1\n", "axioms 1 bad 1 extra\n",
        "axioms 1 bad 1\nfact a\n", "axioms 1 bad 1\nfact a | \n",
        "axioms 1 bad 1\nrule a -> | missing head\n",
        "axioms 1 bad 1\nrule -> a | missing body\n",
        "axioms 1 bad 1\nfact a(x) | variables unsupported\n",
        "axioms 1 bad 1\nfact !!a | invalid negation\n",
        "axioms 1 bad 1\nrule a -> b -> c | two heads\n"}) {
        rejects([&] { static_cast<void>(axiom::parse_module(source)); });
    }
    rejects([] { static_cast<void>(proof_request("fact a | seed\n", "a & b")); });
}

void codec() {
    const auto request = proof_request("fact a | seed\nrule a -> b | implication\n", "b");
    const auto wire = axiom::encode_request(request);
    const auto result = axiom::run_binary(wire);
    check(axiom::encode_request(axiom::decode_request(wire)) == wire);
    check(axiom::encode_result(axiom::decode_result(result)) == result);
    for (std::size_t length = 0; length < wire.size(); ++length) {
        rejects([&] { static_cast<void>(axiom::decode_request(std::span(wire).first(length))); });
    }
    for (std::size_t length = 0; length < result.size(); ++length) {
        rejects([&] { static_cast<void>(axiom::decode_result(std::span(result).first(length))); });
    }
    for (auto index : {0U, 4U, 5U, 6U, 7U, 8U, 12U}) {
        auto corrupt = wire;
        corrupt[index] = 255;
        rejects([&] { static_cast<void>(axiom::decode_request(corrupt)); });
    }
    auto trailing = wire;
    trailing.push_back(0);
    rejects([&] { static_cast<void>(axiom::decode_request(trailing)); });
    rejects([&] { static_cast<void>(axiom::decode_result(wire)); });
    rejects([&] { static_cast<void>(axiom::decode_request(result)); });
    axiom::Bytes huge(axiom::max_wire_bytes + 1, 0);
    rejects([&] { static_cast<void>(axiom::decode_request(huge)); });
    auto expression = axiom::encode_request(axiom::compile({"eval", "1"}));
    // Expression count lives immediately after the operation byte.
    expression[13] = 1;
    expression[14] = 2; // 513 instructions, above the cap.
    rejects([&] { static_cast<void>(axiom::decode_request(expression)); });
    expression = axiom::encode_request(axiom::compile({"eval", "1"}));
    expression[17] = static_cast<std::uint8_t>(axiom::Code::Add);
    rejects([&] { static_cast<void>(axiom::decode_request(expression)); });
    expression = axiom::encode_request(axiom::compile({"eval", "1"}));
    expression[18] = 2; // Invalid rational sign.
    rejects([&] { static_cast<void>(axiom::decode_request(expression)); });
    expression = axiom::encode_request(axiom::compile({"eval", "1"}));
    expression[23] = 0; // Noncanonical positive magnitude ending in zero.
    rejects([&] { static_cast<void>(axiom::decode_request(expression)); });
    auto invalid_probability = run({"born", "[1,0]"});
    invalid_probability.values[0] = axiom::Rational(1, 2);
    rejects([&] { static_cast<void>(axiom::encode_result(invalid_probability)); });

    std::stringstream stream(std::ios::in | std::ios::out | std::ios::binary);
    axiom::write_binary(stream, wire);
    check(axiom::read_binary(stream) == wire);
    std::istringstream oversized(std::string(axiom::max_wire_bytes + 1, 'x'));
    rejects([&] { static_cast<void>(axiom::read_binary(oversized)); });
}

void gpu_boundary_and_oracle() {
    // Exhaust 256 finite two-symbol theories against an independent fixed-point
    // oracle. This exercises CPU search and the GPU callback protocol, NOT CUDA.
    const std::array<axiom::Rule, 8> candidates{{
        {0, {}, "a"}, {1, {}, "not a"}, {2, {}, "b"}, {3, {}, "not b"},
        {0, {2}, "b implies a"}, {2, {0}, "a implies b"},
        {1, {2, 3}, "both b polarities imply not a"}, {3, {1}, "not a implies not b"}
    }};
    for (unsigned mask = 0; mask < 256; ++mask) {
        axiom::Module module{"oracle", "1", {"a", "b"}, {}};
        for (std::size_t i = 0; i < candidates.size(); ++i) {
            if ((mask & (1U << i)) != 0) {
                module.rules.push_back(candidates[i]);
            }
        }
        std::array<bool, 4> known{};
        for (int round = 0; round < 4; ++round) {
            for (const auto& rule : module.rules) {
                bool ready = true;
                for (auto premise : rule.premises) {
                    ready = ready && known[premise];
                }
                known[rule.head] = known[rule.head] || ready;
            }
        }
        for (std::uint32_t goal = 0; goal < 4; ++goal) {
            axiom::Request request;
            request.operation = axiom::Operation::Prove;
            request.module = module;
            request.goal = goal;
            const auto expected = known[goal] ?
                (known[goal ^ 1U] ? axiom::Status::Contradiction : axiom::Status::Proved) :
                (known[goal ^ 1U] ? axiom::Status::Disproved : axiom::Status::Unknown);
            const auto cpu = axiom::execute(request);
            check(cpu.status == expected);
            check(axiom::verify_proof(module, cpu.proof));
            if (!module.rules.empty()) {
                request.gpu = true;
                const auto gpu = axiom::decode_result(axiom::run_binary(axiom::encode_request(request), scalar_matcher));
                check(gpu.status == expected && gpu.gpu);
                check(axiom::verify_proof(module, gpu.proof));
            }
        }
    }
    auto request = proof_request("fact a | seed\nrule a -> b | step\n", "b");
    request.gpu = true;
    rejects([&] { static_cast<void>(axiom::execute(request)); });
    rejects([&] { static_cast<void>(axiom::execute(request, [](const auto&, auto) {
        return std::vector<std::uint8_t>{};
    })); });
    rejects([&] { static_cast<void>(axiom::execute(request, [](const auto& masks, auto) {
        return std::vector<std::uint8_t>(masks.size(), 0);
    })); });
    rejects([&] { static_cast<void>(axiom::execute(request, [](const auto& masks, auto) {
        return std::vector<std::uint8_t>(masks.size(), 1);
    })); });
    request.limits.events = 1;
    const auto limited = axiom::execute(request, scalar_matcher);
    check(limited.status == axiom::Status::Unknown && limited.limited);
    request.limits.events = 4096;
    for (int i = 2; i < 33; ++i) {
        request.module.symbols.push_back("symbol" + std::to_string(i));
    }
    rejects([&] { static_cast<void>(axiom::execute(request, scalar_matcher)); });
}
} // namespace

int main() {
    try {
        mathematics();
        logic();
        codec();
        gpu_boundary_and_oracle();
        std::cout << "core tests passed: " << checks << " deterministic assertions\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
