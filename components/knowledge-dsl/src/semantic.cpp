#include "semantic.hpp"
#include "semantic_prompt.hpp"

#include <curl/curl.h>
#include <json-c/json.h>

#include <initializer_list>
#include <memory>
#include <stdexcept>

namespace axiom {
namespace {
using Json = std::unique_ptr<json_object, decltype(&json_object_put)>;

Json own(json_object* value) { return Json(value, json_object_put); }

Json parse(const std::string& text) {
    if (text.empty() || text.size() > max_text_bytes || text.find('\0') != std::string::npos) {
        throw std::runtime_error("Invalid semantic JSON size or embedded null");
    }
    const auto tok = std::unique_ptr<json_tokener, decltype(&json_tokener_free)>(
        json_tokener_new(), json_tokener_free);
    json_tokener_set_flags(tok.get(), JSON_TOKENER_STRICT);
    auto value = own(json_tokener_parse_ex(tok.get(), text.c_str(), static_cast<int>(text.size())));
    if (json_tokener_get_error(tok.get()) != json_tokener_success || !value
        || json_object_get_type(value.get()) != json_type_object) {
        throw std::runtime_error("Malformed semantic JSON object");
    }
    return value;
}

json_object* member(json_object* value, const char* key, json_type type) {
    json_object* result = nullptr;
    if (!json_object_object_get_ex(value, key, &result)
        || json_object_get_type(result) != type) {
        throw std::runtime_error(std::string("Invalid semantic field: ") + key);
    }
    return result;
}

std::string text(json_object* value, const char* key) {
    const auto field = member(value, key, json_type_string);
    std::string result(json_object_get_string(field),
                       static_cast<std::size_t>(json_object_get_string_len(field)));
    if (result.empty() || result.find('\0') != std::string::npos) {
        throw std::runtime_error(std::string("Empty or invalid semantic field: ") + key);
    }
    return result;
}

void fields(json_object* value, std::initializer_list<const char*> allowed) {
    if (static_cast<std::size_t>(json_object_object_length(value)) != allowed.size()) {
        throw std::runtime_error("Unexpected semantic fields");
    }
    for (const auto key : allowed) (void)text(value, key);
}

Json schema() {
    auto root = own(json_object_new_object());
    auto alternatives = json_object_new_array();
    const auto add = [&](const char* operation, std::initializer_list<const char*> keys) {
        auto item = json_object_new_object();
        json_object_object_add(item, "type", json_object_new_string("object"));
        json_object_object_add(item, "additionalProperties", json_object_new_boolean(false));
        auto properties = json_object_new_object();
        auto required = json_object_new_array();
        auto op = json_object_new_object();
        json_object_object_add(op, "const", json_object_new_string(operation));
        json_object_object_add(properties, "operation", op);
        json_object_array_add(required, json_object_new_string("operation"));
        for (const auto key : keys) {
            auto field = json_object_new_object();
            json_object_object_add(field, "type", json_object_new_string("string"));
            json_object_object_add(field, "minLength", json_object_new_int(1));
            json_object_object_add(properties, key, field);
            json_object_array_add(required, json_object_new_string(key));
        }
        json_object_object_add(item, "properties", properties);
        json_object_object_add(item, "required", required);
        json_object_array_add(alternatives, item);
    };
    add("eval", {"expression"});
    add("solve", {"expression"});
    add("det", {"matrix"});
    add("linear", {"matrix", "rhs"});
    add("born", {"matrix"});
    add("prove", {"goal"});
    add("research", {"query"});
    add("library_calculation", {"query", "quantity", "unit", "expression"});
    add("clarify", {"reason"});
    add("unsupported", {"reason"});
    json_object_object_add(root.get(), "oneOf", alternatives);
    return root;
}

Json request_body(const std::string& question) {
    auto body = own(json_object_new_object());
    auto messages = json_object_new_array();
    for (const auto& [role, content] :
         {std::pair{"system", std::string(semantic_instructions)}, std::pair{"user", question}}) {
        auto message = json_object_new_object();
        json_object_object_add(message, "role", json_object_new_string(role));
        json_object_object_add(message, "content", json_object_new_string(content.c_str()));
        json_object_array_add(messages, message);
    }
    json_object_object_add(body.get(), "messages", messages);
    json_object_object_add(body.get(), "temperature", json_object_new_int(0));
    json_object_object_add(body.get(), "seed", json_object_new_int(20261015));
    json_object_object_add(body.get(), "max_tokens", json_object_new_int(256));
    auto options = json_object_new_object();
    json_object_object_add(options, "enable_thinking", json_object_new_boolean(false));
    json_object_object_add(body.get(), "chat_template_kwargs", options);
    auto format = json_object_new_object();
    json_object_object_add(format, "type", json_object_new_string("json_schema"));
    auto specification = json_object_new_object();
    json_object_object_add(specification, "name", json_object_new_string("interpretation"));
    json_object_object_add(specification, "strict", json_object_new_boolean(true));
    json_object_object_add(specification, "schema", schema().release());
    json_object_object_add(format, "json_schema", specification);
    json_object_object_add(body.get(), "response_format", format);
    return body;
}

std::size_t receive(char* data, std::size_t size, std::size_t count, void* output) {
    auto& result = *static_cast<std::string*>(output);
    if (size != 0 && count > max_text_bytes / size) return 0;
    const auto length = size * count;
    if (length > max_text_bytes - result.size()) return 0;
    try {
        result.append(data, length);
    } catch (const std::exception&) {
        return 0; // No exception may cross libcurl's C callback.
    }
    return length;
}
} // namespace

EnglishRequest parse_semantic_interpretation(const std::string& json) {
    const auto value = parse(json);
    const auto op = text(value.get(), "operation");
    EnglishRequest result;
    if (op == "eval" || op == "solve") {
        fields(value.get(), {"operation", "expression"});
        result.command = {op, text(value.get(), "expression")};
    } else if (op == "det" || op == "born") {
        fields(value.get(), {"operation", "matrix"});
        result.command = {op, text(value.get(), "matrix")};
    } else if (op == "linear") {
        fields(value.get(), {"operation", "matrix", "rhs"});
        result.command = {op, text(value.get(), "matrix"), text(value.get(), "rhs")};
    } else if (op == "prove") {
        fields(value.get(), {"operation", "goal"});
        result.kind = EnglishKind::Proof;
        result.goal = text(value.get(), "goal");
        result.query = result.goal;
        return result;
    } else if (op == "research") {
        fields(value.get(), {"operation", "query"});
        result.kind = EnglishKind::Research;
        result.query = text(value.get(), "query");
        return result;
    } else if (op == "library_calculation") {
        fields(value.get(), {"operation", "query", "quantity", "unit", "expression"});
        result.kind = EnglishKind::LibraryCalculation;
        result.query = text(value.get(), "query");
        result.quantity = text(value.get(), "quantity");
        result.unit = text(value.get(), "unit");
        result.expression = text(value.get(), "expression");
        return result;
    } else if (op == "clarify" || op == "unsupported") {
        fields(value.get(), {"operation", "reason"});
        throw std::runtime_error(op + ": " + text(value.get(), "reason"));
    } else {
        throw std::runtime_error("Unknown semantic operation");
    }
    (void)compile(result.command); // Reject non-representable math before execution.
    return result;
}

static std::string complete_local_json(const std::string& payload, long timeout_ms) {
    const auto initialized = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (initialized != CURLE_OK) throw std::runtime_error("Cannot initialize semantic HTTP client");
    struct Cleanup { ~Cleanup() { curl_global_cleanup(); } } cleanup;
    const auto handle = std::unique_ptr<CURL, decltype(&curl_easy_cleanup)>(
        curl_easy_init(), curl_easy_cleanup);
    if (!handle) throw std::runtime_error("Cannot create semantic HTTP client");
    std::string response;
    const auto headers = std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)>(
        curl_slist_append(nullptr, "Content-Type: application/json"), curl_slist_free_all);
    const auto set = [&](CURLoption option, auto setting) {
        if (curl_easy_setopt(handle.get(), option, setting) != CURLE_OK) {
            throw std::runtime_error("Cannot configure semantic HTTP client");
        }
    };
    set(CURLOPT_URL, "http://127.0.0.1:18089/v1/chat/completions");
    set(CURLOPT_PROXY, "");
    set(CURLOPT_CONNECTTIMEOUT_MS, 1000L);
    set(CURLOPT_TIMEOUT_MS, timeout_ms);
    set(CURLOPT_NOSIGNAL, 1L);
    set(CURLOPT_HTTPHEADER, headers.get());
    set(CURLOPT_POSTFIELDS, payload.c_str());
    set(CURLOPT_WRITEFUNCTION, &receive);
    set(CURLOPT_WRITEDATA, &response);
    const auto code = curl_easy_perform(handle.get());
    if (code != CURLE_OK) {
        throw std::runtime_error(std::string("Local interpreter unavailable: ") + curl_easy_strerror(code));
    }
    long status = 0;
    if (curl_easy_getinfo(handle.get(), CURLINFO_RESPONSE_CODE, &status) != CURLE_OK || status != 200) {
        throw std::runtime_error("Local interpreter HTTP error: " + std::to_string(status));
    }
    const auto envelope = parse(response);
    const auto choices = member(envelope.get(), "choices", json_type_array);
    if (json_object_array_length(choices) != 1) throw std::runtime_error("Expected one interpretation");
    const auto choice = json_object_array_get_idx(choices, 0);
    const auto message = member(choice, "message", json_type_object);
    if (text(message, "role") != "assistant") throw std::runtime_error("Invalid interpreter role");
    const auto content = text(message, "content");
    if (text(choice, "finish_reason") != "stop") throw IncompleteInterpretation(content);
    return content;
}

std::string interpret_local_json(const std::string& question) {
    if (question.empty() || question.size() > 4096 || question.find('\0') != std::string::npos) {
        throw std::runtime_error("Semantic question must contain 1 to 4096 non-null bytes");
    }
    const auto body = request_body(question);
    return complete_local_json(json_object_to_json_string_ext(body.get(), JSON_C_TO_STRING_PLAIN),30000L);
}

std::string interpret_local_proposal_json(const std::string& instructions,
    const std::string& input, const std::string& constraint, std::uint32_t max_tokens,
    ProposalOutputFormat output_format) {
    if (instructions.empty() || instructions.size() > 8192 || input.empty()
        || input.size() > 131072 || input.find('\0') != std::string::npos
        || instructions.find('\0') != std::string::npos || max_tokens == 0 || max_tokens > 2048) {
        throw std::runtime_error("Invalid problem interpretation request bounds");
    }
    auto body = request_body("proposal");
    auto messages = json_object_new_array();
    for (const auto& [role,content] : {std::pair{"system",instructions},std::pair{"user",input}}) {
        auto message = json_object_new_object();
        json_object_object_add(message,"role",json_object_new_string(role));
        json_object_object_add(message,"content",json_object_new_string_len(content.data(),content.size()));
        json_object_array_add(messages,message);
    }
    json_object_object_add(body.get(),"messages",messages);
    json_object_object_add(body.get(),"max_tokens",json_object_new_int64(max_tokens));
    switch (output_format) {
    case ProposalOutputFormat::JsonSchema: {
        auto format = member(body.get(),"response_format",json_type_object);
        auto specification = member(format,"json_schema",json_type_object);
        json_object_object_add(specification,"name",json_object_new_string("problem_proposal"));
        json_object_object_add(specification,"schema",parse(constraint).release());
        break;
    }
    case ProposalOutputFormat::LarkGrammar: {
        if (constraint.empty() || constraint.size() > max_text_bytes
            || constraint.find('\0') != std::string::npos) {
            throw std::runtime_error("Invalid output grammar");
        }
        auto format = json_object_new_object();
        json_object_object_add(format, "type", json_object_new_string("grammar"));
        auto specification = json_object_new_object();
        json_object_object_add(specification, "name", json_object_new_string("comprehension_dsl_v1"));
        json_object_object_add(specification, "syntax", json_object_new_string("lark"));
        json_object_object_add(specification, "definition", json_object_new_string_len(
            constraint.data(), static_cast<int>(constraint.size())));
        json_object_object_add(format, "grammar", specification);
        json_object_object_add(body.get(), "response_format", format);
        break;
    }
    }
    return complete_local_json(json_object_to_json_string_ext(body.get(),JSON_C_TO_STRING_PLAIN),180000L);
}

void verify_local_interpreter_context(std::uint32_t expected_tokens) {
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        throw std::runtime_error("Cannot initialize semantic HTTP client");
    }
    struct Cleanup { ~Cleanup() { curl_global_cleanup(); } } cleanup;
    const auto handle = std::unique_ptr<CURL, decltype(&curl_easy_cleanup)>(
        curl_easy_init(), curl_easy_cleanup);
    if (!handle) throw std::runtime_error("Cannot create semantic HTTP client");
    const auto set = [&](CURLoption option, auto setting) {
        if (curl_easy_setopt(handle.get(), option, setting) != CURLE_OK) {
            throw std::runtime_error("Cannot configure semantic context check");
        }
    };
    std::string response;
    set(CURLOPT_URL, "http://127.0.0.1:18089/props");
    set(CURLOPT_PROXY, "");
    set(CURLOPT_CONNECTTIMEOUT_MS, 1000L);
    set(CURLOPT_TIMEOUT_MS, 5000L);
    set(CURLOPT_NOSIGNAL, 1L);
    set(CURLOPT_WRITEFUNCTION, &receive);
    set(CURLOPT_WRITEDATA, &response);
    const auto code = curl_easy_perform(handle.get());
    if (code != CURLE_OK) {
        throw std::runtime_error(std::string("Local interpreter context unavailable: ")
                                 + curl_easy_strerror(code));
    }
    long status = 0;
    if (curl_easy_getinfo(handle.get(), CURLINFO_RESPONSE_CODE, &status) != CURLE_OK || status != 200) {
        throw std::runtime_error("Local interpreter context HTTP error: " + std::to_string(status));
    }
    const auto props = parse(response);
    const auto settings = member(props.get(), "default_generation_settings", json_type_object);
    const auto actual = json_object_get_int64(member(settings, "n_ctx", json_type_int));
    if (actual != expected_tokens) {
        throw std::runtime_error("Interpreter context mismatch: expected "
            + std::to_string(expected_tokens) + ", actual " + std::to_string(actual));
    }
}

EnglishRequest interpret_local(const std::string& question) {
    const auto interpretation = interpret_local_json(question);
    try {
        return parse_semantic_interpretation(interpretation);
    } catch (const std::runtime_error& error) {
        throw std::runtime_error(std::string(error.what()) + "; rejected interpretation: "
                                 + interpretation);
    }
}
} // namespace axiom
