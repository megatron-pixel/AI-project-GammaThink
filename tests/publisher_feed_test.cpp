#include "publisher_feed.hpp"
#include "gks_snapshot.hpp"

#include <algorithm>
#include <iostream>

namespace {
using namespace axiom::knowledge;
std::size_t assertions = 0;
void check(bool value) {
    ++assertions;
    if (!value) throw std::runtime_error("Publisher feed assertion " + std::to_string(assertions));
}
template<class Function> void rejects(ErrorCode expected, Function function) {
    try { function(); }
    catch (const Error& error) { check(error.code == expected); return; }
    throw std::runtime_error("Expected publisher feed rejection");
}
Bytes altered(const Bytes& wire, std::string_view from, std::string_view to) {
    std::string value(wire.begin(), wire.end());
    const auto found = value.find(from);
    if (found == std::string::npos) throw std::runtime_error("Missing mutation field");
    value.replace(found, from.size(), to);
    return Bytes(value.begin(), value.end());
}
class MetadataSource final : public BlockSource {
public:
    explicit MetadataSource(Bytes value) : wire(std::move(value)), expected{digest(wire)} {}
    Bytes read(const BlockId& id, std::size_t maximum) override {
        ++reads; last_maximum = maximum;
        check(id == expected);
        if (wire.size() > maximum) throw Error(ErrorCode::Budget, "Synthetic metadata source budget");
        return wire;
    }
    Bytes wire;
    BlockId expected;
    std::size_t reads = 0, last_maximum = 0;
};
}
int main() {
    try {
        // Public RFC 8032 test-vector seed, not an account/provider credential.
        const auto seed = parse_publisher_key("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60");
        const auto key = publisher_key_from_seed(seed);
        check(publisher_key_text(key) == "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a");
        check(parse_publisher_key(publisher_key_text(key)) == key);
        const SnapshotOffer offer{"synthetic", "1", {digest(Bytes{1})}, {{digest(Bytes{2})}}, {}};
        PublisherFeed feed{"stable", 1, {}, {offer}};
        const auto wire = sign_publisher_feed(feed, seed);
        check(wire == sign_publisher_feed(feed, seed));
        const auto verified = verify_publisher_feed(wire, key);
        check(verified.publisher() == key && verified.content() == feed);
        check(verified.id().digest == digest(wire));
        check(std::equal(wire.begin(), wire.end(), verified.wire().begin(), verified.wire().end()));
        check(hex(verified.id()).size() == 64);
        MetadataSource source(wire);
        const auto fetched = fetch_publisher_feed(source, block_cid(source.expected), key);
        check(fetched.id() == verified.id() && source.reads == 1 && source.last_maximum == 1024 * 1024);
        source.wire[0] ^= 1;
        rejects(ErrorCode::Integrity, [&] { fetch_publisher_feed(source, block_cid(source.expected), key); });
        source.wire = wire;
        PublisherFeedLimits fetch_limit; fetch_limit.wire_bytes = wire.size() - 1;
        rejects(ErrorCode::Budget, [&] { fetch_publisher_feed(source, block_cid(source.expected), key, fetch_limit); });
        auto wrong_key = key; wrong_key[0] ^= 1;
        rejects(ErrorCode::Permission, [&] { verify_publisher_feed(wire, wrong_key); });
        rejects(ErrorCode::Integrity, [&] {
            verify_publisher_feed(altered(wire, "\"stable\"", "\"stale\""), key);
        });
        auto signature = wire;
        signature[signature.size() - 3] = signature[signature.size() - 3] == '0' ? '1' : '0';
        rejects(ErrorCode::Integrity, [&] { verify_publisher_feed(signature, key); });
        rejects(ErrorCode::Schema, [&] { verify_publisher_feed(altered(wire, "{\"payload\":", "{ \"payload\":"), key); });
        rejects(ErrorCode::Schema, [&] {
            verify_publisher_feed(altered(wire, "\"channel\":\"stable\"", "\"channel\":\"unused\",\"channel\":\"stable\""), key);
        });
        rejects(ErrorCode::Schema, [&] {
            verify_publisher_feed(altered(wire, "\"channel\":\"stable\"", "\"channel\":\"stable\",\"untrusted\":true"), key);
        });
        rejects(ErrorCode::Schema, [&] { verify_publisher_feed(altered(wire, "\"sequence\":1", "\"sequence\":1.0"), key); });
        rejects(ErrorCode::Schema, [&] { verify_publisher_feed(altered(wire, "\"sequence\":1", "\"sequence\":-1"), key); });
        rejects(ErrorCode::Schema, [&] { verify_publisher_feed(altered(wire, "\"sequence\":1", "\"sequence\":9223372036854775808"), key); });
        auto nul = altered(wire, "\"stable\"", "\"sta\\u0000ble\"");
        rejects(ErrorCode::Schema, [&] { verify_publisher_feed(nul, key); });
        auto utf8 = wire;
        const auto position = std::search(utf8.begin(), utf8.end(), feed.channel.begin(), feed.channel.end());
        *position = 0xff;
        rejects(ErrorCode::Schema, [&] { verify_publisher_feed(utf8, key); });
        rejects(ErrorCode::Budget, [&] { verify_publisher_feed({}, key); });
        PublisherFeedLimits tiny; tiny.wire_bytes = wire.size() - 1;
        rejects(ErrorCode::Budget, [&] { verify_publisher_feed(wire, key, tiny); });
        rejects(ErrorCode::Budget, [&] { sign_publisher_feed(feed, seed, tiny); });
        rejects(ErrorCode::Schema, [&] { publisher_key_from_seed(std::span(seed).first(31)); });
        rejects(ErrorCode::Schema, [&] { parse_publisher_key(std::string(64, 'F')); });
        auto next = feed;
        next.sequence = 2; next.previous = verified.id();
        next.offers[0].parents = {offer.snapshot};
        next.offers[0].snapshot = {digest(Bytes{3})};
        // Signature checking intentionally does not grant chain acceptance.
        const auto successor = verify_publisher_feed(sign_publisher_feed(next, seed), key);
        check(successor.content() == next && successor.id() != verified.id());
        auto fork = next;
        fork.offers.push_back(offer); // Same name/revision, distinct root: preserve both.
        check(verify_publisher_feed(sign_publisher_feed(fork, seed), key).content().offers.size() == 2);
        fork.offers.push_back(offer);
        rejects(ErrorCode::Schema, [&] { sign_publisher_feed(fork, seed); });
        fork = next; fork.previous.reset();
        rejects(ErrorCode::Schema, [&] { sign_publisher_feed(fork, seed); });
        fork = feed; fork.previous = verified.id();
        rejects(ErrorCode::Schema, [&] { sign_publisher_feed(fork, seed); });
        fork = feed; fork.sequence = 0;
        rejects(ErrorCode::Schema, [&] { sign_publisher_feed(fork, seed); });
        fork = feed; fork.offers[0].parents = {offer.snapshot};
        rejects(ErrorCode::Schema, [&] { sign_publisher_feed(fork, seed); });
        fork = feed; fork.offers[0].roots.push_back(offer.roots.front());
        rejects(ErrorCode::Schema, [&] { sign_publisher_feed(fork, seed); });
        fork = feed; fork.offers[0].roots.clear();
        rejects(ErrorCode::Budget, [&] { sign_publisher_feed(fork, seed); });
        fork = feed; fork.channel = std::string(129, 'a');
        rejects(ErrorCode::Budget, [&] { sign_publisher_feed(fork, seed); });
        PublisherFeedLimits limits; limits.offers = 1;
        fork = next; fork.offers.push_back(offer);
        rejects(ErrorCode::Budget, [&] { sign_publisher_feed(fork, seed, limits); });
        limits = {}; limits.root_references = 1;
        rejects(ErrorCode::Budget, [&] { sign_publisher_feed(fork, seed, limits); });
        limits = {}; limits.parent_references = 0;
        rejects(ErrorCode::Budget, [&] { sign_publisher_feed(next, seed, limits); });
        fork = feed; fork.offers.clear();
        check(verify_publisher_feed(sign_publisher_feed(fork, seed), key).content().offers.empty());
        std::cout << "Publisher feeds passed: " << assertions << " deterministic assertions\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
