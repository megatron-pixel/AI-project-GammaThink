#include "conversation.hpp"
#include "journal.hpp"

#include <iostream>
#include <sqlite3.h>
#include <stdlib.h>
#include <unistd.h>

namespace {
using namespace axiom::context;
using axiom::knowledge::Error;
using axiom::knowledge::ErrorCode;
std::size_t checks = 0;

void check(bool value) {
    ++checks;
    if (!value) throw std::runtime_error("Conversation assertion " + std::to_string(checks));
}
template<class F> void rejects(ErrorCode expected, F&& operation) {
    ++checks;
    try { operation(); }
    catch (const Error& error) {
        if (error.code == expected) return;
        throw;
    }
    throw std::runtime_error("Expected conversation rejection " + std::to_string(checks));
}
struct Directory {
    std::filesystem::path path;
    Directory() {
        auto pattern = (std::filesystem::temp_directory_path() / "gammathink-context-XXXXXX").string();
        std::vector<char> name(pattern.begin(), pattern.end());
        name.push_back(0);
        const auto* created = mkdtemp(name.data());
        if (!created) throw std::runtime_error("Cannot create conversation test directory");
        path = created;
    }
    ~Directory() { std::filesystem::remove_all(path); }
};
int raw_sql(const std::filesystem::path& path, const char* sql) {
    sqlite3* raw = nullptr;
    const auto opened = sqlite3_open(path.c_str(), &raw);
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(raw, sqlite3_close);
    if (opened != SQLITE_OK) throw std::runtime_error("Cannot open raw fixture");
    return sqlite3_exec(raw, sql, nullptr, nullptr, nullptr);
}

void persistence() {
    Directory directory;
    const auto path = directory.path / "conversation.sqlite";
    Message a, b, c;
    Summary original;
    Constraint pin;
    {
        ConversationMemory memory(path);
        memory.open_session("session-a", "project");
        memory.open_session("session-a", "project");
        memory.open_session("session-b", "project");
        check(memory.recent("session-a").messages.empty());
        rejects(ErrorCode::Schema, [&] { memory.open_session("session-a", "different"); });
        rejects(ErrorCode::Missing, [&] { memory.append("missing", Role::User, "t", "text"); });
        a = memory.append("session-a", Role::User, "2026-11-14T10:00:00Z", "Use one billion parameters");
        b = memory.append("session-a", Role::Assistant, "2026-11-14T10:00:01Z", "Recorded");
        c = memory.append("session-a", Role::User, "2026-11-14T10:00:02Z", "Use three billion parameters");
        const auto other = memory.append("session-b", Role::User, "t", "Other project context");
        check(a.id < b.id && b.id < c.id && c.id < other.id);
        const auto recent = memory.recent("session-a", 2);
        check(!recent.complete && !recent.next && recent.messages == std::vector<Message>{b, c});
        check(memory.recent("session-a").messages == std::vector<Message>{a, b, c});
        check(memory.recent("session-a").complete);
        check(!memory.summary("session-a"));
        original = memory.save_summary("session-a", {0, a.id, b.id, "Original requirement"}, 0);
        check(original.version == 1 && memory.summary("session-a") == original);
        pin = memory.revise_constraint("session-a", {"size", a.content, 0, a.id, true}, 0);
        check(pin.revision == 1 && memory.constraints("session-a") == std::vector<Constraint>{pin});
        const auto updated = memory.revise_constraint("session-a", {"size", c.content, 0, c.id, true}, 1);
        check(updated.revision == 2 && updated.source_message == c.id);
        check(memory.constraint("session-a", "size", 1) == pin);
        check(memory.constraints("session-a") == std::vector<Constraint>{updated});
        rejects(ErrorCode::Integrity, [&] {
            memory.revise_constraint("session-a", {"size", "stale", 0, a.id, true}, 2);
        });
        rejects(ErrorCode::Integrity, [&] {
            memory.revise_constraint("session-a", {"size", "stale writer", 0, c.id, true}, 1);
        });
        rejects(ErrorCode::Schema, [&] {
            memory.revise_constraint("session-a", {"assistant", "not a user correction", 0, b.id, true}, 0);
        });
        rejects(ErrorCode::Missing, [&] {
            memory.revise_constraint("session-a", {"cross-session", "no", 0, other.id, true}, 0);
        });
        rejects(ErrorCode::Missing, [&] {
            memory.save_summary("session-a", {0, a.id, other.id, "Cross-session source"}, 1);
        });
        rejects(ErrorCode::Integrity, [&] {
            memory.save_summary("session-a", {0, a.id, c.id, "Stale summary"}, 0);
        });
        const auto next = memory.save_summary("session-a", {0, a.id, c.id, "Revised summary"}, 1);
        check(next.version == 2 && memory.summary("session-a", 1) == original);
        check(memory.constraint("session-a", "size")->content == c.content);
        // Summary persistence cannot rewrite or deactivate a pinned correction.
        check(memory.recent("session-a").messages.front() == a);
    }
    {
        ConversationMemory memory(path);
        check(memory.recent("session-a").messages == std::vector<Message>{a, b, c});
        check(memory.summary("session-a")->version == 2 && memory.summary("session-a", 1) == original);
        check(memory.constraint("session-a", "size")->source_message == c.id);
        check(memory.constraint("session-a", "size", 1) == pin);
        const auto withdrawn = memory.revise_constraint("session-a", {"size", "Withdrawn", 0, c.id, false}, 2);
        check(withdrawn.revision == 3 && memory.constraints("session-a").empty());
        check(memory.constraint("session-a", "size")->active == false);
        check(memory.constraint("session-a", "size", 2)->active);
        check(!memory.summary("session-a", 99) && !memory.constraint("session-a", "absent"));
        check(raw_sql(path, "UPDATE messages SET content='rewritten'") == SQLITE_CONSTRAINT);
        check(raw_sql(path, "DELETE FROM messages") == SQLITE_CONSTRAINT);
        check(memory.recent("session-a").messages.front() == a);
    }
}
void search_and_limits() {
    Directory directory;
    const auto path = directory.path / "search.sqlite";
    ConversationMemory memory(path);
    memory.open_session("' OR 1=1 --", "project");
    memory.open_session("other", "project");
    const auto a = memory.append("' OR 1=1 --", Role::User, "t", "alpha beta");
    memory.append("' OR 1=1 --", Role::User, "t", "alpha something beta");
    const auto b = memory.append("' OR 1=1 --", Role::User, "t", "alpha beta");
    const auto literal = memory.append("' OR 1=1 --", Role::User, "t", "alpha OR beta");
    memory.append("other", Role::User, "t", "alpha beta");
    const auto first = memory.search("' OR 1=1 --", "alpha beta", 0, 1);
    check(first.messages == std::vector<Message>{a} && !first.complete && first.next == a.id);
    const auto second = memory.search("' OR 1=1 --", "alpha beta", *first.next, 1);
    check(second.messages == std::vector<Message>{b} && second.complete && !second.next);
    check(memory.search("' OR 1=1 --", "alpha OR beta").messages == std::vector<Message>{literal});
    check(memory.search("' OR 1=1 --", "\" OR missing").messages.empty());
    check(memory.search("' OR 1=1 --", "alpha beta", b.id).messages.empty());
    check(memory.search("' OR 1=1 --", "alpha beta", 0, 1, a.content.size()).messages.size() == 1);
    rejects(ErrorCode::Budget, [&] { memory.search("' OR 1=1 --", "alpha beta", 0, 1, 1); });
    rejects(ErrorCode::Budget, [&] { memory.recent("' OR 1=1 --", 32, 1); });
    rejects(ErrorCode::Budget, [&] { memory.recent("' OR 1=1 --", 0); });
    rejects(ErrorCode::Budget, [&] { memory.search("' OR 1=1 --", "alpha", 0, 4097); });
    rejects(ErrorCode::Schema, [&] { memory.search("' OR 1=1 --", ""); });
    rejects(ErrorCode::Schema, [&] { memory.append("other", static_cast<Role>(99), "t", "no"); });
    rejects(ErrorCode::Schema, [&] { memory.append("other", Role::User, "t", ""); });
    rejects(ErrorCode::Schema, [&] { memory.append("other", Role::User, "t", std::string("a\0b", 3)); });
    rejects(ErrorCode::Budget, [&] {
        memory.append("other", Role::User, "t", std::string(65537, 'a'));
    });
    memory.revise_constraint("other", {"a", "pin a", 0, memory.recent("other").messages.front().id, true}, 0);
    memory.revise_constraint("other", {"b", "pin b", 0, memory.recent("other").messages.front().id, true}, 0);
    rejects(ErrorCode::Budget, [&] { memory.constraints("other", 1); });
    check(memory.constraints("other", 2).size() == 2);
    ConversationMemory limited(path, {4096, 1});
    rejects(ErrorCode::Budget, [&] { limited.append("other", Role::User, "t", "budget rejected"); });
    rejects(ErrorCode::Budget, [&] { limited.recent("other"); });
    check(memory.search("other", "budget rejected").messages.empty());
    const auto count = memory.recent("other").messages.size();
    rejects(ErrorCode::Budget, [&] {
        limited.revise_constraint("other", {"a", "budget rejected", 0,
            memory.recent("other").messages.front().id, true}, 1);
    });
    check(memory.constraint("other", "a")->revision == 1 && memory.recent("other").messages.size() == count);
}
void concurrent_writers_and_separation() {
    Directory directory;
    const auto path = directory.path / "conversations.sqlite";
    ConversationMemory first(path);
    first.open_session("session", "project");
    const auto source = first.append("session", Role::User, "t", "Original source");
    ConversationMemory second(path);
    const auto correction = second.append("session", Role::User, "t", "New correction");
    first.save_summary("session", {0, source.id, source.id, "First"}, 0);
    rejects(ErrorCode::Integrity, [&] {
        second.save_summary("session", {0, source.id, correction.id, "Stale"}, 0);
    });
    second.save_summary("session", {0, source.id, correction.id, "Second"}, 1);
    check(first.summary("session")->version == 2);
    first.revise_constraint("session", {"key", "Original", 0, source.id, true}, 0);
    second.revise_constraint("session", {"key", "New", 0, correction.id, true}, 1);
    rejects(ErrorCode::Integrity, [&] {
        first.revise_constraint("session", {"key", "Stale", 0, correction.id, true}, 1);
    });
    check(first.constraint("session", "key")->content == "New");
    const auto journal_path = directory.path / "knowledge.sqlite";
    axiom::knowledge::Journal journal(journal_path);
    rejects(ErrorCode::Schema, [&] { ConversationMemory not_context(journal_path); });
    rejects(ErrorCode::Schema, [&] { axiom::knowledge::Journal not_knowledge(path); });
    const auto unrelated = directory.path / "unrelated.sqlite";
    check(raw_sql(unrelated, "CREATE TABLE keep(value)") == SQLITE_OK);
    rejects(ErrorCode::Schema, [&] { ConversationMemory not_context(unrelated); });
    check(raw_sql(unrelated, "INSERT INTO keep VALUES('preserved')") == SQLITE_OK);
    check(raw_sql(path, "PRAGMA user_version=99") == SQLITE_OK);
    rejects(ErrorCode::Schema, [&] { ConversationMemory future(path); });
}
void guarded_responses() {
    Directory directory;
    const auto path = directory.path / "responses.sqlite";
    ConversationMemory memory(path);
    memory.open_session("responses", "project");
    const auto user = memory.append("responses", Role::User, "1", "fixture-request");
    memory.revise_constraint("responses", {"network", "offline", 0, user.id, true}, 0);
    const auto pins = memory.constraints("responses");
    auto changed = user; changed.content_hash[0] ^= 1;
    rejects(ErrorCode::Integrity, [&] { memory.append_response(changed, pins, "2", "{\"nonce\":42}"); });
    check(memory.recent("responses").messages == std::vector<Message>{user});
    ConversationMemory writer(path);
    writer.revise_constraint("responses", {"network", "local-only", 0, user.id, true}, 1);
    rejects(ErrorCode::Integrity, [&] { memory.append_response(user, pins, "2", "{\"nonce\":42}"); });
    check(memory.recent("responses").messages.size() == 1);
    const auto revised = memory.constraints("responses");
    writer.revise_constraint("responses", {"network", "withdrawn", 0, user.id, false}, 2);
    rejects(ErrorCode::Integrity, [&] { memory.append_response(user, revised, "2", "{\"nonce\":42}"); });
    writer.revise_constraint("responses", {"execution", "disabled", 0, user.id, true}, 0);
    rejects(ErrorCode::Integrity, [&] { memory.append_response(user, {}, "2", "{\"nonce\":42}"); });
    const auto current = memory.constraints("responses");
    const auto response = memory.append_response(user, current, "2", "{\"response_nonce\":42}");
    check(response.role == Role::Assistant && response.session == user.session && response.id > user.id);
    check(memory.get("responses", user.id) == user && memory.get("responses", response.id) == response);
    check(memory.search("responses", "response_nonce").messages == std::vector<Message>{response});
    rejects(ErrorCode::Integrity, [&] { memory.append_response(user, current, "3", "{\"response_nonce\":42}"); });
    check(memory.recent("responses").messages.size() == 2);
    const auto next = writer.append("responses", Role::User, "3", "next-request");
    writer.append("responses", Role::User, "4", "newer-request");
    rejects(ErrorCode::Integrity, [&] { memory.append_response(next, current, "5", "{\"nonce\":43}"); });
    const auto newest = memory.recent("responses", 1).messages.back();
    ConversationMemory limited(path, {4096, 1});
    rejects(ErrorCode::Budget, [&] { limited.append_response(newest, current, "5", "{\"nonce\":43}"); });
    check(memory.recent("responses").messages.size() == 4);
    rejects(ErrorCode::Schema, [&] { memory.append_response(response, current, "5", "{\"nonce\":43}"); });
}
void rare_match_uses_index_under_bounded_work() {
    Directory directory;
    const auto path = directory.path / "rare-match.sqlite";
    Message first, second;
    {
        // Given many nonmatching rows and matching rows in another session.
        ConversationMemory memory(path);
        memory.open_session("target", "project");
        memory.open_session("other", "project");
        first = memory.append("target", Role::User, "1", "indexed needle");
        for (unsigned index = 0; index < 512; ++index)
            memory.append("target", Role::Assistant, std::to_string(index), "Unrelated ordinary context");
        memory.append("other", Role::User, "2", "indexed needle");
        second = memory.append("target", Role::User, "3", "indexed needle");
    }
    // When a rare phrase is retrieved under a fixed small instruction budget.
    ConversationMemory bounded(path, {4096, 1000});
    const auto page = bounded.search("target", "indexed needle", 0, 1);
    // Then the indexed match stream remains exact, isolated and paginated.
    check(page.messages == std::vector<Message>{first} && !page.complete && page.next == first.id);
    const auto next = bounded.search("target", "indexed needle", *page.next, 1);
    check(next.messages == std::vector<Message>{second} && next.complete);
    check(bounded.search("target", "not present").messages.empty());
}
} // namespace

int main() {
    try {
        persistence();
        search_and_limits();
        concurrent_writers_and_separation();
        guarded_responses();
        rare_match_uses_index_under_bounded_work();
        std::cout << "Conversation checks passed: " << checks << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
