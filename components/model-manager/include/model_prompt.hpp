#pragma once

#include "knowledge.hpp"

#include <memory>
#include <ctime>

namespace axiom::neural {

struct PromptPart {
    std::string text;
    bool input = false;
};

// A host-selected, hash-bound model template, not a template supplied by a user
// message. Input-origin marking survives rendering for safe special-token parsing.
class ModelPrompt {
public:
    ModelPrompt(std::string source, std::string bos, std::string eos,
                bool add_bos, bool add_eos, bool enable_thinking = false,
                std::time_t captured_time = 0, std::string trusted_system = {});
    ~ModelPrompt();
    ModelPrompt(const ModelPrompt&) = delete;
    ModelPrompt& operator=(const ModelPrompt&) = delete;
    std::vector<PromptPart> render(std::string_view input, std::size_t maximum_bytes) const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace axiom::neural
