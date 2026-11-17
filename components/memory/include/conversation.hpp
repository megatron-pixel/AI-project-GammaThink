#pragma once

#include "knowledge.hpp"

#include <filesystem>
#include <functional>
#include <memory>

struct sqlite3;

namespace axiom::context {

enum class Role : std::uint8_t { User = 1, Assistant, System };
struct Message {
    std::uint64_t id = 0;
    std::string session, timestamp, content;
    Role role = Role::User;
    knowledge::Digest content_hash{};
    bool operator==(const Message&) const = default;
};
struct MessagePage {
    std::vector<Message> messages;
    bool complete = true;
    std::optional<std::uint64_t> next;
};
struct Summary {
    std::uint64_t version = 0, first_message = 0, last_message = 0;
    std::string content;
    bool operator==(const Summary&) const = default;
};
struct Constraint {
    std::string key, content;
    std::uint64_t revision = 0, source_message = 0;
    bool active = true;
    bool operator==(const Constraint&) const = default;
};
struct ConversationLimits {
    std::uint32_t cache_kib = 4096;
    std::uint64_t sql_instructions = 1000000;
};

// Separate from the CoK journal. One owner thread per connection.
// Original messages and derived revisions are append-only.
class ConversationMemory {
public:
    explicit ConversationMemory(const std::filesystem::path& path,
                                ConversationLimits limits = {});
    ~ConversationMemory();
    ConversationMemory(const ConversationMemory&) = delete;
    ConversationMemory& operator=(const ConversationMemory&) = delete;

    void open_session(std::string_view session, std::string_view project);
    std::string project(std::string_view session);
    Message append(std::string_view session, Role role, std::string_view timestamp,
                   std::string_view content);
    // Atomic compare-and-append: a later turn or changed active correction
    // cannot receive a stale assistant response. Pins use constraints() order.
    Message append_response(const Message& original, std::span<const Constraint> expected_pins,
                            std::string_view timestamp, std::string_view content);
    std::optional<Message> get(std::string_view session, std::uint64_t message_id);
    // Pins a read-only view before calling read. Writes/nested snapshots on
    // this connection are rejected; independent WAL writers may continue.
    void read_snapshot(const std::function<void()>& read);
    MessagePage recent(std::string_view session, std::size_t limit = 32,
                       std::size_t content_bytes = 262144);
    // Exact original records, including every role, in ascending ID order.
    // Bounded keyset pages support reconstruction without a search phrase.
    MessagePage messages_after(std::string_view session, std::uint64_t after = 0,
                               std::size_t limit = 32, std::size_t content_bytes = 262144);
    // Literal unicode61 phrase, never an FTS expression. Session-scoped,
    // ascending message IDs with keyset pagination; no relevance claim.
    MessagePage search(std::string_view session, std::string_view phrase,
                       std::uint64_t after = 0, std::size_t limit = 32,
                       std::size_t content_bytes = 262144);
    // Source ranges must be in this session. expected_version prevents a
    // stale writer from replacing a newer summary. Automatic maintenance uses
    // the separate bounded, source-extractive summary policy.
    Summary save_summary(std::string_view session, Summary summary,
                         std::uint64_t expected_version);
    std::optional<Summary> summary(std::string_view session, std::uint64_t version = 0);
    // Caller explicitly selects a correction; source must be a user message
    // in the same session. Inactive revisions retain the withdrawal history.
    Constraint revise_constraint(std::string_view session, Constraint constraint,
                                 std::uint64_t expected_revision);
    std::vector<Constraint> constraints(std::string_view session, std::size_t limit = 64);
    // Latest revisions including withdrawals, not only currently active pins.
    std::vector<Constraint> constraint_states(std::string_view session, std::size_t limit = 64);
    std::optional<Constraint> constraint(std::string_view session, std::string_view key,
                                       std::uint64_t revision = 0);

private:
    struct Close { void operator()(sqlite3*) const noexcept; };
    std::unique_ptr<sqlite3, Close> database_;
    ConversationLimits limits_;
};

} // namespace axiom::context
