#include "publisher_feed.hpp"
#include "gks_snapshot.hpp"

#include <json-c/json.h>
#include <openssl/evp.h>

#include <algorithm>
#include <memory>
#include <set>

namespace axiom::knowledge {
namespace {
void require(bool value, ErrorCode code, const char* message) {
    if (!value) throw Error(code, message);
}
struct JsonDelete { void operator()(json_object* p) const { json_object_put(p); } };
struct ParserDelete { void operator()(json_tokener* p) const { json_tokener_free(p); } };
struct KeyDelete { void operator()(EVP_PKEY* p) const { EVP_PKEY_free(p); } };
struct ContextDelete { void operator()(EVP_MD_CTX* p) const { EVP_MD_CTX_free(p); } };
using Json = std::unique_ptr<json_object, JsonDelete>;
using Key = std::unique_ptr<EVP_PKEY, KeyDelete>;
void limits_valid(PublisherFeedLimits limits) {
    require(limits.wire_bytes > 0 && limits.wire_bytes <= 1024 * 1024
        && limits.offers > 0 && limits.offers <= 64
        && limits.root_references > 0 && limits.root_references <= maximum_items
        && limits.parent_references <= maximum_items, ErrorCode::Budget, "Invalid publisher feed limits");
}
json_object* allocated(json_object* value) {
    require(value != nullptr, ErrorCode::Storage, "Cannot allocate publisher feed JSON");
    return value;
}
void add(json_object* parent, const char* key, json_object* value) {
    require(json_object_object_add(parent, key, value) == 0, ErrorCode::Storage, "Cannot encode publisher feed field");
}
void append(json_object* parent, json_object* value) {
    require(json_object_array_add(parent, value) == 0, ErrorCode::Storage, "Cannot encode publisher feed item");
}
json_object* text(std::string_view value) {
    return allocated(json_object_new_string_len(value.data(), static_cast<int>(value.size())));
}
std::string wire_text(json_object* value) {
    const auto* result = json_object_to_json_string_ext(value, JSON_C_TO_STRING_PLAIN);
    require(result != nullptr, ErrorCode::Storage, "Cannot serialize publisher feed");
    return result;
}
void label(std::string_view value) {
    validate_text(value, 128);
    require(!value.empty(), ErrorCode::Schema, "Empty publisher feed label");
}
Json payload(const PublisherFeed& feed, const PublisherKey& key, PublisherFeedLimits limits) {
    limits_valid(limits);
    label(feed.channel);
    require(feed.sequence > 0 && feed.sequence <= static_cast<std::uint64_t>(INT64_MAX)
        && (feed.sequence == 1) == !feed.previous.has_value(), ErrorCode::Schema, "Invalid publisher feed sequence");
    require(feed.offers.size() <= limits.offers, ErrorCode::Budget, "Publisher feed offer limit exceeded");
    std::size_t roots = 0, parents = 0;
    std::set<BlockId> snapshots;
    Json result(allocated(json_object_new_object()));
    add(result.get(), "format", text("gammathink.publisher-feed.v1"));
    add(result.get(), "publisher", text(publisher_key_text(key)));
    add(result.get(), "channel", text(feed.channel));
    add(result.get(), "sequence", allocated(json_object_new_uint64(feed.sequence)));
    add(result.get(), "previous", feed.previous ? text(hex(*feed.previous)) : nullptr);
    auto* offers = allocated(json_object_new_array());
    add(result.get(), "offers", offers);
    for (const auto& offer : feed.offers) {
        label(offer.name); label(offer.revision);
        require(snapshots.insert(offer.snapshot).second, ErrorCode::Schema, "Duplicate offered snapshot");
        require(!offer.roots.empty() && offer.roots.size() <= limits.root_references - roots
            && offer.parents.size() <= limits.parent_references - parents,
            ErrorCode::Budget, "Publisher feed reference budget exceeded");
        roots += offer.roots.size(); parents += offer.parents.size();
        require(std::set<ObjectId>(offer.roots.begin(), offer.roots.end()).size() == offer.roots.size()
            && std::set<BlockId>(offer.parents.begin(), offer.parents.end()).size() == offer.parents.size(),
            ErrorCode::Schema, "Duplicate publisher feed reference");
        require(std::find(offer.parents.begin(), offer.parents.end(), offer.snapshot) == offer.parents.end(),
            ErrorCode::Schema, "Snapshot cannot declare itself a prior version");
        auto* item = allocated(json_object_new_object());
        append(offers, item);
        add(item, "name", text(offer.name));
        add(item, "revision", text(offer.revision));
        add(item, "snapshot", text(cid_text(block_cid(offer.snapshot))));
        auto* root_array = allocated(json_object_new_array());
        add(item, "roots", root_array);
        for (const auto& id : offer.roots) append(root_array, text(hex(id)));
        auto* parent_array = allocated(json_object_new_array());
        add(item, "parents", parent_array);
        for (const auto& id : offer.parents) append(parent_array, text(cid_text(block_cid(id))));
    }
    return result;
}
std::string hex_bytes(std::span<const std::uint8_t> bytes) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result; result.reserve(bytes.size() * 2);
    for (auto byte : bytes) { result.push_back(digits[byte >> 4]); result.push_back(digits[byte & 15]); }
    return result;
}
Bytes parse_hex(std::string_view value, std::size_t bytes) {
    require(value.size() == 2 * bytes, ErrorCode::Schema, "Invalid publisher hexadecimal extent");
    Bytes result; result.reserve(bytes);
    const auto digit = [](char ch) -> std::uint8_t {
        if (ch >= '0' && ch <= '9') return static_cast<std::uint8_t>(ch - '0');
        if (ch >= 'a' && ch <= 'f') return static_cast<std::uint8_t>(ch - 'a' + 10);
        throw Error(ErrorCode::Schema, "Expected lowercase publisher hexadecimal");
    };
    for (std::size_t i = 0; i < value.size(); i += 2)
        result.push_back(static_cast<std::uint8_t>((digit(value[i]) << 4) | digit(value[i + 1])));
    return result;
}
Key private_key(std::span<const std::uint8_t> seed) {
    require(seed.size() == 32, ErrorCode::Schema, "Ed25519 private seed must contain 32 bytes");
    Key key(EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, seed.data(), seed.size()));
    require(bool(key), ErrorCode::Storage, "Cannot initialize publisher signing key");
    return key;
}
PublisherKey public_key(EVP_PKEY* key) {
    PublisherKey result{};
    auto size = result.size();
    require(EVP_PKEY_get_raw_public_key(key, result.data(), &size) == 1 && size == result.size(),
        ErrorCode::Storage, "Cannot derive publisher verification key");
    return result;
}
Json envelope(Json value, std::span<const std::uint8_t> signature) {
    Json result(allocated(json_object_new_object()));
    add(result.get(), "payload", value.release());
    add(result.get(), "signature", text(hex_bytes(signature)));
    return result;
}
json_object* field(json_object* value, const char* key, json_type type) {
    json_object* result = nullptr;
    require(json_object_is_type(value, json_type_object) && json_object_object_get_ex(value, key, &result)
        && json_object_is_type(result, type), ErrorCode::Schema, "Invalid publisher feed field");
    return result;
}
std::string_view string(json_object* value) {
    require(json_object_is_type(value, json_type_string), ErrorCode::Schema, "Expected publisher feed string");
    return {json_object_get_string(value), static_cast<std::size_t>(json_object_get_string_len(value))};
}
}

std::string hex(const PublisherFeedId& id) { return hex_bytes(id.digest); }
std::string publisher_key_text(const PublisherKey& key) { return hex_bytes(key); }
PublisherKey parse_publisher_key(std::string_view value) {
    const auto bytes = parse_hex(value, 32);
    PublisherKey result; std::copy(bytes.begin(), bytes.end(), result.begin()); return result;
}
PublisherKey publisher_key_from_seed(std::span<const std::uint8_t> seed) {
    return public_key(private_key(seed).get());
}
Bytes sign_publisher_feed(const PublisherFeed& feed, std::span<const std::uint8_t> seed, PublisherFeedLimits limits) {
    auto key = private_key(seed);
    auto value = payload(feed, public_key(key.get()), limits);
    const auto message = wire_text(value.get());
    require(message.size() <= limits.wire_bytes, ErrorCode::Budget, "Publisher feed exceeds byte budget");
    std::array<std::uint8_t, 64> signature{};
    auto size = signature.size();
    std::unique_ptr<EVP_MD_CTX, ContextDelete> context(EVP_MD_CTX_new());
    require(context && EVP_DigestSignInit(context.get(), nullptr, nullptr, nullptr, key.get()) == 1
        && EVP_DigestSign(context.get(), signature.data(), &size,
            reinterpret_cast<const unsigned char*>(message.data()), message.size()) == 1 && size == signature.size(),
        ErrorCode::Storage, "Cannot sign publisher feed");
    const auto encoded = wire_text(envelope(std::move(value), signature).get());
    require(encoded.size() <= limits.wire_bytes, ErrorCode::Budget, "Signed publisher feed exceeds byte budget");
    return Bytes(encoded.begin(), encoded.end());
}
VerifiedPublisherFeed verify_publisher_feed(std::span<const std::uint8_t> wire,
                                            const PublisherKey& pinned, PublisherFeedLimits limits) {
    limits_valid(limits);
    require(!wire.empty() && wire.size() <= limits.wire_bytes, ErrorCode::Budget, "Publisher feed exceeds byte budget");
    std::unique_ptr<json_tokener, ParserDelete> parser(json_tokener_new_ex(12));
    require(bool(parser), ErrorCode::Storage, "Cannot allocate publisher feed parser");
    json_tokener_set_flags(parser.get(), JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
    Json input(json_tokener_parse_ex(parser.get(), reinterpret_cast<const char*>(wire.data()), static_cast<int>(wire.size())));
    require(input && json_tokener_get_error(parser.get()) == json_tokener_success
        && json_tokener_get_parse_end(parser.get()) == wire.size(), ErrorCode::Schema, "Invalid publisher feed JSON");
    auto* value = field(input.get(), "payload", json_type_object);
    require(string(field(value, "format", json_type_string)) == "gammathink.publisher-feed.v1",
        ErrorCode::Schema, "Unsupported publisher feed format");
    const auto publisher = parse_publisher_key(string(field(value, "publisher", json_type_string)));
    require(publisher == pinned, ErrorCode::Permission, "Publisher differs from host-pinned key");
    PublisherFeed feed;
    feed.channel = string(field(value, "channel", json_type_string));
    auto* sequence = field(value, "sequence", json_type_int);
    require(json_object_get_int64(sequence) > 0, ErrorCode::Schema, "Invalid publisher feed sequence");
    feed.sequence = json_object_get_uint64(sequence);
    json_object* previous = nullptr;
    require(json_object_object_get_ex(value, "previous", &previous), ErrorCode::Schema, "Missing publisher predecessor");
    if (previous) feed.previous = PublisherFeedId{parse_publisher_key(string(previous))};
    auto* offers = field(value, "offers", json_type_array);
    require(json_object_array_length(offers) <= limits.offers, ErrorCode::Budget, "Publisher feed offer limit exceeded");
    std::size_t root_count = 0, parent_count = 0;
    for (std::size_t i = 0; i < json_object_array_length(offers); ++i) {
        auto* item = json_object_array_get_idx(offers, i);
        SnapshotOffer offer;
        offer.name = string(field(item, "name", json_type_string));
        offer.revision = string(field(item, "revision", json_type_string));
        offer.snapshot = block_id(parse_block_cid(string(field(item, "snapshot", json_type_string))));
        auto* roots = field(item, "roots", json_type_array);
        auto* parents = field(item, "parents", json_type_array);
        require(json_object_array_length(roots) <= limits.root_references - root_count
            && json_object_array_length(parents) <= limits.parent_references - parent_count,
            ErrorCode::Budget, "Publisher feed reference limit exceeded");
        root_count += json_object_array_length(roots); parent_count += json_object_array_length(parents);
        for (std::size_t j = 0; j < json_object_array_length(roots); ++j)
            offer.roots.push_back(parse_id(string(json_object_array_get_idx(roots, j))));
        for (std::size_t j = 0; j < json_object_array_length(parents); ++j)
            offer.parents.push_back(block_id(parse_block_cid(string(json_object_array_get_idx(parents, j)))));
        feed.offers.push_back(std::move(offer));
    }
    const auto signature = parse_hex(string(field(input.get(), "signature", json_type_string)), 64);
    auto canonical = payload(feed, publisher, limits);
    const auto message = wire_text(canonical.get());
    const auto encoded = wire_text(envelope(std::move(canonical), signature).get());
    require(encoded.size() == wire.size() && std::equal(encoded.begin(), encoded.end(), wire.begin(),
        [](char a, std::uint8_t b) { return static_cast<std::uint8_t>(a) == b; }),
        ErrorCode::Schema, "Publisher feed is not canonical");
    Key key(EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, pinned.data(), pinned.size()));
    std::unique_ptr<EVP_MD_CTX, ContextDelete> context(EVP_MD_CTX_new());
    require(key && context && EVP_DigestVerifyInit(context.get(), nullptr, nullptr, nullptr, key.get()) == 1,
        ErrorCode::Storage, "Cannot initialize publisher verification");
    require(EVP_DigestVerify(context.get(), signature.data(), signature.size(),
        reinterpret_cast<const unsigned char*>(message.data()), message.size()) == 1,
        ErrorCode::Integrity, "Publisher feed signature differs");
    return VerifiedPublisherFeed(publisher, std::move(feed), {digest(wire)}, Bytes(wire.begin(), wire.end()));
}
VerifiedPublisherFeed fetch_publisher_feed(BlockSource& source, const BlockCid& cid,
                                           const PublisherKey& pinned, PublisherFeedLimits limits) {
    limits_valid(limits);
    const auto wire = source.read(block_id(cid), limits.wire_bytes);
    require(wire.size() <= limits.wire_bytes, ErrorCode::Budget, "Publisher feed source exceeded byte budget");
    verify_block_cid(cid, wire);
    return verify_publisher_feed(wire, pinned, limits);
}

} // namespace axiom::knowledge
