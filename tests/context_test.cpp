#include "context.hpp"

#include <algorithm>
#include <iostream>
#include <json-c/json.h>
#include <stdlib.h>
#include <unistd.h>

namespace {
using namespace axiom::context;
using axiom::knowledge::Error;
using axiom::knowledge::ErrorCode;
std::size_t checks = 0;
void check(bool value) {
    ++checks;
    if (!value) throw std::runtime_error("Context assertion " + std::to_string(checks));
}
template<class F> void rejects(ErrorCode expected, F&& operation) {
    ++checks;
    try { operation(); }
    catch (const Error& error) {
        if (error.code == expected) return;
        throw;
    }
    throw std::runtime_error("Expected context rejection " + std::to_string(checks));
}
struct Directory {
    std::filesystem::path path;
    Directory() {
        auto pattern = (std::filesystem::temp_directory_path() / "gammathink-context-assembly-XXXXXX").string();
        std::vector<char> name(pattern.begin(), pattern.end());
        name.push_back(0);
        const auto* created = mkdtemp(name.data());
        if (!created) throw std::runtime_error("Cannot create context test directory");
        path = created;
    }
    ~Directory() { std::filesystem::remove_all(path); }
};
using Json = std::unique_ptr<json_object, decltype(&json_object_put)>;
Json parse(std::string_view text) {
    auto* raw = json_tokener_parse(std::string(text).c_str());
    check(raw != nullptr);
    return {raw, json_object_put};
}
std::uint64_t number(json_object* value, const char* key) {
    json_object* field = nullptr;
    check(json_object_object_get_ex(value, key, &field) && json_object_is_type(field, json_type_int));
    return json_object_get_uint64(field);
}
std::string string(json_object* value, const char* key) {
    json_object* field = nullptr;
    check(json_object_object_get_ex(value, key, &field) && json_object_is_type(field, json_type_string));
    return {json_object_get_string(field), static_cast<std::size_t>(json_object_get_string_len(field))};
}
std::size_t synthetic_tokens(std::string_view input) {
    // Test-only exact tokenizer: one byte per token, plus fixed framing.
    return input.size() + 7;
}

void retrieval_and_precedence() {
    Directory directory;
    const auto path = directory.path / "memory.sqlite";
    Message old, instruction, recent, current;
    Summary summary;
    {
        ConversationMemory memory(path);
        memory.open_session("s", "p");
        memory.open_session("other", "p");
        old = memory.append("s", Role::User, "t", "history match old");
        instruction = memory.append("s", Role::User, "t", "session instruction");
        recent = memory.append("s", Role::Assistant, "t", "history match recent");
        current = memory.append("s", Role::User, "t", "current \"quoted\"\n\t caf\xc3\xa9");
        memory.append("other", Role::User, "t", "history match private");
        memory.revise_constraint("s", {"size", "old", 0, old.id, true}, 0);
        memory.revise_constraint("s", {"size", "new", 0, current.id, true}, 1);
        summary = memory.save_summary("s", {0, old.id, recent.id, "stale derived context"}, 0);
    }
    ConversationMemory memory(path);
    const std::array instructions{instruction.id};
    const auto sources = retrieve_context(memory, "s", instructions, "history match", {1, 16, 32, 64, 262144});
    check(sources.current == current);
    check(sources.instructions == std::vector<Message>{instruction});
    check(sources.recent == std::vector<Message>{recent});
    check(sources.history == std::vector<Message>{old});
    check(!sources.recent_complete && sources.history_complete);
    check(sources.constraints.size() == 1 && sources.constraints.front().revision == 2);
    check(sources.constraints.front().source_message == current.id);
    check(sources.summary == summary);
    const auto package = assemble_context(sources, synthetic_tokens, {20000, 20000, 256});
    check(package.entries.size() == 6);
    check(package.input_tokens == synthetic_tokens(package.json));
    for (std::size_t i = 0; i < package.entries.size(); ++i) {
        check(static_cast<unsigned>(package.entries[i].layer) == i + 1);
    }
    check(package.entries[2].content == sources.constraints.front().content);
    check(package.entries[4].revision == summary.version);
    check(package.entries[5].message == old.id);
    check(std::all_of(package.omitted.begin(), package.omitted.end(), [](auto n) { return n == 0; }));
    auto json = parse(package.json);
    check(number(json.get(), "schema") == 1);
    check(string(json.get(), "session") == "s");
    json_object* entries = nullptr;
    check(json_object_object_get_ex(json.get(), "entries", &entries));
    check(json_object_array_length(entries) == package.entries.size());
    for (std::size_t i = 0; i < package.entries.size(); ++i) {
        const auto* expected = &package.entries[i];
        auto* entry = json_object_array_get_idx(entries, i);
        check(number(entry, "priority") == static_cast<unsigned>(expected->layer));
        check(number(entry, "message") == expected->message);
        check(number(entry, "revision") == expected->revision);
        check(string(entry, "content") == expected->content);
        check(string(entry, "content_hash") == axiom::knowledge::hex({expected->content_hash}));
    }
    const auto again = assemble_context(retrieve_context(memory, "s", instructions, "history match",
        {1, 16, 32, 64, 262144}), synthetic_tokens, {20000, 20000, 256});
    check(again.json == package.json);
    check(retrieve_context(memory, "s", {}, {}, {0, 0, 0, 64, 262144}).recent.empty());
    rejects(ErrorCode::Schema, [&] { retrieve_context(memory, "s", std::array{recent.id}); });
    rejects(ErrorCode::Schema, [&] { retrieve_context(memory, "s", std::array{current.id}); });
    rejects(ErrorCode::Schema, [&] { retrieve_context(memory, "s", std::array{instruction.id, instruction.id}); });
    rejects(ErrorCode::Missing, [&] { retrieve_context(memory, "s", std::array<std::uint64_t, 1>{999}); });
    rejects(ErrorCode::Budget, [&] { retrieve_context(memory, "s", instructions, {}, {1, 1, 0, 64, 262144}); });
    memory.append("s", Role::Assistant, "t", "responded");
    rejects(ErrorCode::Schema, [&] { retrieve_context(memory, "s"); });
    memory.open_session("empty", "p");
    rejects(ErrorCode::Missing, [&] { retrieve_context(memory, "empty"); });
}

void budgets_and_invalid_sources() {
    Directory directory;
    ConversationMemory memory(directory.path / "memory.sqlite");
    memory.open_session("s", "p");
    const auto old = memory.append("s", Role::User, "t", std::string(2000, 'a'));
    const auto newest = memory.append("s", Role::Assistant, "t", "short");
    const auto current = memory.append("s", Role::User, "t", "request");
    memory.revise_constraint("s", {"key", "pin", 0, old.id, true}, 0);
    auto sources = retrieve_context(memory, "s");
    const auto full = assemble_context(sources, synthetic_tokens, {20000, 20000, 256});
    check(full.entries.size() == 4);
    check(full.entries[2].message == newest.id && full.entries[3].message == old.id);
    const auto exact = assemble_context(sources, synthetic_tokens,
        {full.input_tokens, full.json.size(), 256});
    check(exact.json == full.json && exact.input_tokens == full.input_tokens);
    const auto reduced = assemble_context(sources, synthetic_tokens,
        {full.input_tokens - 1, full.json.size(), 256});
    check(reduced.entries.size() == 3 && reduced.omitted[4] == 1);
    check(reduced.entries.back().message == newest.id);
    check(reduced.input_tokens <= full.input_tokens - 1 && reduced.input_tokens == synthetic_tokens(reduced.json));
    const auto byte_reduced = assemble_context(sources, synthetic_tokens,
        {20000, full.json.size() - 1, 256});
    check(byte_reduced.entries.size() == 3 && byte_reduced.omitted[4] == 1);
    auto mandatory = sources;
    mandatory.recent.clear();
    const auto required = assemble_context(mandatory, synthetic_tokens, {20000, 20000, 256});
    rejects(ErrorCode::Budget, [&] {
        assemble_context(sources, synthetic_tokens, {required.input_tokens - 1, 20000, 256});
    });
    rejects(ErrorCode::Budget, [&] {
        assemble_context(sources, synthetic_tokens, {20000, required.json.size() - 1, 256});
    });
    rejects(ErrorCode::Budget, [&] { assemble_context(sources, synthetic_tokens, {20000, 20000, 1}); });
    rejects(ErrorCode::Schema, [&] { assemble_context(sources, {}); });
    auto invalid = sources;
    invalid.current.content_hash[0] ^= 1;
    rejects(ErrorCode::Integrity, [&] { assemble_context(invalid, synthetic_tokens); });
    invalid = sources; invalid.recent.push_back(invalid.current);
    rejects(ErrorCode::Schema, [&] { assemble_context(invalid, synthetic_tokens); });
    invalid = sources; invalid.recent.front().session = "other";
    rejects(ErrorCode::Schema, [&] { assemble_context(invalid, synthetic_tokens); });
    invalid = sources; invalid.constraints.front().active = false;
    rejects(ErrorCode::Schema, [&] { assemble_context(invalid, synthetic_tokens); });
    invalid = sources; invalid.constraints.push_back(invalid.constraints.front());
    rejects(ErrorCode::Schema, [&] { assemble_context(invalid, synthetic_tokens); });
    invalid = sources; invalid.summary = Summary{1, old.id, current.id + 1, "future"};
    rejects(ErrorCode::Schema, [&] { assemble_context(invalid, synthetic_tokens); });
    // Tokenization exceptions are real failures, not optional-record omission.
    bool tokenizer_failed = false;
    try {
        assemble_context(sources, [](std::string_view) -> std::size_t {
            throw std::runtime_error("Tokenizer unavailable");
        });
    } catch (const std::runtime_error& error) {
        tokenizer_failed = std::string_view(error.what()) == "Tokenizer unavailable";
    }
    check(tokenizer_failed);
    // Whole-package tokenization, deliberately non-additive.
    const auto nonlinear = [](std::string_view input) { return input.size() % 17 + 1; };
    const auto nonadditive = assemble_context(sources, nonlinear, {17, 20000, 256});
    check(nonadditive.entries.size() == full.entries.size());
    check(nonadditive.input_tokens == nonlinear(nonadditive.json));
}

void snapshots_and_recovery() {
    Directory directory;
    const auto path = directory.path / "memory.sqlite";
    ConversationMemory reader(path), writer(path);
    reader.open_session("s", "p");
    const auto first = reader.append("s", Role::User, "t", "before");
    reader.revise_constraint("s", {"key", "before", 0, first.id, true}, 0);
    reader.save_summary("s", {0, first.id, first.id, "before"}, 0);
    Message second;
    reader.read_snapshot([&] {
        // The view is pinned by read_snapshot itself, before this writer runs.
        second = writer.append("s", Role::User, "t", "after");
        writer.revise_constraint("s", {"key", "after", 0, second.id, true}, 1);
        writer.save_summary("s", {0, first.id, second.id, "after"}, 1);
        check(reader.recent("s").messages == std::vector<Message>{first});
        check(!reader.get("s", second.id));
        check(reader.constraint("s", "key")->revision == 1);
        check(reader.summary("s")->version == 1);
        rejects(ErrorCode::Schema, [&] { reader.read_snapshot([] {}); });
        rejects(ErrorCode::Schema, [&] { reader.append("s", Role::User, "t", "not allowed"); });
    });
    check(reader.recent("s").messages == std::vector<Message>{first, second});
    check(reader.get("s", second.id) == second);
    check(reader.summary("s")->version == 2 && reader.constraint("s", "key")->revision == 2);
    bool callback_failed = false;
    try {
        reader.read_snapshot([] { throw std::runtime_error("Callback failed"); });
    } catch (const std::runtime_error& error) {
        callback_failed = std::string_view(error.what()) == "Callback failed";
    }
    check(callback_failed);
    const auto last = reader.append("s", Role::User, "t", "recovered");
    check(reader.get("s", last.id) == last);
    const auto sources = retrieve_context(reader, "s");
    check(sources.current == last && sources.constraints.front().revision == 2);
}
} // namespace

int main() {
    try {
        retrieval_and_precedence();
        budgets_and_invalid_sources();
        snapshots_and_recovery();
        std::cout << "Context checks passed: " << checks << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
