#pragma once

#include "journal.hpp"

#include <cstdint>
#include <span>

namespace axiom::knowledge {

struct NativeReplyCheck {
    bool valid = false;
    bool conclusive = false;
    bool conflict = false;
};

// Bind an existing AXRM v1/v2/v3 reply to its original request before reuse.
// "Valid" describes formal replay, not English fidelity or empirical truth.
// Unresolved/limited replies never become conclusive through this API.
// Invalid request/reply pairs are rejected; allocation failures are not hidden.
NativeReplyCheck check_native_reply(std::span<const std::uint8_t> request,
                                    std::span<const std::uint8_t> reply);

inline constexpr std::string_view native_request_format = "application/x-axiom-native-request";
inline constexpr std::string_view native_reply_format = "application/x-axiom-native-reply";
Object native_execution_record(const Object& goal, const ObjectId& request,
                                std::span<const std::uint8_t> reply);

enum class NativeStrategy { Existing = 1, Forward };
struct NativeRunLimits {
    std::size_t maximum_dependencies = maximum_items;
    std::size_t maximum_certificates = 64;
    NativeStrategy strategy = NativeStrategy::Existing;
    std::uint64_t forward_work = 1000000;
};
struct NativeRun {
    NativeReplyCheck check;
    Bytes reply;
    std::optional<ObjectId> certificate;
    bool reused = false;
    // Producer work only; cached replies do not claim a new search occurred.
    std::optional<std::uint64_t> producer_work;
    std::vector<ObjectId> unavailable, rejected_certificates;
};

// Formal/internal compiled-request entry point, not an English interpreter or
// an alternative archive meaning schema. Normal translation retains its DSL
// and shared JSON upstream. Results remain scoped to the complete input.
// Native certificates keep the original ordered proof/assumption/source data.
// This is execution-record reuse; CoK per-inference graph construction is
// separate. Every hit replays the checker and rechecks current dependencies.
NativeRun run_native(Journal& journal, const ObjectId& request,
                     NativeRunLimits limits = {});

} // namespace axiom::knowledge
