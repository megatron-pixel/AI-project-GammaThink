#include "binary_dispatch.hpp"
#include "relational.hpp"
#include "problem_fixture.hpp"
#include "problem_wire.hpp"

#include <iostream>
#include <stdexcept>

namespace {
void check(bool condition) {
    if (!condition) throw std::runtime_error("Binary dispatch assertion failed");
}
template<class Function> void rejects(Function operation) {
    try { operation(); } catch (const std::exception&) { return; }
    throw std::runtime_error("Binary dispatch accepted an invalid record");
}
}

int main() {
    try {
        // Frozen from the working v1 executable before version dispatch changed.
        const axiom::Bytes request{
            65,88,82,77,1,0,1,0,30,0,0,0,1,3,0,0,
            0,1,0,1,0,0,0,2,1,0,0,0,1,1,0,1,
            0,0,0,3,1,0,0,0,1,3};
        const axiom::Bytes result{
            65,88,82,77,1,0,2,0,23,0,0,0,1,1,0,0,
            1,0,0,0,0,1,0,0,0,5,1,0,0,0,1,0,
            0,0,0};
        check(axiom::encode_request(axiom::compile({"eval","2+3"})) == request);
        check(axiom::run_request_binary(request) == result);
        check(axiom::render_binary_result(result) == axiom::render(axiom::decode_result(result)));
        auto gpu_request = axiom::compile_proof(
            axiom::parse_module("axioms 1 dispatch 1\nfact x | seed\n"), "x");
        gpu_request.gpu = true;
        bool gpu_called = false;
        const auto matcher = [&](const std::vector<std::uint64_t>& masks, std::uint64_t known) {
            gpu_called = true;
            std::vector<std::uint8_t> eligible;
            for (const auto mask : masks) eligible.push_back((mask & known) == mask);
            return eligible;
        };
        const auto gpu_result = axiom::decode_result(
            axiom::run_request_binary(axiom::encode_request(gpu_request), matcher));
        check(gpu_called && gpu_result.gpu && gpu_result.status == axiom::Status::Proved);
        const auto relational = axiom::compile_relational(
            axiom::load_relational_module("modules/relations.axioms"), "ancestor(ana,cy)");
        const auto input = axiom::encode_relational_request(relational);
        const auto output = axiom::run_request_binary(input);
        const auto decoded = axiom::decode_relational_result(output);
        check(decoded.proof.status == axiom::Status::Proved);
        check(axiom::verify_relational_result(relational, decoded));
        check(axiom::render_binary_result(output) == axiom::render_relational(decoded));
        const auto problem = problem_fixture::request();
        const auto problem_wire = axiom::encode_problem_request(problem);
        const auto problem_output = axiom::run_request_binary(problem_wire);
        const auto problem_decoded = axiom::decode_problem_result(problem_output);
        check(problem_decoded.answer == axiom::Rational(17,5));
        check(axiom::verify_problem_result(problem,problem_decoded));
        check(axiom::render_binary_result(problem_output) == axiom::render_problem(problem_decoded));
        rejects([&] { axiom::run_request_binary(problem_output); });
        rejects([&] { axiom::render_binary_result(problem_wire); });
        rejects([&] { axiom::run_request_binary(output); });
        rejects([&] { axiom::render_binary_result(input); });
        for (std::size_t length = 0; length < 12; ++length) {
            rejects([&] { axiom::run_request_binary(std::span(request).first(length)); });
            rejects([&] { axiom::render_binary_result(std::span(result).first(length)); });
        }
        for (const auto v : {0U, 3U, 256U, 65535U}) {
            auto invalid = request;
            invalid[4] = static_cast<std::uint8_t>(v);
            invalid[5] = static_cast<std::uint8_t>(v >> 8);
            rejects([&] { axiom::run_request_binary(invalid); });
            invalid[6] = 2;
            rejects([&] { axiom::render_binary_result(invalid); });
        }
        for (const auto index : {0U, 6U, 7U, 8U}) {
            auto invalid = input;
            invalid[index] = 255;
            rejects([&] { axiom::run_request_binary(invalid); });
        }
        auto huge = request;
        huge.resize(axiom::max_wire_bytes + 1);
        rejects([&] { axiom::run_request_binary(huge); });
        std::cout << "BINARY_DISPATCH_TEST_OK legacy_bytes_unchanged=true relational_checked=true problem_checked=true\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
