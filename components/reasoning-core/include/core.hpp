#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <gmpxx.h>
#include <iosfwd>
#include <span>
#include <string>
#include <vector>

namespace axiom {

using Bytes = std::vector<std::uint8_t>;
using Rational = mpq_class;
using Matrix = std::vector<std::vector<Rational>>;
using RuleMatcher = std::function<std::vector<std::uint8_t>(
    const std::vector<std::uint64_t>&, std::uint64_t)>;
inline constexpr std::size_t max_wire_bytes = 1024 * 1024;
inline constexpr std::size_t max_text_bytes = 65536;
inline constexpr std::uint32_t no_step = UINT32_MAX;

enum class Operation : std::uint8_t { Evaluate = 1, Solve, Determinant, Linear, Born, Prove };
enum class Code : std::uint8_t { Constant = 1, Variable, Add, Subtract, Multiply, Divide, Negate };
enum class Status : std::uint8_t { Ok = 1, Infinite, Inconsistent, Proved, Disproved, Unknown, Contradiction };
enum class EventKind : std::uint8_t { Visit = 1, TryRule, Cached, Cycle, Backtrack, Derived, Bound };

struct Instruction {
    Code code = Code::Constant;
    Rational value = 0;
};
// Literal 2*n denotes symbol n; 2*n+1 is its explicit negation.
struct Rule {
    std::uint32_t head = 0;
    std::vector<std::uint32_t> premises;
    std::string provenance;
};
struct Module {
    std::string name;
    std::string version;
    std::vector<std::string> symbols;
    std::vector<Rule> rules;
};
struct Limits {
    std::uint32_t events = 4096;
    std::uint32_t depth = 128;
};
struct Request {
    Operation operation = Operation::Evaluate;
    std::vector<Instruction> expression;
    Matrix matrix;
    Matrix rhs;
    Module module;
    std::uint32_t goal = 0;
    Limits limits;
    bool gpu = false;
};
struct ProofStep {
    std::uint32_t literal = 0;
    std::uint32_t rule = 0;
    std::vector<std::uint32_t> dependencies;
};
struct SearchEvent {
    EventKind kind = EventKind::Visit;
    std::uint32_t literal = 0;
    std::uint32_t rule = no_step;
    std::uint32_t depth = 0;
};
struct Result {
    Operation operation = Operation::Evaluate;
    Status status = Status::Unknown;
    std::vector<Rational> values;
    // Linear: one row per nullspace basis vector; values is a particular solution.
    Matrix basis;
    Module module;
    std::uint32_t goal = 0;
    std::vector<ProofStep> proof;
    std::uint32_t positive_root = no_step;
    std::uint32_t negative_root = no_step;
    std::vector<SearchEvent> history;
    bool limited = false;
    bool gpu = false;
};

// Text bridges. The executor never parses English, module source, or expressions.
Module parse_module(const std::string& source);
Module load_module(const std::filesystem::path& path);
Request compile(const std::vector<std::string>& arguments);
Request compile_proof(Module module, const std::string& goal);
std::string render(const Result& decoded_result);
std::string help();

Bytes encode_request(const Request& request);
Request decode_request(std::span<const std::uint8_t> bytes);
Bytes encode_result(const Result& result);
Result decode_result(std::span<const std::uint8_t> bytes);
Bytes run_binary(std::span<const std::uint8_t> request, const RuleMatcher& gpu_matcher = {});
Bytes read_binary(std::istream& input);
void write_binary(std::ostream& output, const Bytes& bytes);

// Typed executor and independent certificate checker, also exposed for testing.
Result execute(const Request& request, const RuleMatcher& gpu_matcher = {});
bool verify_proof(const Module& module, const std::vector<ProofStep>& proof);

} // namespace axiom
