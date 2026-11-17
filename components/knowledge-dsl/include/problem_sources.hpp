#pragma once

#include "problem.hpp"

#include <optional>
#include <string_view>

namespace axiom {

struct ProblemSourceEntry {
    EvidenceSpan span;
    std::uint32_t region = 0;
    bool whole_region = false;
    bool operator==(const ProblemSourceEntry&) const = default;
};

// Owns an immutable context snapshot. Model-visible handles never encode values,
// guessed quote positions, or native proposal indices.
class ProblemSourceCatalog {
public:
    explicit ProblemSourceCatalog(ProblemContext context, ProblemLimits limits = {});
    const ProblemContext& context() const { return context_; }
    const ProblemLimits& limits() const { return limits_; }
    const std::string& snapshot() const { return snapshot_; }
    const std::string& input() const { return input_; }
    const std::vector<ProblemSourceEntry>& entries() const { return entries_; }

    EvidenceSpan resolve(std::string_view snapshot, std::string_view first,
                         std::optional<std::string_view> last = {}) const;
    std::uint32_t unit(std::string_view snapshot, std::string_view handle) const;
    static std::string source_handle(std::size_t index);

private:
    ProblemContext context_;
    ProblemLimits limits_;
    std::string snapshot_, input_;
    std::vector<ProblemSourceEntry> entries_;
};

} // namespace axiom
