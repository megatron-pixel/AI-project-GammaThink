#pragma once

#include "knowledge.hpp"

#include <atomic>
#include <memory>

namespace axiom::knowledge {

struct ReadHttpStats {
    std::uint64_t requests = 0;
    std::size_t accepted_bytes = 0;
    // Full body allowances charged before POST, including failed attempts.
    std::size_t query_bytes = 0;
};
struct ReadHttpBudgetLimits {
    std::uint64_t requests = 128;
    std::size_t total_bytes = 64 * 1024 * 1024;
    std::size_t query_bytes = 8 * 1024 * 1024;
};
// Shared across owner-thread transports/views. These count accepted body
// bytes, not wire traffic. New views cannot replenish this explicit budget.
class ReadHttpBudget {
public:
    explicit ReadHttpBudget(ReadHttpBudgetLimits limits = {});
    const ReadHttpStats& stats() const { return stats_; }
private:
    friend class ReadHttp;
    ReadHttpBudgetLimits limits_;
    ReadHttpStats stats_;
};
struct ReadHttpOptions {
    std::string origin;
    bool network = false;
    bool allow_insecure_loopback = false;
    long connect_timeout_ms = 2000;
    long timeout_ms = 5000;
    std::size_t total_bytes = 64 * 1024 * 1024;
    std::size_t header_bytes = 65536;
    std::uint64_t requests = 128;
    std::size_t query_bytes = 8 * 1024 * 1024;
    // Host-owned, never loaded from a remote catalogue or model proposal.
    // A gate can revoke existing transports; callbacks abort cooperatively.
    std::shared_ptr<const std::atomic<bool>> network_gate;
    std::shared_ptr<ReadHttpBudget> budget;
};
// Owner-thread transport for raw block GET and explicitly enabled query POST.
// No write/publish API, redirects, ambient proxies, credentials or retries.
class ReadHttp {
public:
    explicit ReadHttp(ReadHttpOptions options);
    Bytes raw_get(std::string_view target, std::size_t maximum_bytes);
    Bytes query_post(std::string_view target, std::span<const std::uint8_t> body, std::size_t maximum_bytes);
    const ReadHttpStats& stats() const { return stats_; }
private:
    ReadHttpOptions options_;
    ReadHttpStats stats_;
    Bytes perform(std::string_view target, bool post, std::span<const std::uint8_t> body, std::size_t maximum_bytes);
};

} // namespace axiom::knowledge
