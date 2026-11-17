#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace axiom {

struct GpuReport {
    std::string device;
    std::size_t rules_tested;
    double elapsed_ms;
    std::size_t total_vram_bytes;
    std::size_t free_vram_bytes;
    std::size_t explicit_buffer_bytes;
};

std::vector<std::uint8_t> gpu_rule_matches(
    const std::vector<std::uint64_t>& required_masks, std::uint64_t known_facts);
GpuReport test_gpu_rules();

}
