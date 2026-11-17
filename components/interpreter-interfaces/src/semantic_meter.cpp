#include "semantic_meter.hpp"

#include <curl/curl.h>
#include <json-c/json.h>

#include <algorithm>
#include <limits>
#include <memory>
#include <string_view>

namespace axiom {
namespace {
constexpr std::size_t max_bytes = 1048576;
using J = json_object*;
using Json = std::unique_ptr<json_object, decltype(&json_object_put)>;
void require(bool condition, const char* message) {
    if (!condition) throw InterpretationAccountingError(message);
}
Json parse(const std::string& input) {
    require(!input.empty() && input.size() <= max_bytes && input.find('\0') == std::string::npos,
        "Invalid metered JSON size");
    const auto tokener = std::unique_ptr<json_tokener, decltype(&json_tokener_free)>(
        json_tokener_new_ex(48),json_tokener_free);
    json_tokener_set_flags(tokener.get(),JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
    Json value(json_tokener_parse_ex(tokener.get(),input.data(),static_cast<int>(input.size())),json_object_put);
    require(value && json_tokener_get_error(tokener.get()) == json_tokener_success
        && json_object_get_type(value.get()) == json_type_object,"Malformed metered JSON");
    for (auto i = json_tokener_get_parse_end(tokener.get()); i < input.size(); ++i)
        require(input[i] == ' ' || input[i] == '\r' || input[i] == '\n' || input[i] == '\t',
            "Trailing metered JSON");
    return value;
}
J field(J value, const char* key, json_type type) {
    J result = nullptr;
    require(json_object_get_type(value) == json_type_object
        && json_object_object_get_ex(value,key,&result) && json_object_get_type(result) == type,
        "Missing or mistyped metered field");
    return result;
}
std::string string(J value, const char* key, bool empty = false) {
    const auto item = field(value,key,json_type_string);
    std::string result(json_object_get_string(item),json_object_get_string_len(item));
    require((empty || !result.empty()) && result.find('\0') == std::string::npos,
        "Invalid metered text");
    return result;
}
std::uint64_t number(J value, const char* key) {
    const auto item = field(value,key,json_type_int);
    const auto result = json_object_get_int64(item);
    require(result >= 0 && static_cast<std::uint64_t>(result) <= std::numeric_limits<std::uint32_t>::max(),
        "Invalid token usage count");
    return static_cast<std::uint64_t>(result);
}
J text(std::string_view value) {
    return json_object_new_string_len(value.data(),static_cast<int>(value.size()));
}
std::string dump(J value) { return json_object_to_json_string_ext(value,JSON_C_TO_STRING_PLAIN); }
struct Transfer {
    std::string& output;
    std::size_t& total;
    bool exceeded = false;
};
std::size_t receive(char* data, std::size_t size, std::size_t count, void* context) {
    auto& transfer = *static_cast<Transfer*>(context);
    if (size && count > max_bytes / size) { transfer.exceeded = true; return 0; }
    const auto length = size * count;
    if (length > max_bytes - transfer.total) {
        transfer.total += length; // Count the observed chunk even though it is not retained.
        transfer.exceeded = true;
        return 0;
    }
    try {
        transfer.output.append(data,length);
        transfer.total += length;
    } catch (const std::exception&) { return 0; }
    return length;
}
struct CurlRuntime {
    CurlRuntime() {
        if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
            throw std::runtime_error("Cannot initialize metered HTTP client");
    }
    ~CurlRuntime() { curl_global_cleanup(); }
};
}

MeteredSemanticSession::MeteredSemanticSession(bool staged, std::uint16_t port, Clock::time_point start,
    SemanticSamplingProfile sampling, SemanticBudgetProfile budget)
    : staged_(staged), sampling_(sampling), budget_(budget), port_(port),
      deadline_(start + std::chrono::milliseconds(180000)) {
    if (!port) throw std::invalid_argument("Invalid local interpreter port");
    if (sampling != SemanticSamplingProfile::GreedyControl && sampling != SemanticSamplingProfile::QwenNonThinking)
        throw std::invalid_argument("Invalid interpreter sampling profile");
    if ((budget != SemanticBudgetProfile::Standard && budget != SemanticBudgetProfile::QuantityDecisionsExperimental)
        || (staged && budget != SemanticBudgetProfile::Standard))
        throw std::invalid_argument("Invalid interpreter budget profile");
}
std::int64_t MeteredSemanticSession::remaining_ms() const {
    const auto now = Clock::now();
    if (now >= deadline_) return 0;
    return std::chrono::ceil<std::chrono::milliseconds>(deadline_ - now).count();
}
void MeteredSemanticSession::ensure_time() const {
    if (!remaining_ms()) throw InterpretationLimit("Request-wide interpretation deadline exceeded");
}
std::int64_t MeteredSemanticSession::elapsed_ms() const {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::now()-(deadline_-std::chrono::milliseconds(180000))).count();
}
std::string MeteredSemanticSession::post(const std::string& path, const std::string& payload) {
    ensure_time();
    static CurlRuntime runtime;
    (void)runtime;
    exchanges_.push_back({path,payload,{},0,0});
    auto& exchange = exchanges_.back();
    const auto start = Clock::now();
    const auto handle = std::unique_ptr<CURL, decltype(&curl_easy_cleanup)>(curl_easy_init(),curl_easy_cleanup);
    if (!handle) throw std::runtime_error("Cannot create metered HTTP client");
    const auto headers = std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)>(
        curl_slist_append(nullptr,"Content-Type: application/json"),curl_slist_free_all);
    const auto set = [&](CURLoption option, auto setting) {
        if (curl_easy_setopt(handle.get(),option,setting) != CURLE_OK)
            throw std::runtime_error("Cannot configure metered HTTP client");
    };
    const auto url = "http://127.0.0.1:"+std::to_string(port_)+path;
    Transfer transfer{exchange.response,response_bytes_};
    set(CURLOPT_URL,url.c_str());
    set(CURLOPT_PROXY,"");
    set(CURLOPT_CONNECTTIMEOUT_MS,std::min<long>(1000,remaining_ms()));
    const auto timeout = remaining_ms();
    if (!timeout) throw InterpretationLimit("Request-wide interpretation deadline exceeded");
    set(CURLOPT_TIMEOUT_MS,static_cast<long>(timeout));
    set(CURLOPT_NOSIGNAL,1L);
    set(CURLOPT_HTTPHEADER,headers.get());
    set(CURLOPT_POSTFIELDS,payload.data());
    set(CURLOPT_POSTFIELDSIZE,static_cast<long>(payload.size()));
    set(CURLOPT_WRITEFUNCTION,&receive);
    set(CURLOPT_WRITEDATA,&transfer);
    const auto code = curl_easy_perform(handle.get());
    exchange.elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-start).count();
    (void)curl_easy_getinfo(handle.get(),CURLINFO_RESPONSE_CODE,&exchange.status);
    if (transfer.exceeded) throw InterpretationLimit("Shared interpretation response-byte limit exceeded");
    if (code == CURLE_OPERATION_TIMEDOUT) throw InterpretationLimit("Request-wide interpretation deadline exceeded");
    if (code != CURLE_OK)
        throw std::runtime_error(std::string("Metered interpreter unavailable: ")+curl_easy_strerror(code));
    if (exchange.status != 200)
        throw std::runtime_error("Metered interpreter HTTP error: "+std::to_string(exchange.status));
    return exchange.response;
}

SemanticPromptPreflight MeteredSemanticSession::prepare(const std::string& instructions,
    const std::string& input, const std::string& schema,
    std::optional<std::uint32_t> lower_output_limit) try {
    if (closed_ || calls_ >= call_limit())
        throw InterpretationLimit("Interpretation session is closed");
    ensure_time();
    if (instructions.empty() || instructions.size() > 8192 || input.empty() || input.size() > max_bytes
        || instructions.find('\0') != std::string::npos || input.find('\0') != std::string::npos)
        throw std::invalid_argument("Invalid metered interpretation request");
    if (completion_tokens_ >= completion_limit())
        throw InterpretationLimit("Aggregate interpretation output limit exceeded");
    const auto allocated = budget_ == SemanticBudgetProfile::QuantityDecisionsExperimental
        ? std::min<std::uint32_t>(64,completion_limit()-completion_tokens_)
        : staged_ ? (calls_ == 0 ? 512U : 1024U) : 1536U;
    const auto output_cap = lower_output_limit.value_or(allocated);
    if (!output_cap || output_cap > allocated)
        throw std::invalid_argument("Requested output limit exceeds stage allocation");
    Json body(json_object_new_object(),json_object_put);
    auto messages = json_object_new_array();
    json_object_object_add(body.get(),"messages",messages);
    for (const auto& [role,content] : {std::pair{"system",instructions},std::pair{"user",input}}) {
        auto message = json_object_new_object();
        json_object_object_add(message,"role",text(role));
        json_object_object_add(message,"content",text(content));
        json_object_array_add(messages,message);
    }
    if (sampling_ == SemanticSamplingProfile::QwenNonThinking) {
        json_object_object_add(body.get(),"temperature",json_object_new_double(0.7));
        json_object_object_add(body.get(),"top_p",json_object_new_double(0.8));
        json_object_object_add(body.get(),"top_k",json_object_new_int(20));
        json_object_object_add(body.get(),"min_p",json_object_new_int(0));
    } else json_object_object_add(body.get(),"temperature",json_object_new_int(0));
    json_object_object_add(body.get(),"seed",json_object_new_int(20261015));
    json_object_object_add(body.get(),"max_tokens",json_object_new_int64(output_cap));
    auto options = json_object_new_object();
    json_object_object_add(options,"enable_thinking",json_object_new_boolean(false));
    json_object_object_add(body.get(),"chat_template_kwargs",options);
    auto format = json_object_new_object(), specification = json_object_new_object();
    json_object_object_add(format,"type",text("json_schema"));
    json_object_object_add(specification,"name",text("problem_proposal"));
    json_object_object_add(specification,"strict",json_object_new_boolean(true));
    json_object_object_add(specification,"schema",parse(schema).release());
    json_object_object_add(format,"json_schema",specification);
    json_object_object_add(body.get(),"response_format",format);
    const auto payload = dump(body.get());
    const auto rendered = parse(post("/apply-template",payload));
    Json tokenizer(json_object_new_object(),json_object_put);
    json_object_object_add(tokenizer.get(),"content",text(string(rendered.get(),"prompt")));
    // Pinned handle_completions_impl uses tokenize_input_prompts(...,true,true).
    json_object_object_add(tokenizer.get(),"add_special",json_object_new_boolean(true));
    json_object_object_add(tokenizer.get(),"parse_special",json_object_new_boolean(true));
    const auto tokenized = parse(post("/tokenize",dump(tokenizer.get())));
    const auto tokens = field(tokenized.get(),"tokens",json_type_array);
    const auto prompt_count = json_object_array_length(tokens);
    for (std::size_t i = 0; i < prompt_count; ++i) {
        const auto token = json_object_array_get_idx(tokens,i);
        require(json_object_get_type(token) == json_type_int && json_object_get_int64(token) >= 0,
            "Invalid preflight token ID");
    }
    require(prompt_count > 0,"Empty rendered prompt");
    const bool within_limits = prompt_count <= prompt_limit() - prompt_tokens_
        && prompt_count + output_cap <= 8192 && output_cap <= completion_limit() - completion_tokens_;
    ensure_time();
    return {payload,static_cast<std::uint32_t>(prompt_count),output_cap,within_limits};
} catch (...) {
    closed_ = true;
    throw;
}

SemanticPromptPreflight MeteredSemanticSession::preflight(const std::string& instructions,
    const std::string& input, const std::string& schema) try {
    if (calls_ || !exchanges_.empty())
        throw std::invalid_argument("Preflight requires a fresh session");
    auto result = prepare(instructions,input,schema,{});
    closed_ = true;
    return result;
} catch (...) {
    closed_ = true;
    throw;
}

MeteredSemanticCompletion MeteredSemanticSession::complete(const std::string& instructions,
    const std::string& input, const std::string& schema,
    std::optional<std::uint32_t> lower_output_limit) try {
    const auto prepared = prepare(instructions,input,schema,lower_output_limit);
    if (!prepared.within_limits)
        throw InterpretationLimit("Aggregate interpretation token limit exceeded before generation");
    ++calls_;
    accounting_complete_ = false;
    const auto response = parse(post("/v1/chat/completions",prepared.request));
    MeteredSemanticCompletion result;
    result.preflight_prompt_tokens = prepared.prompt_tokens;
    result.requested_output_tokens = prepared.requested_output_tokens;
    result.model = string(response.get(),"model");
    const auto choices = field(response.get(),"choices",json_type_array);
    require(json_object_array_length(choices) == 1,"Expected one metered completion");
    const auto choice = json_object_array_get_idx(choices,0);
    const auto message = field(choice,"message",json_type_object);
    require(string(message,"role") == "assistant","Invalid metered completion role");
    result.content = string(message,"content",true);
    result.finish_reason = string(choice,"finish_reason");
    const auto usage = field(response.get(),"usage",json_type_object);
    result.usage = {number(usage,"prompt_tokens"),number(usage,"completion_tokens"),number(usage,"total_tokens"),
        number(field(usage,"prompt_tokens_details",json_type_object),"cached_tokens")};
    require(result.usage.total == result.usage.prompt + result.usage.completion
        && result.usage.cached_prompt <= result.usage.prompt,"Inconsistent metered token usage");
    prompt_tokens_ += result.usage.prompt;
    completion_tokens_ += result.usage.completion;
    cached_prompt_tokens_ += result.usage.cached_prompt;
    require(result.usage.prompt == prepared.prompt_tokens,"Rendered prompt usage mismatch");
    require(result.usage.completion <= prepared.requested_output_tokens,"Completion exceeds requested output cap");
    require(prompt_tokens_ <= prompt_limit() && completion_tokens_ <= completion_limit(),
        "Reported usage exceeds aggregate caps");
    accounting_complete_ = true;
    closed_ = result.finish_reason != "stop";
    ensure_time();
    return result;
} catch (...) {
    closed_ = true;
    throw;
}

} // namespace axiom
