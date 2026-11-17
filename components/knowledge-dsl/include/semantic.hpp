#pragma once

#include "translator.hpp"

#include <stdexcept>
#include <utility>
#include <cstdint>

namespace axiom {

struct IncompleteInterpretation : std::runtime_error {
    std::string response;
    explicit IncompleteInterpretation(std::string content)
        : std::runtime_error("Incomplete interpretation"), response(std::move(content)) {}
};

// Parsing checks the representation, not whether it preserves English meaning.
EnglishRequest parse_semantic_interpretation(const std::string& json);
void verify_local_interpreter_context(std::uint32_t expected_tokens);
std::string interpret_local_json(const std::string& question);
enum class ProposalOutputFormat { JsonSchema, LarkGrammar };
std::string interpret_local_proposal_json(const std::string& instructions,
    const std::string& input, const std::string& constraint, std::uint32_t max_tokens,
    ProposalOutputFormat format = ProposalOutputFormat::JsonSchema);
EnglishRequest interpret_local(const std::string& question);

}
