#pragma once

#include "reasoning.hpp"

namespace axiom::knowledge {

struct ProofBatchLimits {
    std::size_t workers = 1, requests = 32;
    std::size_t input_bytes = 16 * 1024 * 1024;
    std::size_t output_bytes = 32 * 1024 * 1024;
    std::uint64_t search_events = 262144;
    std::uint32_t search_depth = 256;
};
struct ProofBatchReply {
    Bytes reply;
    NativeReplyCheck check;
    // Index of the first byte-identical input in this invocation.
    std::size_t producer = 0;
};
struct ProofBatchRun {
    std::vector<ProofBatchReply> replies;
    std::size_t executions = 0, memo_hits = 0, workers_started = 0;
    std::size_t peak_active_workers = 0, input_bytes = 0, output_bytes = 0;
    std::uint64_t reserved_search_events = 0;
};

// Independent immutable CPU finite-Horn requests only (AXRM v1 Prove/v2).
// One worker is the default; at most four workers and 64 inputs are admitted.
// Coalesce complete byte-identical requests before scheduling, including all
// provenance, goals and limits. Independently replay each produced reply.
// Preserve input order and exact native proof bytes regardless of scheduling.
// No journal/model/network access, live dependency lookup or truth assessment
// occurs here. The caller retains normal source/dependency revalidation.
//
// Wire storage and aggregate requested search events are reserved before work.
// output_bytes reserves the maximum native envelope per input, including
// duplicates. Native per-request limits remain unchanged. These are explicit
// allocation/work bounds, not an RSS measurement or a wall-time guarantee.
ProofBatchRun run_proof_batch(std::span<const Bytes> requests, ProofBatchLimits limits = {});

} // namespace axiom::knowledge
