#include "context.hpp"

#include <algorithm>
#include <set>
#include <tuple>

namespace axiom::context {
namespace {
using knowledge::Error;
using knowledge::ErrorCode;
void require(bool condition, ErrorCode code, const char* message) {
    if (!condition) throw Error(code, message);
}
void read_limits(ContextReadLimits limits) {
    require(limits.recent_messages <= 256 && limits.historical_messages <= 256 &&
            limits.instructions <= 256 && limits.constraints > 0 && limits.constraints <= 256 &&
            limits.content_bytes > 0 && limits.content_bytes <= 4 * 1024 * 1024,
            ErrorCode::Budget, "Context retrieval limits exceeded");
}
struct SerializationLimit {};
struct Json {
    std::string value;
    std::size_t maximum;
    void operator+=(std::string_view part) {
        if (part.size() > maximum - value.size()) throw SerializationLimit{};
        value += part;
    }
    void operator+=(char c) {
        if (value.size() == maximum) throw SerializationLimit{};
        value += c;
    }
};
void quoted(Json& output, std::string_view value) {
    constexpr std::string_view hex = "0123456789abcdef";
    output += '"';
    for (const unsigned char c : value) {
        if (c == '"' || c == '\\') {
            output += '\\';
            output += static_cast<char>(c);
        } else if (c < 32) {
            output += "\\u00";
            output += hex[c >> 4U];
            output += hex[c & 15U];
        } else {
            output += static_cast<char>(c);
        }
    }
    output += '"';
}
std::string serialize(const ContextPackage& package, std::size_t maximum) {
    Json out{{}, maximum};
    out += package.documents_enabled ? "{\"schema\":2,\"session\":" : "{\"schema\":1,\"session\":";
    quoted(out, package.session);
    out += ",\"recent_complete\":" + std::string(package.recent_complete ? "true" : "false");
    out += ",\"history_complete\":" + std::string(package.history_complete ? "true" : "false");
    if (package.documents_enabled)
        out += ",\"documents_complete\":" + std::string(package.documents_complete ? "true" : "false");
    out += ",\"entries\":[";
    bool first = true;
    for (const auto& entry : package.entries) {
        if (!first) out += ',';
        first = false;
        out += "{\"priority\":" + std::to_string(static_cast<unsigned>(entry.layer));
        if (entry.document) {
            const auto& excerpt = *entry.document;
            const auto& source = excerpt.source;
            out += ",\"document\":{\"project\":"; quoted(out, source.project);
            out += ",\"session\":"; quoted(out, source.session);
            out += ",\"key\":"; quoted(out, source.key);
            out += ",\"uri\":"; quoted(out, source.uri);
            out += ",\"kind\":" + std::to_string(static_cast<unsigned>(source.kind));
            out += ",\"attribution\":" + std::to_string(static_cast<unsigned>(source.attribution));
            out += ",\"source_hash\":"; quoted(out, knowledge::hex({source.content_hash}));
            out += ",\"source_bytes\":" + std::to_string(excerpt.source_bytes);
            out += ",\"offset\":" + std::to_string(excerpt.offset) + "}";
        } else out += ",\"role\":" + std::to_string(static_cast<unsigned>(entry.role));
        out += ",\"message\":" + std::to_string(entry.message);
        out += ",\"revision\":" + std::to_string(entry.revision);
        out += ",\"first_message\":" + std::to_string(entry.first_message);
        out += ",\"last_message\":" + std::to_string(entry.last_message);
        out += ",\"key\":"; quoted(out, entry.key);
        out += ",\"timestamp\":"; quoted(out, entry.timestamp);
        out += ",\"content_hash\":"; quoted(out, knowledge::hex({entry.content_hash}));
        out += ",\"content\":"; quoted(out, entry.content);
        out += '}';
    }
    out += "],\"omitted\":[";
    const auto layers = package.documents_enabled ? package.omitted.size() : 7;
    for (std::size_t i = 1; i < layers; ++i) {
        if (i != 1) out += ',';
        out += std::to_string(package.omitted[i]);
    }
    out += "]}";
    return std::move(out.value);
}
ContextEntry message_entry(ContextLayer layer, const Message& message) {
    return {layer, message.role, message.id, 0, 0, 0, {}, message.timestamp,
            message.content, message.content_hash};
}
knowledge::Digest hash(std::string_view content) {
    return knowledge::digest({reinterpret_cast<const std::uint8_t*>(content.data()), content.size()});
}
} // namespace

ContextSources retrieve_context(ConversationMemory& memory, std::string_view session,
    std::span<const std::uint64_t> instruction_ids, std::string_view phrase, ContextReadLimits limits) {
    read_limits(limits);
    require(instruction_ids.size() <= limits.instructions, ErrorCode::Budget,
            "Too many explicit context instructions");
    ContextSources result;
    memory.read_snapshot([&] {
        result.project = memory.project(session);
        auto recent = memory.recent(session, limits.recent_messages + 1, limits.content_bytes);
        require(!recent.messages.empty(), ErrorCode::Missing, "No current conversation request");
        result.current = std::move(recent.messages.back());
        recent.messages.pop_back();
        require(result.current.role == Role::User, ErrorCode::Schema,
                "Context requires a latest user message");
        result.recent = std::move(recent.messages);
        result.recent_complete = recent.complete;
        std::set<std::uint64_t> included{result.current.id};
        for (const auto id : instruction_ids) {
            require(included.insert(id).second, ErrorCode::Schema, "Duplicate context instruction");
            auto message = memory.get(session, id);
            require(message.has_value(), ErrorCode::Missing, "Missing context instruction source");
            require(message->role == Role::User, ErrorCode::Schema,
                    "Explicit context instructions require user sources");
            result.instructions.push_back(std::move(*message));
        }
        result.constraints = memory.constraints(session, limits.constraints);
        result.summary = memory.summary(session);
        for (const auto& pin : result.constraints) {
            const auto source = memory.get(session, pin.source_message);
            require(source && source->role == Role::User, ErrorCode::Integrity,
                    "Invalid pinned context source");
        }
        if (result.summary) {
            require(memory.get(session, result.summary->first_message).has_value() &&
                    memory.get(session, result.summary->last_message).has_value(),
                    ErrorCode::Integrity, "Missing context summary source");
        }
        for (const auto& message : result.recent) included.insert(message.id);
        if (!phrase.empty() && limits.historical_messages != 0) {
            // One bounded indexed page. A partial page is reported, not silently
            // scanned indefinitely to fill a requested number of unique hits.
            const auto page = memory.search(session, phrase, 0, limits.historical_messages,
                                            limits.content_bytes);
            result.history_complete = page.complete;
            for (const auto& message : page.messages) {
                if (included.insert(message.id).second) result.history.push_back(message);
            }
        }
    });
    // Instructions are a separately attributed, higher-priority view.
    std::erase_if(result.recent, [&](const Message& message) {
        return std::any_of(result.instructions.begin(), result.instructions.end(),
            [&](const Message& instruction) { return instruction.id == message.id; });
    });
    return result;
}

ContextPackage assemble_context(const ContextSources& sources, const ContextTokenCounter& count,
                                ContextBudget budget) {
    require(static_cast<bool>(count), ErrorCode::Schema, "Missing exact context tokenizer");
    return assemble_context_package(sources, [&](const ContextPackage& package) {
        return count(package.json);
    }, budget);
}

ContextPackage assemble_context_package(const ContextSources& sources,
    const std::function<std::size_t(const ContextPackage&)>& count, ContextBudget budget) {
    require(static_cast<bool>(count), ErrorCode::Schema, "Missing exact context tokenizer");
    require(budget.input_tokens > 0 && budget.serialized_bytes > 0 &&
            budget.serialized_bytes <= 4 * 1024 * 1024 && budget.candidates > 0 &&
            budget.candidates <= 1024, ErrorCode::Budget, "Invalid context assembly budget");
    const auto candidates = 1 + sources.instructions.size() + sources.constraints.size() +
        sources.recent.size() + sources.history.size() + sources.documents.size() + (sources.summary ? 1 : 0);
    require(candidates <= budget.candidates, ErrorCode::Budget, "Too many context candidates");
    require(sources.current.role == Role::User && sources.current.id > 0 &&
            !sources.current.session.empty(), ErrorCode::Schema, "Invalid current context message");
    ContextPackage result;
    result.session = sources.current.session;
    knowledge::validate_text(result.session, 256);
    result.recent_complete = sources.recent_complete;
    result.history_complete = sources.history_complete;
    result.documents_enabled = sources.documents_enabled;
    result.documents_complete = sources.documents_complete;
    require(sources.documents_enabled || sources.documents.empty(), ErrorCode::Schema,
            "Document context requires explicit retrieval");
    std::set<std::uint64_t> message_ids;
    auto validate_message = [&](const Message& message) {
        require(message.session == result.session && message.id > 0 &&
                message.id <= sources.current.id && message.role >= Role::User &&
                message.role <= Role::System && message_ids.insert(message.id).second,
                ErrorCode::Schema, "Invalid or duplicate context message source");
        knowledge::validate_text(message.content);
        knowledge::validate_text(message.timestamp, 256);
        require(hash(message.content) == message.content_hash, ErrorCode::Integrity,
                "Context message digest mismatch");
    };
    validate_message(sources.current);
    result.entries.push_back(message_entry(ContextLayer::Current, sources.current));
    for (const auto& instruction : sources.instructions) {
        validate_message(instruction);
        require(instruction.role == Role::User, ErrorCode::Schema, "Non-user context instruction");
        result.entries.push_back(message_entry(ContextLayer::Instruction, instruction));
    }
    std::set<std::string> keys;
    for (const auto& constraint : sources.constraints) {
        require(constraint.active && constraint.revision > 0 && constraint.source_message > 0 &&
                constraint.source_message <= sources.current.id && !constraint.key.empty() &&
                keys.insert(constraint.key).second, ErrorCode::Schema, "Invalid active context pin");
        knowledge::validate_text(constraint.content);
        knowledge::validate_text(constraint.key, 256);
        result.entries.push_back({ContextLayer::Constraint, Role::User, constraint.source_message,
            constraint.revision, 0, 0, constraint.key, {}, constraint.content, hash(constraint.content)});
    }
    std::vector<ContextEntry> optional;
    for (const auto* messages : {&sources.recent, &sources.history}) {
        const auto layer = messages == &sources.recent ? ContextLayer::Recent : ContextLayer::History;
        for (const auto& message : *messages) {
            validate_message(message);
            optional.push_back(message_entry(layer, message));
        }
    }
    if (sources.summary) {
        const auto& summary = *sources.summary;
        require(summary.version > 0 && summary.first_message > 0 &&
                summary.first_message <= summary.last_message &&
                summary.last_message <= sources.current.id, ErrorCode::Schema,
                "Invalid context summary source range");
        knowledge::validate_text(summary.content);
        optional.push_back({ContextLayer::Summary, Role::Assistant, 0, summary.version,
            summary.first_message, summary.last_message, {}, {}, summary.content, hash(summary.content)});
    }
    std::set<std::tuple<std::string, std::string, std::uint64_t, std::uint64_t>> document_ids;
    for (const auto& excerpt : sources.documents) {
        const auto& source = excerpt.source;
        require(source.project == sources.project && !source.project.empty() &&
                (source.session.empty() || source.session == result.session) &&
                source.active && source.revision > 0 && source.content.empty() &&
                !source.key.empty() && !source.uri.empty() && !excerpt.content.empty() &&
                excerpt.offset <= excerpt.source_bytes &&
                excerpt.content.size() <= excerpt.source_bytes - excerpt.offset &&
                source.kind >= DocumentKind::Project && source.kind <= DocumentKind::Summary &&
                source.attribution >= DocumentAttribution::Explicit &&
                source.attribution <= DocumentAttribution::Derived &&
                document_ids.emplace(source.session, source.key, source.revision, excerpt.offset).second,
                ErrorCode::Schema, "Invalid or duplicate context document source");
        knowledge::validate_text(source.project, 256);
        knowledge::validate_text(source.session, 256);
        knowledge::validate_text(source.key, 256);
        knowledge::validate_text(source.uri, 4096);
        knowledge::validate_text(source.timestamp, 256);
        knowledge::validate_text(excerpt.content, 16384);
        require(hash(excerpt.content) == excerpt.content_hash &&
                std::any_of(source.content_hash.begin(), source.content_hash.end(), [](auto b) { return b != 0; }),
                ErrorCode::Integrity, "Context document digest mismatch");
        const auto layer = source.kind == DocumentKind::Project ? ContextLayer::Project :
            source.kind == DocumentKind::Preference ? ContextLayer::Preference :
            source.kind == DocumentKind::Summary ? ContextLayer::History : ContextLayer::Document;
        ContextEntry entry{layer, Role::User, 0, source.revision, 0, 0,
            source.key, source.timestamp, excerpt.content, excerpt.content_hash};
        entry.document = excerpt;
        optional.push_back(std::move(entry));
    }
    std::stable_sort(optional.begin(), optional.end(), [](const ContextEntry& a, const ContextEntry& b) {
        if (a.layer != b.layer) return a.layer < b.layer;
        return a.message > b.message;
    });
    // Charge omission metadata from the start. Full candidate serialization
    // is recounted for every attempted addition: token lengths are not additive
    // or necessarily monotonic across BPE boundaries or chat-template framing.
    for (const auto& entry : optional) ++result.omitted[static_cast<unsigned>(entry.layer)];
    auto fits = [&] {
        std::string json;
        try { json = serialize(result, budget.serialized_bytes); }
        catch (const SerializationLimit&) { return false; }
        auto candidate = result;
        candidate.json = std::move(json);
        const auto tokens = count(candidate);
        if (tokens > budget.input_tokens) return false;
        result.json = std::move(candidate.json);
        result.input_tokens = tokens;
        return true;
    };
    require(fits(), ErrorCode::Budget, "Current request, instructions and pins exceed context budget");
    for (auto& entry : optional) {
        const auto layer = static_cast<unsigned>(entry.layer);
        result.entries.push_back(std::move(entry));
        --result.omitted[layer];
        if (!fits()) {
            result.entries.pop_back();
            ++result.omitted[layer];
        }
    }
    return result;
}

} // namespace axiom::context
