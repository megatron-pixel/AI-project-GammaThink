#pragma once

#include "conversation.hpp"

namespace axiom::context {

struct SummaryMaintenanceLimits {
    std::size_t cadence_messages = 32, recent_messages = 64, constraints = 64;
    std::size_t source_bytes = 262144, summary_bytes = 16384;
};

// Deterministic extractive maintenance, without a neural model or fact
// inference. Retains exact whole recent-message quotes and an initial-message
// quote when it fits (otherwise an explicit hash/ID reference, never a clip),
// role/hash/ID attribution, latest pin revisions (including withdrawals),
// explicit omissions and the previous summary's version/hash reference.
// Originals and earlier summary revisions are never modified.
//
// Append when the cadence is reached, or when an automatically maintained
// summary's pin-state digest changes. Existing manually authored summaries
// remain untouched until the declared cadence is due. A resource failure is
// explicit; no truncated or stale replacement is silently stored.
// Reads one SQLite snapshot, then uses the existing summary version CAS.
bool refresh_summary(ConversationMemory& memory, std::string_view session,
                     SummaryMaintenanceLimits limits = {});

} // namespace axiom::context
