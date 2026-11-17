#pragma once

#include "core.hpp"
#include "library.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace axiom {

enum class EnglishKind { Calculation, Proof, Research, LibraryCalculation };

struct EnglishRequest {
    EnglishKind kind = EnglishKind::Calculation;
    std::vector<std::string> command;
    std::string goal;
    std::string query;
    std::string quantity;
    std::string unit;
    std::string expression;
};

struct QuantityPremise {
    Rational value;
    Evidence source;
    std::size_t offset = 0;
    std::size_t length = 0;
};

enum class GroundingStatus { Unknown, Conditional, Conflict };

struct GroundedCalculation {
    GroundingStatus status = GroundingStatus::Unknown;
    std::vector<QuantityPremise> premises;
    Bytes binary_result;
    bool coverage_complete = false;
};

EnglishRequest translate_english(const std::string& question);
GroundedCalculation calculate_from_library(const EnglishRequest& request,
                                           const LibrarySearch& search);
std::string translator_help();

}
