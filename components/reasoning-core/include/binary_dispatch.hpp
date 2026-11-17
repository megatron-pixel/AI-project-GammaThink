#pragma once

#include "core.hpp"

namespace axiom {
Bytes run_request_binary(std::span<const std::uint8_t> request, const RuleMatcher& gpu_matcher = {});
std::string render_binary_result(std::span<const std::uint8_t> result);
}
