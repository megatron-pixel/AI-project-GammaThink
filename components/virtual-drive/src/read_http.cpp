#include "read_http.hpp"

#include <algorithm>
#include <curl/curl.h>
#include <exception>
#include <limits>
#include <memory>

namespace axiom::knowledge {
namespace {
void require(bool value, ErrorCode code, const char* message) {
    if (!value) throw Error(code, message);
}
struct Transfer {
    Bytes body;
    std::size_t limit, header_limit, headers = 0;
    std::shared_ptr<const std::atomic<bool>> gate;
    bool revoked = false;
    bool exceeded = false;
    std::exception_ptr failure;
};
bool permitted(Transfer& transfer) noexcept {
    if (transfer.revoked) return false;
    if (transfer.gate && !transfer.gate->load(std::memory_order_acquire)) {
        transfer.revoked = true;
        return false;
    }
    return true;
}
int progress_callback(void* context, curl_off_t, curl_off_t, curl_off_t, curl_off_t) noexcept {
    return permitted(*static_cast<Transfer*>(context)) ? 0 : 1;
}
std::size_t body_callback(char* data, std::size_t width, std::size_t count, void* context) noexcept {
    auto& transfer = *static_cast<Transfer*>(context);
    if (!permitted(transfer)) return 0;
    if (width && count > std::numeric_limits<std::size_t>::max() / width) {
        transfer.exceeded = true; return 0;
    }
    const auto size = width * count;
    if (!size) return 0;
    if (size > transfer.limit - transfer.body.size()) { transfer.exceeded = true; return 0; }
    try {
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(data);
        transfer.body.insert(transfer.body.end(), bytes, bytes + size);
    } catch (...) { transfer.failure = std::current_exception(); return 0; }
    return size;
}
std::size_t header_callback(char*, std::size_t width, std::size_t count, void* context) noexcept {
    auto& transfer = *static_cast<Transfer*>(context);
    if (!permitted(transfer)) return 0;
    if (width && count > std::numeric_limits<std::size_t>::max() / width) {
        transfer.exceeded = true; return 0;
    }
    const auto size = width * count;
    if (size > transfer.header_limit - transfer.headers) { transfer.exceeded = true; return 0; }
    transfer.headers += size;
    return size;
}
std::string origin(std::string_view input, bool insecure_loopback) {
    validate_text(input);
    require(!input.empty(), ErrorCode::Schema, "Missing retrieval origin");
    std::unique_ptr<CURLU, decltype(&curl_url_cleanup)> url(curl_url(), curl_url_cleanup);
    require(static_cast<bool>(url), ErrorCode::Storage, "Cannot allocate retrieval URL");
    require(curl_url_set(url.get(), CURLUPART_URL, std::string(input).c_str(), CURLU_DISALLOW_USER) == CURLUE_OK,
            ErrorCode::Schema, "Invalid retrieval origin or URL credentials");
    const auto part = [&](CURLUPart field) {
        char* value = nullptr;
        const auto code = curl_url_get(url.get(), field, &value, 0);
        std::unique_ptr<char, decltype(&curl_free)> owner(value, curl_free);
        require(code == CURLUE_OK && value, ErrorCode::Schema, "Incomplete retrieval origin");
        return std::string(value);
    };
    const auto absent = [&](CURLUPart field, CURLUcode missing) {
        char* value = nullptr;
        const auto code = curl_url_get(url.get(), field, &value, 0);
        curl_free(value);
        return code == missing;
    };
    const auto scheme = part(CURLUPART_SCHEME), host = part(CURLUPART_HOST), path = part(CURLUPART_PATH);
    require(scheme == "https" || (scheme == "http" && insecure_loopback
        && (host == "127.0.0.1" || host == "[::1]")), ErrorCode::Permission, "Retrieval requires HTTPS or explicit loopback HTTP");
    require(path == "/" && absent(CURLUPART_QUERY, CURLUE_NO_QUERY)
        && absent(CURLUPART_FRAGMENT, CURLUE_NO_FRAGMENT),
        ErrorCode::Schema, "Retrieval origin must not contain a path, query or fragment");
    auto result = part(CURLUPART_URL);
    if (result.back() == '/') result.pop_back();
    return result;
}
}
ReadHttpBudget::ReadHttpBudget(ReadHttpBudgetLimits limits) : limits_(limits) {
    require(limits.total_bytes <= 1024ULL * 1024 * 1024 &&
        limits.query_bytes <= 1024ULL * 1024 * 1024,
        ErrorCode::Budget, "Invalid shared retrieval HTTP budget");
}
ReadHttp::ReadHttp(ReadHttpOptions options) : options_(std::move(options)) {
    require(options_.connect_timeout_ms > 0 && options_.connect_timeout_ms <= 60000
        && options_.timeout_ms >= options_.connect_timeout_ms && options_.timeout_ms <= 300000
        && options_.total_bytes <= 1024ULL * 1024 * 1024 && options_.query_bytes <= 1024ULL * 1024 * 1024
        && options_.header_bytes >= 256 && options_.header_bytes <= 1024 * 1024,
        ErrorCode::Budget, "Invalid retrieval HTTP budgets");
    static const auto initialized = curl_global_init(CURL_GLOBAL_DEFAULT);
    require(initialized == CURLE_OK, ErrorCode::Storage, "Cannot initialize retrieval HTTP transport");
    options_.origin = origin(options_.origin, options_.allow_insecure_loopback);
}
Bytes ReadHttp::raw_get(std::string_view target, std::size_t maximum_bytes) {
    return perform(target, false, {}, maximum_bytes);
}
Bytes ReadHttp::query_post(std::string_view target, std::span<const std::uint8_t> body, std::size_t maximum_bytes) {
    return perform(target, true, body, maximum_bytes);
}
Bytes ReadHttp::perform(std::string_view target, bool post, std::span<const std::uint8_t> body,
                        std::size_t maximum_bytes) {
    require(options_.network && (!options_.network_gate ||
        options_.network_gate->load(std::memory_order_acquire)), ErrorCode::Unavailable,
        "Retrieval network access is disabled");
    validate_text(target);
    require(target.starts_with("/") && !target.starts_with("//") && target.find_first_of("\r\n#") == std::string_view::npos,
            ErrorCode::Schema, "Invalid retrieval request target");
    require(body.size() <= 2 * maximum_text_bytes + 256, ErrorCode::Budget, "Query body exceeds protocol bounds");
    require(stats_.requests < options_.requests, ErrorCode::Budget, "Retrieval request budget exceeded");
    require(body.size() <= options_.query_bytes - stats_.query_bytes, ErrorCode::Budget, "Query disclosure byte budget exceeded");
    auto limit = std::min({maximum_bytes, std::size_t{16 * 1024 * 1024}, options_.total_bytes - stats_.accepted_bytes});
    auto* shared = options_.budget.get();
    if (shared) {
        require(shared->stats_.requests < shared->limits_.requests,
                ErrorCode::Budget, "Shared retrieval request budget exceeded");
        require(body.size() <= shared->limits_.query_bytes - shared->stats_.query_bytes,
                ErrorCode::Budget, "Shared query disclosure budget exceeded");
        limit = std::min(limit, shared->limits_.total_bytes - shared->stats_.accepted_bytes);
    }
    require(limit > 0, ErrorCode::Budget, "Retrieval body byte budget exceeded");
    ++stats_.requests; stats_.query_bytes += body.size();
    if (shared) { ++shared->stats_.requests; shared->stats_.query_bytes += body.size(); }
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> request(curl_easy_init(), curl_easy_cleanup);
    require(static_cast<bool>(request), ErrorCode::Storage, "Cannot allocate retrieval request");
    auto* raw_headers = curl_slist_append(nullptr, post
        ? "Accept: application/vnd.gammathink.query-page" : "Accept: application/vnd.ipld.raw");
    require(raw_headers != nullptr, ErrorCode::Storage, "Cannot allocate retrieval headers");
    std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headers(raw_headers, curl_slist_free_all);
    if (post) {
        auto* extended = curl_slist_append(headers.get(), "Content-Type: application/vnd.gammathink.query-request");
        require(extended != nullptr, ErrorCode::Storage, "Cannot allocate query header");
    }
    Transfer transfer{{}, limit, options_.header_bytes, 0, options_.network_gate, false, false, {}};
    const auto url = options_.origin + std::string(target);
    const auto option = [&](CURLoption key, auto value) {
        require(curl_easy_setopt(request.get(), key, value) == CURLE_OK, ErrorCode::Storage, "Cannot configure retrieval request");
    };
    option(CURLOPT_URL, url.c_str());
    if (post) {
        option(CURLOPT_POST, 1L);
        option(CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body.size()));
        option(CURLOPT_POSTFIELDS, body.empty() ? "" : reinterpret_cast<const char*>(body.data()));
    } else option(CURLOPT_HTTPGET, 1L);
    option(CURLOPT_HTTPHEADER, headers.get());
    option(CURLOPT_PROTOCOLS_STR, "https,http");
    option(CURLOPT_FOLLOWLOCATION, 0L);
    option(CURLOPT_PROXY, "");
    option(CURLOPT_NETRC, static_cast<long>(CURL_NETRC_IGNORED));
    option(CURLOPT_NOSIGNAL, 1L);
    option(CURLOPT_SSL_VERIFYPEER, 1L);
    option(CURLOPT_SSL_VERIFYHOST, 2L);
    option(CURLOPT_HTTP_VERSION, static_cast<long>(CURL_HTTP_VERSION_2TLS));
    option(CURLOPT_CONNECTTIMEOUT_MS, options_.connect_timeout_ms);
    option(CURLOPT_TIMEOUT_MS, options_.timeout_ms);
    option(CURLOPT_MAXFILESIZE_LARGE, static_cast<curl_off_t>(limit));
    option(CURLOPT_ACCEPT_ENCODING, "identity");
    option(CURLOPT_HTTP_CONTENT_DECODING, 0L);
    option(CURLOPT_WRITEFUNCTION, body_callback);
    option(CURLOPT_WRITEDATA, &transfer);
    option(CURLOPT_HEADERFUNCTION, header_callback);
    option(CURLOPT_HEADERDATA, &transfer);
    option(CURLOPT_NOPROGRESS, 0L);
    option(CURLOPT_XFERINFOFUNCTION, progress_callback);
    option(CURLOPT_XFERINFODATA, &transfer);
    const auto status = curl_easy_perform(request.get());
    stats_.accepted_bytes += transfer.body.size();
    if (shared) shared->stats_.accepted_bytes += transfer.body.size();
    if (transfer.failure) std::rethrow_exception(transfer.failure);
    require(permitted(transfer), ErrorCode::Unavailable, "Retrieval network capability was revoked");
    require(!transfer.exceeded && status != CURLE_FILESIZE_EXCEEDED && status != CURLE_OPERATION_TIMEDOUT,
            ErrorCode::Budget, "Retrieval transfer budget exceeded");
    require(status == CURLE_OK, ErrorCode::Unavailable, "Retrieval transfer failed");
    long response = 0;
    require(curl_easy_getinfo(request.get(), CURLINFO_RESPONSE_CODE, &response) == CURLE_OK,
            ErrorCode::Unavailable, "Missing retrieval response status");
    require(response == 200, ErrorCode::Unavailable, "Retrieval unavailable or redirect refused");
    return std::move(transfer.body);
}
} // namespace axiom::knowledge
