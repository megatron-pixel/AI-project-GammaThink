#pragma once

#include "core.hpp"

namespace axiom {

enum class TermKind : std::uint8_t { Constant = 1, Variable = 2 };
struct Term {
    TermKind kind = TermKind::Constant;
    std::uint32_t index = 0;
    bool operator==(const Term&) const = default;
};
struct Predicate {
    std::string name;
    std::uint32_t arity = 0;
    bool operator==(const Predicate&) const = default;
};
struct Atom {
    std::uint32_t predicate = 0;
    bool negative = false;
    std::vector<Term> terms;
    bool operator==(const Atom&) const = default;
};
struct RelationalRule {
    std::uint32_t variables = 0;
    Atom head;
    std::vector<Atom> premises;
    std::string provenance;
    bool operator==(const RelationalRule&) const = default;
};
struct RelationalModule {
    std::string name;
    std::string version;
    std::vector<std::string> constants;
    std::vector<Predicate> predicates;
    std::vector<RelationalRule> rules;
    bool operator==(const RelationalModule&) const = default;
};
struct RelationalRequest {
    RelationalModule module;
    Atom goal;
    Limits limits;
    bool gpu = false;
};
struct GroundInstance {
    std::uint32_t source_rule = 0;
    std::vector<std::uint32_t> substitution;
    bool operator==(const GroundInstance&) const = default;
};
struct Grounding {
    Request request;
    std::vector<GroundInstance> instances;
};
struct RelationalResult {
    RelationalRequest request;
    std::vector<GroundInstance> instances;
    Result proof;
};

RelationalModule parse_relational_module(const std::string& source);
RelationalModule load_relational_module(const std::filesystem::path& path);
Atom parse_relational_goal(const RelationalModule& module, const std::string& goal);
RelationalRequest compile_relational(RelationalModule module, const std::string& goal,
                                     Limits limits = {}, bool gpu = false);
Grounding ground_relational(const RelationalRequest& request);
Bytes encode_relational_request(const RelationalRequest& request);
RelationalRequest decode_relational_request(std::span<const std::uint8_t> bytes);
Bytes encode_relational_result(const RelationalResult& result);
RelationalResult decode_relational_result(std::span<const std::uint8_t> bytes);
RelationalResult execute_relational(const RelationalRequest& request);
bool verify_relational_result(const RelationalRequest& request, const RelationalResult& result);
std::string render_relational(const RelationalResult& result);
Bytes run_relational_binary(std::span<const std::uint8_t> request);

} // namespace axiom
