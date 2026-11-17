#include "binary_dispatch.hpp"
#include "relational.hpp"
#include "problem_wire.hpp"

#include <stdexcept>

namespace axiom {
namespace {
std::uint16_t version(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < 12 || bytes.size() > max_wire_bytes
        || bytes[0] != 'A' || bytes[1] != 'X' || bytes[2] != 'R' || bytes[3] != 'M') {
        throw std::runtime_error("Invalid binary protocol envelope");
    }
    return static_cast<std::uint16_t>(bytes[4] | (static_cast<std::uint16_t>(bytes[5]) << 8));
}
}

Bytes run_request_binary(std::span<const std::uint8_t> request, const RuleMatcher& gpu_matcher) {
    switch (version(request)) {
    case 1: return run_binary(request, gpu_matcher);
    case 2: return run_relational_binary(request);
    case 3: return run_problem_binary(request);
    default: throw std::runtime_error("Unsupported binary protocol version");
    }
}

std::string render_binary_result(std::span<const std::uint8_t> result) {
    switch (version(result)) {
    case 1: return render(decode_result(result));
    case 2: return render_relational(decode_relational_result(result));
    case 3: return render_problem(decode_problem_result(result));
    default: throw std::runtime_error("Unsupported binary protocol version");
    }
}
}
