#include "parallel_reasoning.hpp"

#include "binary_dispatch.hpp"
#include "relational.hpp"

#include <algorithm>
#include <atomic>
#include <exception>
#include <map>
#include <thread>

namespace axiom::knowledge {
namespace {
Limits request_limits(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < 12 || bytes.size() > max_wire_bytes ||
        bytes[0] != 'A' || bytes[1] != 'X' || bytes[2] != 'R' || bytes[3] != 'M') {
        throw Error(ErrorCode::Schema, "Invalid parallel proof request envelope");
    }
    const auto version = static_cast<std::uint16_t>(bytes[4] |
        (static_cast<std::uint16_t>(bytes[5]) << 8));
    Limits limits;
    bool gpu = false;
    try {
        if (version == 1) {
            const auto request = decode_request(bytes);
            if (request.operation != Operation::Prove) {
                throw Error(ErrorCode::Schema, "Parallel proof requires finite-Horn requests");
            }
            limits = request.limits;
            gpu = request.gpu;
        } else if (version == 2) {
            const auto request = decode_relational_request(bytes);
            limits = request.limits;
            gpu = request.gpu;
        } else {
            throw Error(ErrorCode::Schema, "Unsupported parallel proof request version");
        }
    } catch (const std::runtime_error& error) {
        throw Error(ErrorCode::Schema, error.what());
    }
    if (gpu) throw Error(ErrorCode::Permission, "Parallel proof workers are CPU-only");
    return limits;
}
} // namespace

ProofBatchRun run_proof_batch(std::span<const Bytes> requests, ProofBatchLimits limits) {
    if (limits.workers == 0 || limits.workers > 4 || limits.requests == 0 ||
        limits.requests > 64 || requests.size() > limits.requests ||
        limits.input_bytes == 0 || limits.input_bytes > 64 * max_wire_bytes ||
        limits.output_bytes == 0 || limits.output_bytes > 64 * max_wire_bytes ||
        limits.search_events == 0 || limits.search_events > 1048576 ||
        limits.search_depth == 0 || limits.search_depth > 256) {
        throw Error(ErrorCode::Budget, "Invalid parallel proof batch limits");
    }
    if (requests.size() > limits.output_bytes / max_wire_bytes) {
        throw Error(ErrorCode::Budget, "Parallel proof output reservation exceeds budget");
    }
    ProofBatchRun run;
    std::map<Bytes, std::size_t> producers;
    std::vector<std::size_t> unique, positions;
    for (std::size_t index = 0; index < requests.size(); ++index) {
        const auto& request = requests[index];
        if (request.size() > limits.input_bytes - run.input_bytes) {
            throw Error(ErrorCode::Budget, "Parallel proof input exceeds budget");
        }
        run.input_bytes += request.size();
        const auto [entry, inserted] = producers.emplace(request, unique.size());
        positions.push_back(entry->second);
        if (!inserted) continue;
        const auto reserved = request_limits(request);
        if (reserved.events > limits.search_events - run.reserved_search_events ||
            reserved.depth > limits.search_depth) {
            throw Error(ErrorCode::Budget, "Parallel proof search reservation exceeds budget");
        }
        run.reserved_search_events += reserved.events;
        unique.push_back(index);
    }
    run.executions = unique.size();
    run.memo_hits = requests.size() - unique.size();
    run.workers_started = std::min(limits.workers, unique.size());
    std::vector<ProofBatchReply> computed(unique.size());
    std::vector<std::exception_ptr> failures(unique.size());
    std::atomic<std::size_t> next{0}, active{0}, peak{0};
    std::vector<std::jthread> workers;
    for (std::size_t worker = 0; worker < run.workers_started; ++worker) {
        workers.emplace_back([&] {
            for (;;) {
                const auto slot = next.fetch_add(1, std::memory_order_relaxed);
                if (slot >= unique.size()) return;
                const auto running = active.fetch_add(1, std::memory_order_relaxed) + 1;
                auto observed = peak.load(std::memory_order_relaxed);
                while (observed < running && !peak.compare_exchange_weak(
                    observed, running, std::memory_order_relaxed)) {}
                try {
                    const auto index = unique[slot];
                    auto& result = computed[slot];
                    result.producer = index;
                    result.reply = run_request_binary(requests[index]);
                    if (result.reply.size() > max_wire_bytes) {
                        throw Error(ErrorCode::Budget, "Native proof reply exceeds envelope bound");
                    }
                    result.check = check_native_reply(requests[index], result.reply);
                    if (!result.check.valid) {
                        throw Error(ErrorCode::Integrity, "Parallel proof failed independent replay");
                    }
                } catch (...) {
                    // Transport the original exception to the owner thread.
                    // No worker failure is converted into a proof outcome.
                    failures[slot] = std::current_exception();
                }
                active.fetch_sub(1, std::memory_order_relaxed);
            }
        });
    }
    workers.clear(); // Join all producers before reading their distinct slots.
    run.peak_active_workers = peak.load(std::memory_order_relaxed);
    for (const auto& failure : failures) if (failure) std::rethrow_exception(failure);
    run.replies.reserve(requests.size());
    for (const auto position : positions) {
        run.output_bytes += computed[position].reply.size();
        run.replies.push_back(computed[position]);
    }
    return run;
}

} // namespace axiom::knowledge
