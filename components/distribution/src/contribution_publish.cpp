#include "contribution_publish.hpp"

#include <curl/curl.h>
#include <json-c/json.h>

#include <algorithm>
#include <deque>
#include <map>
#include <mutex>
#include <set>

namespace axiom::knowledge {
namespace {
void require(bool value, ErrorCode code, const char* message) {
    if (!value) throw Error(code, message);
}
void validate_limits(const PublicationLimits& limits) {
    require(limits.blocks > 0 && limits.blocks <= 65536 && limits.payload_bytes > 0
        && limits.payload_bytes <= 256 * 1024 * 1024 && limits.operation_time.count() > 0
        && limits.record_bytes > 0 && limits.record_bytes <= 256 * 1024 * 1024
        && limits.operation_time <= std::chrono::minutes(5), ErrorCode::Budget, "Invalid publication limits");
}
struct UrlDelete { void operator()(CURLU* p) const { curl_url_cleanup(p); } };
struct CurlDelete { void operator()(CURL* p) const { curl_easy_cleanup(p); } };
struct MimeDelete { void operator()(curl_mime* p) const { curl_mime_free(p); } };
struct HeaderDelete { void operator()(curl_slist* p) const { curl_slist_free_all(p); } };
struct JsonDelete { void operator()(json_object* p) const { json_object_put(p); } };
struct TokenerDelete { void operator()(json_tokener* p) const { json_tokener_free(p); } };
std::string part(CURLU* url, CURLUPart component, bool optional = false) {
    char* value = nullptr;
    const auto code = curl_url_get(url, component, &value, 0);
    if (optional && (code == CURLUE_NO_QUERY || code == CURLUE_NO_FRAGMENT || code == CURLUE_NO_USER
        || code == CURLUE_NO_PASSWORD || code == CURLUE_NO_OPTIONS || code == CURLUE_NO_PORT)) return {};
    require(code == CURLUE_OK && value, ErrorCode::Schema, "Invalid Kubo origin");
    std::string result(value); curl_free(value); return result;
}
struct Transfer {
    const std::shared_ptr<std::atomic<bool>>& owner;
    const std::shared_ptr<std::atomic<bool>>& permit;
    std::string response;
    std::size_t headers = 0;
    bool exceeded = false;
};
size_t receive(char* bytes, size_t size, size_t count, void* raw) {
    auto& transfer = *static_cast<Transfer*>(raw);
    if (size && count > 8192 / size) { transfer.exceeded = true; return 0; }
    const auto length = size * count;
    if (length > 8192 - transfer.response.size()) { transfer.exceeded = true; return 0; }
    transfer.response.append(bytes, length); return length;
}
size_t header(char*, size_t size, size_t count, void* raw) {
    auto& transfer = *static_cast<Transfer*>(raw);
    if (size && count > 8192 / size) { transfer.exceeded = true; return 0; }
    const auto length = size * count;
    if (length > 8192 - transfer.headers) { transfer.exceeded = true; return 0; }
    transfer.headers += length; return length;
}
int progress(void* raw, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    const auto& transfer = *static_cast<Transfer*>(raw);
    return !transfer.owner->load() || !transfer.permit->load();
}
void acknowledge(std::string_view text, const BlockId& block, std::size_t bytes) {
    std::unique_ptr<json_tokener, TokenerDelete> parser(json_tokener_new_ex(8));
    require(bool(parser), ErrorCode::Storage, "Cannot allocate publication response parser");
    json_tokener_set_flags(parser.get(), JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
    std::unique_ptr<json_object, JsonDelete> value(json_tokener_parse_ex(parser.get(), text.data(), static_cast<int>(text.size())));
    require(json_tokener_get_error(parser.get()) == json_tokener_success && value
        && json_tokener_get_parse_end(parser.get()) == text.size()
        && json_object_is_type(value.get(), json_type_object)
        && json_object_object_length(value.get()) == 2, ErrorCode::Schema, "Invalid Kubo block response");
    json_object *key = nullptr, *size = nullptr;
    require(json_object_object_get_ex(value.get(), "Key", &key) && json_object_is_type(key, json_type_string)
        && json_object_object_get_ex(value.get(), "Size", &size) && json_object_is_type(size, json_type_int)
        && json_object_get_int64(size) >= 0 && json_object_get_uint64(size) == bytes,
        ErrorCode::Integrity, "Kubo block size acknowledgement differs");
    const auto cid = parse_block_cid(std::string_view(json_object_get_string(key), json_object_get_string_len(key)));
    require(block_id(cid) == block, ErrorCode::Integrity, "Kubo block identity acknowledgement differs");
}
}

static PublicationPlan inspect_snapshot(GksSnapshot& snapshot, const BlockId& root,
                                       std::span<const ObjectId> roots, std::span<const ObjectId> selection,
                                       PublicationLimits limits) {
    const auto deadline = std::chrono::steady_clock::now() + limits.operation_time;
    require(!roots.empty() && roots.size() <= selection.size() && !selection.empty()
        && selection.size() <= maximum_items, ErrorCode::Schema, "Invalid prepared contribution selection");
    const std::set<ObjectId> expected(selection.begin(), selection.end());
    require(expected.size() == selection.size(), ErrorCode::Schema, "Duplicate prepared contribution identity");
    for (const auto& id : roots)
        require(expected.contains(id), ErrorCode::Integrity, "Contribution root is not in the selection");
    require(snapshot.manifest().object_count == expected.size(), ErrorCode::Integrity, "Contribution object count differs");
    PublicationPlan plan{root, {}, 0};
    std::set<BlockId> included, chunks;
    std::set<ObjectId> objects;
    std::map<ObjectId, std::vector<ObjectId>> dependencies;
    std::uint64_t record_bytes = 0;
    const auto add = [&](const BlockId& id) {
        require(std::chrono::steady_clock::now() < deadline, ErrorCode::Budget, "Publication inspection time budget exceeded");
        if (included.contains(id)) return;
        require(included.size() < limits.blocks, ErrorCode::Budget, "Publication block limit exceeded");
        const auto wire = snapshot.block(id);
        require(wire.size() <= 2 * 1024 * 1024, ErrorCode::Budget, "Publication exceeds Kubo 2 MiB block profile");
        require(wire.size() <= limits.payload_bytes - plan.payload_bytes, ErrorCode::Budget, "Publication byte limit exceeded");
        included.insert(id); plan.payload_bytes += wire.size();
    };
    add(plan.root);
    struct Pending { BlockId id; std::string format; std::uint32_t level; };
    std::deque<Pending> pending;
    for (const auto& index : snapshot.manifest().indexes)
        pending.push_back({index.block, index.key_format, index.level});
    std::map<BlockId, std::pair<std::string, std::uint32_t>> indexes;
    while (!pending.empty()) {
        const auto item = std::move(pending.front()); pending.pop_front();
        if (const auto found = indexes.find(item.id); found != indexes.end()) {
            require(found->second == std::pair(item.format, item.level), ErrorCode::Integrity, "Conflicting index interpretation");
            continue;
        }
        require(item.id != plan.root && !chunks.contains(item.id), ErrorCode::Integrity, "Conflicting publication block kind");
        add(item.id); indexes.emplace(item.id, std::pair(item.format, item.level));
        const auto index = decode_index_chunk(item.id, snapshot.block(item.id));
        require(index.level == item.level && index.key_format == item.format,
                ErrorCode::Integrity, "Publication index contract differs");
        if (index.level) {
            for (const auto& child : index.children) {
                require(pending.size() < limits.blocks, ErrorCode::Budget, "Publication index frontier limit exceeded");
                pending.push_back({child.block, item.format, item.level - 1});
            }
        } else for (const auto& entry : index.entries) {
            require(entry.block != plan.root && !indexes.contains(entry.block), ErrorCode::Integrity, "Conflicting publication chunk kind");
            if (!chunks.insert(entry.block).second) continue;
            add(entry.block);
            const auto chunk = decode_object_chunk(entry.block, snapshot.block(entry.block));
            for (const auto& record : chunk.records) {
                require(std::chrono::steady_clock::now() < deadline, ErrorCode::Budget, "Publication inspection time budget exceeded");
                const auto size = snapshot_record_size(record);
                require(size <= limits.record_bytes - record_bytes, ErrorCode::Budget, "Contribution decoded record budget exceeded");
                record_bytes += size;
                const auto object_id = identify(record.object);
                require(expected.contains(object_id) && objects.insert(object_id).second,
                        ErrorCode::Integrity, "Publication chunk has unselected or duplicate records");
                require(record.object.visibility == Visibility::Public, ErrorCode::Permission, "Publication contains private data");
                const auto indexed = snapshot.get(object_id);
                require(indexed && indexed->object == record.object && indexed->assessment == record.assessment,
                        ErrorCode::Integrity, "Publication object locator differs");
                auto required = references(record.object);
                if (record.assessment) {
                    required.insert(required.end(), record.assessment->checked_dependencies.begin(),
                                        record.assessment->checked_dependencies.end());
                    if (record.assessment->certificate) required.push_back(*record.assessment->certificate);
                }
                for (const auto& dependency : required)
                    require(expected.contains(dependency), ErrorCode::Missing, "Publication dependency is unavailable");
                std::sort(required.begin(), required.end());
                required.erase(std::unique(required.begin(), required.end()), required.end());
                dependencies.emplace(object_id, std::move(required));
            }
        }
    }
    require(objects == expected, ErrorCode::Integrity, "Publication omits selected objects");
    std::vector<ObjectId> reachable;
    std::set<ObjectId> seen;
    for (const auto& id : roots) {
        require(seen.insert(id).second, ErrorCode::Schema, "Duplicate contribution root");
        reachable.push_back(id);
    }
    for (std::size_t i = 0; i < reachable.size(); ++i)
        for (const auto& id : dependencies.at(reachable[i]))
            if (seen.insert(id).second) reachable.push_back(id);
    require(seen == expected, ErrorCode::Integrity, "Publication includes objects outside the selected root closure");
    included.erase(plan.root);
    plan.blocks.assign(included.begin(), included.end()); plan.blocks.push_back(plan.root);
    return plan;
}

PublicationPlan inspect_publication(const PreparedContribution& contribution, PublicationLimits limits) {
    validate_limits(limits);
    GksSnapshot snapshot(contribution.directory, contribution.snapshot.root);
    return inspect_snapshot(snapshot, contribution.snapshot.root, contribution.roots, contribution.objects, limits);
}
PublicationPlan inspect_contribution(std::shared_ptr<BlockSource> source, const BlockId& root,
                                    std::span<const ObjectId> roots, std::span<const ObjectId> objects,
                                    PublicationLimits limits) {
    validate_limits(limits);
    GksSnapshot snapshot(std::move(source), root);
    return inspect_snapshot(snapshot, root, roots, objects, limits);
}

PublicationPermit::PublicationPermit(std::weak_ptr<std::atomic<bool>> owner, PreparedContribution contribution, PublicationPlan plan)
    : owner_(std::move(owner)), active_(std::make_shared<std::atomic<bool>>(true)),
      contribution_(std::move(contribution)), plan_(std::move(plan)) {}
PublicationPermit::PublicationPermit(std::weak_ptr<std::atomic<bool>> owner, Bytes signed_feed, PublicationPlan plan)
    : owner_(std::move(owner)), active_(std::make_shared<std::atomic<bool>>(true)),
      plan_(std::move(plan)), signed_feed_(std::move(signed_feed)) {}
PublicationPermit::PublicationPermit(PublicationPermit&& other) noexcept
    : owner_(std::move(other.owner_)), active_(std::move(other.active_)),
      contribution_(std::move(other.contribution_)), plan_(std::move(other.plan_)),
      signed_feed_(std::move(other.signed_feed_)),
      used_(std::exchange(other.used_, true)) {}
PublicationPermit& PublicationPermit::operator=(PublicationPermit&& other) noexcept {
    if (this != &other) {
        revoke(); owner_ = std::move(other.owner_); active_ = std::move(other.active_);
        contribution_ = std::move(other.contribution_); plan_ = std::move(other.plan_);
        signed_feed_ = std::move(other.signed_feed_);
        used_ = std::exchange(other.used_, true);
    }
    return *this;
}
void PublicationPermit::revoke() noexcept { if (active_) active_->store(false); }

ContributionPublisher::ContributionPublisher(std::string origin, PublicationLimits limits) : limits_(limits) {
    validate_limits(limits_);
    static std::once_flag initialized;
    std::call_once(initialized, [] {
        require(curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK, ErrorCode::Unavailable, "Cannot initialize publication transport");
    });
    std::unique_ptr<CURLU, UrlDelete> url(curl_url());
    require(bool(url) && origin.size() <= 256 && origin.find('\0') == std::string::npos
        && curl_url_set(url.get(), CURLUPART_URL, origin.c_str(), 0) == CURLUE_OK,
            ErrorCode::Schema, "Invalid Kubo origin");
    require(origin.find_first_of("?#@") == std::string::npos
        && part(url.get(), CURLUPART_SCHEME) == "http" && part(url.get(), CURLUPART_HOST) == "127.0.0.1"
        && part(url.get(), CURLUPART_PATH) == "/" && part(url.get(), CURLUPART_QUERY, true).empty()
        && part(url.get(), CURLUPART_FRAGMENT, true).empty() && part(url.get(), CURLUPART_USER, true).empty()
        && part(url.get(), CURLUPART_PASSWORD, true).empty() && part(url.get(), CURLUPART_OPTIONS, true).empty(),
        ErrorCode::Permission, "Publication requires an explicit bare loopback Kubo origin");
    const auto port = part(url.get(), CURLUPART_PORT, true);
    endpoint_ = "http://127.0.0.1" + (port.empty() ? "" : ":" + port);
    endpoint_ += "/api/v0/block/put?cid-codec=raw&mhtype=sha2-256&mhlen=32&pin=true&allow-big-block=false";
}
ContributionPublisher::~ContributionPublisher() { revoke_all(); }
void ContributionPublisher::revoke_all() noexcept { active_->store(false); }
PublicationPermit ContributionPublisher::approve(const PreparedContribution& contribution) {
    require(std::this_thread::get_id() == thread_ && active_->load(), ErrorCode::Permission, "Publication owner is unavailable");
    auto copy = contribution;
    copy.directory = std::filesystem::absolute(copy.directory);
    auto plan = inspect_publication(copy, limits_);
    return PublicationPermit(active_, std::move(copy), std::move(plan));
}
PublicationPermit ContributionPublisher::approve_feed(const VerifiedPublisherFeed& feed) {
    require(std::this_thread::get_id() == thread_ && active_->load(), ErrorCode::Permission, "Publication owner is unavailable");
    require(feed.wire().size() <= limits_.payload_bytes, ErrorCode::Budget, "Signed feed exceeds publication byte budget");
    const auto root = identify_block(feed.wire());
    return PublicationPermit(active_, Bytes(feed.wire().begin(), feed.wire().end()), {root, {root}, feed.wire().size()});
}
PublicationReceipt ContributionPublisher::publish(PublicationPermit& permit) {
    require(std::this_thread::get_id() == thread_ && permit.owner_.lock() == active_ && active_->load()
        && permit.active_ && permit.active_->load() && !permit.used_, ErrorCode::Permission, "Publication permit is unavailable");
    permit.used_ = true;
    last_ = {}; last_.root = permit.plan_.root;
    const auto deadline = std::chrono::steady_clock::now() + limits_.operation_time;
    std::unique_ptr<GksSnapshot> snapshot;
    if (permit.signed_feed_.empty()) {
        require(inspect_publication(permit.contribution_, limits_) == permit.plan_, ErrorCode::Integrity, "Approved publication changed");
        snapshot = std::make_unique<GksSnapshot>(permit.contribution_.directory, permit.plan_.root);
    } else {
        require(identify_block(permit.signed_feed_) == permit.plan_.root, ErrorCode::Integrity, "Approved signed feed changed");
    }
    for (const auto& id : permit.plan_.blocks) {
        require(active_->load() && permit.active_->load(), ErrorCode::Permission, "Publication was revoked");
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        require(remaining.count() > 0, ErrorCode::Budget, "Publication time budget exceeded");
        const auto bytes = snapshot ? snapshot->block(id) : permit.signed_feed_;
        std::unique_ptr<CURL, CurlDelete> curl(curl_easy_init());
        require(bool(curl), ErrorCode::Unavailable, "Cannot allocate publication transport");
        std::unique_ptr<curl_mime, MimeDelete> mime(curl_mime_init(curl.get()));
        require(bool(mime), ErrorCode::Storage, "Cannot allocate publication body");
        auto* file = curl_mime_addpart(mime.get());
        require(file && curl_mime_name(file, "file") == CURLE_OK && curl_mime_filename(file, "block.gksb") == CURLE_OK
            && curl_mime_type(file, "application/octet-stream") == CURLE_OK
            && curl_mime_data(file, reinterpret_cast<const char*>(bytes.data()), bytes.size()) == CURLE_OK,
            ErrorCode::Storage, "Cannot encode publication body");
        std::unique_ptr<curl_slist, HeaderDelete> headers(curl_slist_append(nullptr, "Expect:"));
        require(bool(headers), ErrorCode::Storage, "Cannot allocate publication headers");
        Transfer transfer{active_, permit.active_, {}, 0, false};
        const auto option = [&](CURLoption key, auto value) {
            require(curl_easy_setopt(curl.get(), key, value) == CURLE_OK,
                    ErrorCode::Unavailable, "Cannot configure publication transport");
        };
        option(CURLOPT_URL, endpoint_.c_str());
        option(CURLOPT_PROTOCOLS_STR, "http");
        option(CURLOPT_FOLLOWLOCATION, 0L);
        option(CURLOPT_PROXY, "");
        option(CURLOPT_NOPROXY, "*");
        option(CURLOPT_NETRC, static_cast<long>(CURL_NETRC_IGNORED));
        option(CURLOPT_NOSIGNAL, 1L);
        option(CURLOPT_CONNECTTIMEOUT_MS, std::min<long>(3000, remaining.count()));
        option(CURLOPT_TIMEOUT_MS, static_cast<long>(remaining.count()));
        option(CURLOPT_HTTPHEADER, headers.get());
        option(CURLOPT_MIMEPOST, mime.get());
        option(CURLOPT_WRITEFUNCTION, receive);
        option(CURLOPT_WRITEDATA, &transfer);
        option(CURLOPT_HEADERFUNCTION, header);
        option(CURLOPT_HEADERDATA, &transfer);
        option(CURLOPT_NOPROGRESS, 0L);
        option(CURLOPT_XFERINFOFUNCTION, progress);
        option(CURLOPT_XFERINFODATA, &transfer);
        ++last_.attempted_requests;
        const auto result = curl_easy_perform(curl.get());
        curl_off_t sent = 0;
        require(curl_easy_getinfo(curl.get(), CURLINFO_SIZE_UPLOAD_T, &sent) == CURLE_OK,
                ErrorCode::Unavailable, "Cannot read publication upload accounting");
        if (sent > 0) last_.uploaded_http_body_bytes += static_cast<std::uint64_t>(sent);
        require(active_->load() && permit.active_->load(), ErrorCode::Permission, "Publication was revoked");
        require(!transfer.exceeded, ErrorCode::Budget, "Publication acknowledgement exceeds budget");
        require(std::chrono::steady_clock::now() < deadline, ErrorCode::Budget, "Publication time budget exceeded");
        require(result == CURLE_OK, ErrorCode::Unavailable, "Publication transport failed");
        long status = 0;
        require(curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status) == CURLE_OK,
                ErrorCode::Unavailable, "Cannot read publication status");
        require(status == 200, ErrorCode::Unavailable, "Kubo block publication was not acknowledged");
        acknowledge(transfer.response, id, bytes.size());
        last_.confirmed.push_back(block_cid(id)); last_.confirmed_payload_bytes += bytes.size();
        if (id == permit.plan_.root) last_.root_stored = true;
    }
    return last_;
}

} // namespace axiom::knowledge
