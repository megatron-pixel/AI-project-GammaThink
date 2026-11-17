#include "binary_dispatch.hpp"
#include "core.hpp"
#include "relational.hpp"

#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void arity(const std::vector<std::string>& arguments, std::size_t expected) {
    if (arguments.size() != expected) throw std::runtime_error("Incorrect formal command argument count");
}
}

int main(int argc, char** argv) {
    try {
        const std::vector<std::string> arguments(argv + 1, argv + argc);
        if (arguments.empty() || arguments.front() == "--help" || arguments.front() == "help") {
            std::cout << axiom::help()
                      << "  compile-rel MODULE GOAL: emit a finite relational AXRM request\n"
                      << "  prove-rel MODULE GOAL: execute and check the relational proof on CPU\n"
                      << "This executable accepts formal input only; it contains no neural interpreter.\n";
            return 0;
        }
        const auto& command = arguments.front();
        if (command == "run" || command == "decode") {
            arity(arguments, 1);
            const auto bytes = axiom::read_binary(std::cin);
            if (command == "run") axiom::write_binary(std::cout, axiom::run_request_binary(bytes));
            else std::cout << axiom::render_binary_result(bytes);
            return 0;
        }
        if (command == "compile-rel" || command == "prove-rel") {
            arity(arguments, 3);
            const auto request = axiom::compile_relational(
                axiom::load_relational_module(arguments[1]), arguments[2]);
            const auto bytes = axiom::encode_relational_request(request);
            if (command == "compile-rel") axiom::write_binary(std::cout, bytes);
            else std::cout << axiom::render_binary_result(axiom::run_request_binary(bytes));
            return 0;
        }
        if (command == "compile") {
            const std::vector<std::string> input(arguments.begin() + 1, arguments.end());
            axiom::write_binary(std::cout, axiom::encode_request(axiom::compile(input)));
            return 0;
        }
        const auto request = axiom::compile(arguments);
        std::cout << axiom::render_binary_result(
            axiom::run_request_binary(axiom::encode_request(request)));
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
