#include "core.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::runtime_error("Expected one owned fixture directory");
        const std::filesystem::path directory(argv[1]);
        axiom::Request request;
        request.operation = axiom::Operation::Prove;
        request.module.name = "parallel_runtime";
        request.module.version = "v1";
        request.module.symbols = {"premise", "goal", "unresolved"};
        request.module.rules = {{0, {}, "fixture-premise"}, {2, {0}, "fixture-rule"}};
        request.limits.events = 64;
        request.goal = 2;
        std::ofstream proved(directory / "proved.axrm", std::ios::binary);
        axiom::write_binary(proved, axiom::encode_request(request));
        request.goal = 4;
        std::ofstream unknown(directory / "unknown.axrm", std::ios::binary);
        axiom::write_binary(unknown, axiom::encode_request(request));
        request.operation = axiom::Operation::Evaluate;
        request.expression = {{axiom::Code::Constant, 1}};
        std::ofstream unsupported(directory / "unsupported.axrm", std::ios::binary);
        axiom::write_binary(unsupported, axiom::encode_request(request));
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
