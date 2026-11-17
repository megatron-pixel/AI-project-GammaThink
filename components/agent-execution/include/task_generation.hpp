#pragma once

#include <cstdint>
#include "model_manager.hpp"

namespace axiom::runtime {

struct CodeGenerationOptions {
    std::uint32_t output_tokens = 512;
};
struct CodeRepairOptions {
    neural::ModelSpec model;
    CodeGenerationOptions generation;
};

} // namespace axiom::runtime
