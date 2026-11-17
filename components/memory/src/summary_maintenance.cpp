#include "summary_maintenance.hpp"

#include <algorithm>
#include <json-c/json.h>

namespace axiom::context {
namespace {
using knowledge::Error;
using knowledge::ErrorCode;
using Json = std::unique_ptr<json_object, decltype(&json_object_put)>;
constexpr std::string_view profile = "gammathink.context-summary.extractive.v1";

void require(bool value, ErrorCode code, const char* message) {
    if (!value) throw Error(code, message);
}
void text(json_object* object, const char* key, std::string_view value) {
    json_object_object_add(object, key,
        json_object_new_string_len(value.data(), static_cast<int>(value.size())));
}
void number(json_object* object, const char* key, std::uint64_t value) {
    json_object_object_add(object, key, json_object_new_uint64(value));
}
std::string serialized(json_object* object) {
    const auto* value = json_object_to_json_string_ext(object, JSON_C_TO_STRING_PLAIN);
    require(value != nullptr, ErrorCode::Storage, "Cannot serialize extractive summary");
    return value;
}
std::string hash(std::string_view value) {
    return knowledge::hex({knowledge::digest({
        reinterpret_cast<const std::uint8_t*>(value.data()), value.size()})});
}
json_object* source(const Message& message, bool include_content = true) {
    auto* result = json_object_new_object();
    require(result != nullptr, ErrorCode::Storage, "Cannot allocate summary source");
    number(result, "message", message.id);
    number(result, "role", static_cast<unsigned>(message.role));
    text(result, "timestamp", message.timestamp);
    text(result, "content_hash", knowledge::hex({message.content_hash}));
    if (include_content) text(result, "content", message.content);
    return result;
}
std::optional<std::string> previous_pin_digest(const Summary& summary) {
    auto parser = std::unique_ptr<json_tokener, decltype(&json_tokener_free)>(
        json_tokener_new_ex(32), json_tokener_free);
    require(bool(parser), ErrorCode::Storage, "Cannot allocate summary metadata parser");
    Json parsed(json_tokener_parse_ex(parser.get(), summary.content.data(),
        static_cast<int>(summary.content.size())), json_object_put);
    if (json_tokener_get_error(parser.get()) != json_tokener_success ||
        json_tokener_get_parse_end(parser.get()) != summary.content.size() || !parsed ||
        !json_object_is_type(parsed.get(), json_type_object)) return {};
    json_object* kind = nullptr;
    json_object* digest = nullptr;
    if (!json_object_object_get_ex(parsed.get(), "profile", &kind) ||
        !json_object_is_type(kind, json_type_string) ||
        std::string_view(json_object_get_string(kind),
            static_cast<std::size_t>(json_object_get_string_len(kind))) != profile ||
        !json_object_object_get_ex(parsed.get(), "constraint_digest", &digest) ||
        !json_object_is_type(digest, json_type_string)) return {};
    return std::string(json_object_get_string(digest),
        static_cast<std::size_t>(json_object_get_string_len(digest)));
}
} // namespace

bool refresh_summary(ConversationMemory& memory, std::string_view session, SummaryMaintenanceLimits limits) {
    require(limits.cadence_messages > 0 && limits.cadence_messages <= 256 &&
        limits.recent_messages > 0 && limits.recent_messages <= 256 &&
        limits.constraints > 0 && limits.constraints <= 256 &&
        limits.source_bytes > 0 && limits.source_bytes <= 4 * 1024 * 1024 &&
        limits.summary_bytes >= 512 && limits.summary_bytes <= knowledge::maximum_text_bytes,
        ErrorCode::Budget, "Invalid automatic summary policy");
    std::optional<Summary> previous;
    Message initial;
    MessagePage recent;
    std::vector<Constraint> pins;
    std::string pin_digest;
    bool due = false;
    Json constraints(json_object_new_array(), json_object_put);
    require(bool(constraints), ErrorCode::Storage, "Cannot allocate summary constraints");
    memory.read_snapshot([&] {
        previous = memory.summary(session);
        pins = memory.constraint_states(session, limits.constraints);
        for (const auto& pin : pins) {
            auto* value = json_object_new_object();
            require(value != nullptr, ErrorCode::Storage, "Cannot allocate summary constraint");
            text(value, "key", pin.key); text(value, "content", pin.content);
            number(value, "revision", pin.revision); number(value, "source_message", pin.source_message);
            json_object_object_add(value, "active", json_object_new_boolean(pin.active));
            json_object_array_add(constraints.get(), value);
        }
        pin_digest = hash(serialized(constraints.get()));
        const auto changed = memory.messages_after(session, previous ? previous->last_message : 0,
            limits.cadence_messages, limits.source_bytes);
        due = changed.messages.size() >= limits.cadence_messages;
        if (previous) {
            const auto prior = previous_pin_digest(*previous);
            due = due || (prior && *prior != pin_digest);
        }
        if (!due) return;
        const auto first = memory.messages_after(session, 0, 1, limits.source_bytes);
        require(!first.messages.empty(), ErrorCode::Missing, "Missing initial summary source");
        initial = first.messages.front();
        recent = memory.recent(session, limits.recent_messages, limits.source_bytes);
        require(!recent.messages.empty(), ErrorCode::Missing, "Missing recent summary source");
    });
    if (!due) return false;
    Json root(json_object_new_object(), json_object_put);
    require(bool(root), ErrorCode::Storage, "Cannot allocate extractive summary");
    text(root.get(), "profile", profile);
    text(root.get(), "constraint_digest", pin_digest);
    number(root.get(), "previous_version", previous ? previous->version : 0);
    if (previous) text(root.get(), "previous_content_hash", hash(previous->content));
    json_object_object_add(root.get(), "initial_message", source(initial));
    json_object_object_add(root.get(), "constraints", constraints.release());
    json_object_object_add(root.get(), "recent_window_complete", json_object_new_boolean(recent.complete));
    text(root.get(), "recent_order", "newest-first");
    number(root.get(), "considered_recent_messages", recent.messages.size());
    const auto includes_initial = std::any_of(recent.messages.begin(), recent.messages.end(),
        [&](const auto& message) { return message.id == initial.id; });
    std::size_t omitted = recent.messages.size() - static_cast<std::size_t>(includes_initial);
    number(root.get(), "omitted_recent_messages", omitted);
    auto* entries = json_object_new_array();
    require(entries != nullptr, ErrorCode::Storage, "Cannot allocate summary message list");
    json_object_object_add(root.get(), "recent_messages", entries);
    if (serialized(root.get()).size() > limits.summary_bytes) {
        // Keep a source reference rather than clipping an original quotation.
        json_object_object_add(root.get(), "initial_message", source(initial, false));
        json_object_object_add(root.get(), "initial_content_omitted", json_object_new_boolean(true));
        if (includes_initial) {
            ++omitted;
            number(root.get(), "omitted_recent_messages", omitted);
        }
    }
    require(serialized(root.get()).size() <= limits.summary_bytes,
        ErrorCode::Budget, "Source references and constraint states exceed summary budget");
    for (auto message = recent.messages.rbegin(); message != recent.messages.rend(); ++message) {
        if (message->id == initial.id) continue;
        json_object_array_add(entries, source(*message));
        --omitted;
        number(root.get(), "omitted_recent_messages", omitted);
        if (serialized(root.get()).size() > limits.summary_bytes) {
            json_object_array_del_idx(entries, json_object_array_length(entries) - 1, 1);
            ++omitted;
            number(root.get(), "omitted_recent_messages", omitted);
        }
    }
    const auto content = serialized(root.get());
    memory.save_summary(session,
        {0, initial.id, recent.messages.back().id, content}, previous ? previous->version : 0);
    return true;
}

} // namespace axiom::context
