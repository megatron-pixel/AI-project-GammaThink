#pragma once

#include "problem.hpp"

namespace axiom {

// AXRM v3: the existing 12-byte little-endian envelope, kinds 1/2.
// Payload fields follow the native declaration order; variant tags are 1-based.
// Rationals use the v1 canonical sign/LE-magnitude representation (4096 bits).
Bytes encode_problem_request(const ProblemRequest& request);
ProblemRequest decode_problem_request(std::span<const std::uint8_t> bytes);
Bytes encode_problem_result(const ProblemResult& result);
ProblemResult decode_problem_result(std::span<const std::uint8_t> bytes);
// Checks formal replay, not the truth or interpretation of source evidence.
std::string render_problem(const ProblemResult& result);
Bytes run_problem_binary(std::span<const std::uint8_t> request);

} // namespace axiom
