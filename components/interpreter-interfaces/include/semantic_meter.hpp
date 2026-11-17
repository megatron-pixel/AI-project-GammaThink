#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace axiom {

struct InterpretationLimit : std::runtime_error { using std::runtime_error::runtime_error; };
struct InterpretationAccountingError : std::runtime_error { using std::runtime_error::runtime_error; };

struct SemanticTokenUsage {
    std::uint64_t prompt = 0, completion = 0, total = 0, cached_prompt = 0;
};
struct SemanticHttpExchange {
    std::string path, request, response;
    long status = 0;
    std::int64_t elapsed_ms = 0;
};
struct MeteredSemanticCompletion {
    std::string content, finish_reason, model;
    SemanticTokenUsage usage;
    std::uint32_t preflight_prompt_tokens = 0, requested_output_tokens = 0;
};
struct SemanticPromptPreflight {
    std::string request;
    std::uint32_t prompt_tokens = 0, requested_output_tokens = 0;
    bool within_limits = false;
};
enum class SemanticSamplingProfile { GreedyControl, QwenNonThinking };
enum class SemanticBudgetProfile { Standard, QuantityDecisionsExperimental };

// Construct before catalog creation. The same session and deadline cover both
// stages and final compilation/execution; call ensure_time() after local work.
// Errors close the session: there are no implicit retries or budget resets.
class MeteredSemanticSession {
public:
    using Clock = std::chrono::steady_clock;
    explicit MeteredSemanticSession(bool staged = false, std::uint16_t port = 18089,
        Clock::time_point start = Clock::now(),
        SemanticSamplingProfile sampling = SemanticSamplingProfile::GreedyControl,
        SemanticBudgetProfile budget = SemanticBudgetProfile::Standard);
    MeteredSemanticSession(const MeteredSemanticSession&) = delete;
    MeteredSemanticSession& operator=(const MeteredSemanticSession&) = delete;
    MeteredSemanticSession(MeteredSemanticSession&&) = delete;
    MeteredSemanticSession& operator=(MeteredSemanticSession&&) = delete;
    MeteredSemanticCompletion complete(const std::string& instructions,
        const std::string& input, const std::string& schema,
        std::optional<std::uint32_t> lower_output_limit = {});
    // A fresh-session, terminal diagnostic. Never sends a generation request.
    SemanticPromptPreflight preflight(const std::string& instructions,
        const std::string& input, const std::string& schema);
    void ensure_time() const;
    std::int64_t remaining_ms() const;
    std::int64_t elapsed_ms() const;
    std::uint64_t prompt_tokens() const { return prompt_tokens_; }
    std::uint64_t completion_tokens() const { return completion_tokens_; }
    std::uint64_t cached_prompt_tokens() const { return cached_prompt_tokens_; }
    std::uint32_t calls() const { return calls_; }
    std::size_t response_bytes() const { return response_bytes_; }
    bool accounting_complete() const { return accounting_complete_; }
    bool staged() const { return staged_; }
    SemanticSamplingProfile sampling_profile() const { return sampling_; }
    SemanticBudgetProfile budget_profile() const { return budget_; }
    std::uint32_t prompt_limit() const {
        return budget_ == SemanticBudgetProfile::QuantityDecisionsExperimental ? 65536U : 6144U;
    }
    std::uint32_t completion_limit() const {
        return budget_ == SemanticBudgetProfile::QuantityDecisionsExperimental ? 2048U : 1536U;
    }
    std::uint32_t call_limit() const {
        return budget_ == SemanticBudgetProfile::QuantityDecisionsExperimental ? 64U : staged_ ? 2U : 1U;
    }
    const std::vector<SemanticHttpExchange>& exchanges() const { return exchanges_; }
private:
    bool staged_, closed_ = false, accounting_complete_ = true;
    SemanticSamplingProfile sampling_;
    SemanticBudgetProfile budget_;
    std::uint16_t port_;
    Clock::time_point deadline_;
    std::uint64_t prompt_tokens_ = 0, completion_tokens_ = 0, cached_prompt_tokens_ = 0;
    std::uint32_t calls_ = 0;
    std::size_t response_bytes_ = 0;
    std::vector<SemanticHttpExchange> exchanges_;
    SemanticPromptPreflight prepare(const std::string& instructions,
        const std::string& input, const std::string& schema,
        std::optional<std::uint32_t> lower_output_limit);
    std::string post(const std::string& path, const std::string& payload);
};

} // namespace axiom
