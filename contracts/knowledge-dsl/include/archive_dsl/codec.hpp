#pragma once

#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>

namespace gammathink::archive_dsl {
inline constexpr std::size_t max_bytes = 1048576;
inline constexpr std::size_t max_records = 4096;
inline constexpr std::size_t max_depth = 64;

struct Error : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Text APIs retain number tokens exactly; no native semantic admission occurs.
std::string encode(std::string_view input_json);
std::string decode(std::string_view input_dsl);

// Explicit version-2 channel: the same shared profile with a ninth theory
// array, quantified_equations. Legacy APIs remain strictly version 1.
// This preserves declarations; native binding/admission is a separate check.
std::string encode_v2(std::string_view input_json);
std::string decode_v2(std::string_view input_dsl);
} // namespace gammathink::archive_dsl
