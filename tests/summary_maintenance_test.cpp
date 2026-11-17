#include "summary_maintenance.hpp"

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
    if (!value) throw std::runtime_error("Summary maintenance assertion " + std::to_string(checks));
}
template<class F> void rejects(ErrorCode code, F&& operation) {
    ++checks;
    try { operation(); }
    catch (const Error& error) {
        if (error.code == code) return;
        throw;
    }
    throw std::runtime_error("Expected summary maintenance rejection " + std::to_string(checks));
}
struct Directory {
    std::filesystem::path path;
    Directory() {
        auto pattern = (std::filesystem::temp_directory_path() / "gammathink-summary-XXXXXX").string();
        std::vector<char> name(pattern.begin(), pattern.end());
        name.push_back(0);
        const auto* created = mkdtemp(name.data());
        if (!created) throw std::runtime_error("Cannot create summary fixture directory");
        path = created;
    }
    ~Directory() { std::filesystem::remove_all(path); }
};
using Json = std::unique_ptr<json_object, decltype(&json_object_put)>;
Json parse(const Summary& summary) {
    Json result(json_tokener_parse(summary.content.c_str()), json_object_put);
    check(bool(result));
    return result;
}
json_object* field(json_object* object, const char* key) {
    json_object* result = nullptr;
    check(json_object_object_get_ex(object, key, &result));
    return result;
}
std::uint64_t number(json_object* object, const char* key) {
    auto* value = field(object, key);
    check(json_object_is_type(value, json_type_int));
    return json_object_get_uint64(value);
}
std::string text(json_object* object, const char* key) {
    auto* value = field(object, key);
    check(json_object_is_type(value, json_type_string));
    return {json_object_get_string(value), static_cast<std::size_t>(json_object_get_string_len(value))};
}
void cadence_and_corrections() {
    Directory directory;
    const auto path = directory.path / "context.sqlite";
    Message initial, current;
    Summary saved;
    SummaryMaintenanceLimits limits;
    limits.cadence_messages = 4;
    {
        ConversationMemory memory(path);
        memory.open_session("s", "p");
        memory.open_session("other", "p");
        initial = memory.append("s", Role::User, "1", "original \"quoted\"\n caf\xc3\xa9");
        memory.append("other", Role::User, "1", "separate-session-record");
        const auto assistant = memory.append("s", Role::Assistant, "2", "derived response");
        memory.append("s", Role::User, "3", "next original request");
        check(!refresh_summary(memory, "s", limits) && !memory.summary("s"));
        current = memory.append("s", Role::User, "4", "current request");
        check(refresh_summary(memory, "s", limits));
        saved = *memory.summary("s");
        check(saved.version == 1 && saved.first_message == initial.id && saved.last_message == current.id);
        auto json = parse(saved);
        check(number(json.get(), "previous_version") == 0);
        check(number(json.get(), "considered_recent_messages") == 4);
        check(number(json.get(), "omitted_recent_messages") == 0);
        auto* anchor = field(json.get(), "initial_message");
        check(number(anchor, "message") == initial.id && text(anchor, "content") == initial.content);
        check(text(anchor, "content_hash") == axiom::knowledge::hex({initial.content_hash}));
        check(!refresh_summary(memory, "s", limits) && memory.summary("s") == saved);
        const auto page = memory.messages_after("s", 0, 2);
        check(page.messages == std::vector<Message>{initial, assistant});
        check(!page.complete && page.next == assistant.id);
        const auto rest = memory.messages_after("s", *page.next, 2);
        check(rest.messages.size() == 2 && rest.complete && rest.messages.back() == current);

        memory.revise_constraint("s", {"network", "offline", 0, current.id, true}, 0);
        check(refresh_summary(memory, "s", limits));
        const auto second = *memory.summary("s");
        check(second.version == 2 && memory.summary("s", 1) == saved);
        memory.revise_constraint("s", {"network", "withdrawn", 0, current.id, false}, 1);
        check(memory.constraints("s").empty());
        check(memory.constraint_states("s").size() == 1 && !memory.constraint_states("s")[0].active);
        check(refresh_summary(memory, "s", limits));
        const auto third = *memory.summary("s");
        auto revised = parse(third);
        check(third.version == 3 && number(revised.get(), "previous_version") == 2);
        auto* states = field(revised.get(), "constraints");
        check(json_object_array_length(states) == 1);
        auto* state = json_object_array_get_idx(states, 0);
        check(number(state, "revision") == 2 && number(state, "source_message") == current.id);
        check(!json_object_get_boolean(field(state, "active")));
        check(memory.get("s", initial.id) == initial && memory.get("s", current.id) == current);
        check(!refresh_summary(memory, "s", limits));
    }
    {
        ConversationMemory reopened(path);
        check(reopened.get("s", initial.id) == initial);
        check(reopened.summary("s", 1) == saved && reopened.summary("s")->version == 3);
        check(!refresh_summary(reopened, "s", limits));
    }
}
void bounded_whole_records() {
    ConversationMemory memory(":memory:");
    memory.open_session("s", "p");
    const auto initial = memory.append("s", Role::User, "1", "anchor");
    for (unsigned index = 0; index < 5; ++index)
        memory.append("s", Role::Assistant, std::to_string(index), std::string(1000, 'a'));
    SummaryMaintenanceLimits limits;
    limits.cadence_messages = 2;
    limits.summary_bytes = 2400;
    check(refresh_summary(memory, "s", limits));
    const auto summary = *memory.summary("s");
    check(summary.content.size() <= limits.summary_bytes);
    auto json = parse(summary);
    check(number(json.get(), "omitted_recent_messages") > 0);
    auto* recent = field(json.get(), "recent_messages");
    check(json_object_array_length(recent) > 0 && json_object_array_length(recent) < 5);
    for (std::size_t index = 0; index < json_object_array_length(recent); ++index) {
        auto* entry = json_object_array_get_idx(recent, index);
        const auto original = memory.get("s", number(entry, "message"));
        check(original && text(entry, "content") == original->content);
        check(text(entry, "content_hash") == axiom::knowledge::hex({original->content_hash}));
    }
    check(memory.get("s", initial.id) == initial && memory.messages_after("s").messages.size() == 6);
    limits.cadence_messages = 0;
    rejects(ErrorCode::Budget, [&] { refresh_summary(memory, "s", limits); });
    check(memory.summary("s") == summary);
    ConversationMemory large(":memory:");
    large.open_session("s", "p");
    const auto source = large.append("s", Role::User, "1", std::string(2000, 'b'));
    large.revise_constraint("s", {"required", std::string(2000, 'b'), 0, source.id, true}, 0);
    limits = {};
    limits.cadence_messages = 1;
    limits.summary_bytes = 512;
    rejects(ErrorCode::Budget, [&] { refresh_summary(large, "s", limits); });
    check(!large.summary("s") && large.get("s", source.id) == source);
    ConversationMemory referenced(":memory:");
    referenced.open_session("s", "p");
    const auto long_source = referenced.append("s", Role::User, "1", std::string(20000, 'c'));
    referenced.append("s", Role::User, "2", "latest request");
    limits.summary_bytes = 4096;
    check(refresh_summary(referenced, "s", limits));
    auto bounded = parse(*referenced.summary("s"));
    check(json_object_get_boolean(field(bounded.get(), "initial_content_omitted")));
    auto* anchor = field(bounded.get(), "initial_message");
    check(number(anchor, "message") == long_source.id);
    check(text(anchor, "content_hash") == axiom::knowledge::hex({long_source.content_hash}));
    json_object* omitted_content = nullptr;
    check(!json_object_object_get_ex(anchor, "content", &omitted_content));
    check(referenced.get("s", long_source.id) == long_source);
}
void manual_history_is_preserved() {
    ConversationMemory memory(":memory:");
    memory.open_session("s", "p");
    const auto first = memory.append("s", Role::User, "1", "original request");
    const auto manual = memory.save_summary("s", {0, first.id, first.id, "host-authored summary"}, 0);
    SummaryMaintenanceLimits limits;
    limits.cadence_messages = 2;
    check(!refresh_summary(memory, "s", limits) && memory.summary("s") == manual);
    memory.append("s", Role::Assistant, "2", "response");
    const auto latest = memory.append("s", Role::User, "3", "new request");
    check(refresh_summary(memory, "s", limits));
    const auto generated = *memory.summary("s");
    check(generated.version == 2 && generated.last_message == latest.id);
    check(memory.summary("s", 1) == manual);
    auto json = parse(generated);
    check(number(json.get(), "previous_version") == 1);
}
} // namespace

int main() {
    try {
        cadence_and_corrections();
        bounded_whole_records();
        manual_history_is_preserved();
        std::cout << "summary maintenance tests passed: " << checks << " deterministic checks\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
