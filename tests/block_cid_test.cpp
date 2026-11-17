#include "block_cid.hpp"

#include <iostream>
#include <type_traits>

namespace {
using namespace axiom::knowledge;
static_assert(!std::is_convertible_v<ObjectId, BlockCid>);
static_assert(!std::is_convertible_v<BlockId, BlockCid>);
static_assert(!std::is_convertible_v<BlockCid, BlockId>);
std::size_t assertions = 0;
void check(bool condition) {
    ++assertions;
    if (!condition) throw std::runtime_error("Block CID assertion " + std::to_string(assertions));
}
template<class Function> void rejects(ErrorCode expected, Function function) {
    try { function(); }
    catch (const Error& error) { check(error.code == expected); return; }
    throw std::runtime_error("Expected block CID rejection");
}
}
int main() {
    try {
        // "hello" is also the literal raw CIDv1 vector in specs.ipfs.tech/cid.
        // Empty/range vectors were independently derived with Python hashlib
        // and base64.b32encode, not this encoder.
        const std::array<std::string_view, 3> golden{
            "bafkreihdwdcefgh4dqkjv67uzcmw7ojee6xedzdetojuzjevtenxquvyku",
            "bafkreibm6jg3ux5qumhcn2b3flc3tyu6dmlb4xa7u5bf44yegnrjhc4yeq",
            "bafkreicav7zotuwysixepl6umshgsz2jofmhqx55dwuhbzyrajtl7fciqa"};
        std::vector<Bytes> payloads{{}, {'h','e','l','l','o'}, {}};
        for (unsigned i = 0; i < 256; ++i) payloads.back().push_back(static_cast<std::uint8_t>(i));
        for (std::size_t i = 0; i < payloads.size(); ++i) {
            const auto cid = block_cid(identify_block(payloads[i]));
            check(cid_text(cid) == golden[i]);
            check(parse_block_cid(golden[i]) == cid);
            check(decode_block_cid(encode_block_cid(cid)) == cid);
            check(block_id(cid) == identify_block(payloads[i]));
            verify_block_cid(cid, payloads[i]);
        }
        const auto hello = parse_block_cid(golden[1]);
        check(hex(block_id(hello)) == "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824");
        const auto binary = encode_block_cid(hello);
        check(binary.size() == 36 && binary[0] == 1 && binary[1] == 0x55 && binary[2] == 0x12 && binary[3] == 0x20);
        for (unsigned seed = 0; seed < 256; ++seed) {
            BlockId id;
            for (std::size_t i = 0; i < id.digest.size(); ++i) id.digest[i] = static_cast<std::uint8_t>(seed + i);
            const auto cid = block_cid(id);
            const auto text = cid_text(cid);
            check(text.size() == 59 && parse_block_cid(text) == cid && block_id(cid) == id);
            check(encode_block_cid(parse_block_cid(text)) == encode_block_cid(cid));
        }
        for (std::size_t size = 0; size < binary.size(); ++size) {
            rejects(ErrorCode::Schema, [&] { decode_block_cid(std::span(binary).first(size)); });
        }
        auto changed = binary; changed.push_back(0);
        rejects(ErrorCode::Schema, [&] { decode_block_cid(changed); });
        for (unsigned field = 0; field < 4; ++field) {
            changed = binary; changed[field] ^= 1;
            rejects(ErrorCode::Schema, [&] { decode_block_cid(changed); });
            changed = binary; changed[field] |= 0x80U; changed.insert(changed.begin() + field + 1, 0);
            rejects(ErrorCode::Schema, [&] { decode_block_cid(changed); });
        }
        changed.assign(binary.begin() + 2, binary.end()); // CIDv0 dag-pb, not raw.
        rejects(ErrorCode::Schema, [&] { decode_block_cid(changed); });
        changed = binary; changed[1] = 0x70; // Valid codec, unsupported GKS profile.
        rejects(ErrorCode::Schema, [&] { decode_block_cid(changed); });
        changed = binary; changed[1] = 0x71;
        rejects(ErrorCode::Schema, [&] { decode_block_cid(changed); });
        changed = binary; changed[0] = 2;
        rejects(ErrorCode::Schema, [&] { decode_block_cid(changed); });
        const auto canonical = std::string(golden[1]);
        for (std::size_t size = 0; size < canonical.size(); ++size) {
            rejects(ErrorCode::Schema, [&] { parse_block_cid(std::string_view(canonical).substr(0, size)); });
        }
        for (std::size_t position = 0; position < canonical.size(); ++position) {
            auto bad = canonical; bad[position] = '/';
            rejects(ErrorCode::Schema, [&] { parse_block_cid(bad); });
        }
        for (const char suffix : {'r', 's', 't'}) {
            auto bad = canonical; bad.back() = suffix; // Same digest bits, nonzero padding.
            rejects(ErrorCode::Schema, [&] { parse_block_cid(bad); });
        }
        auto bad = canonical; bad[5] = 'A';
        rejects(ErrorCode::Schema, [&] { parse_block_cid(bad); });
        bad = canonical; bad[5] = 0;
        rejects(ErrorCode::Schema, [&] { parse_block_cid(bad); });
        rejects(ErrorCode::Schema, [&] { parse_block_cid(canonical + "="); });
        rejects(ErrorCode::Schema, [&] { parse_block_cid("ipfs://" + canonical); });
        rejects(ErrorCode::Schema, [&] { parse_block_cid(hex(block_id(hello))); });
        rejects(ErrorCode::Integrity, [&] { verify_block_cid(hello, payloads.front()); });
        Object object;
        object.label = "stored object"; object.format = "application/octet-stream"; object.payload = payloads.back();
        const auto object_wire = encode_object(object);
        const auto block_wire = encode_object_chunk({{{object, {}}}});
        const auto stored_id = identify_block(block_wire);
        const auto stored_cid = block_cid(stored_id);
        verify_block_cid(stored_cid, block_wire);
        check(decode_object_chunk(block_id(parse_block_cid(cid_text(stored_cid))), block_wire).records.front().object == object);
        rejects(ErrorCode::Integrity, [&] { verify_block_cid(stored_cid, object_wire); });
        rejects(ErrorCode::Integrity, [&] {
            verify_block_cid(block_cid(BlockId{identify(object).digest}), block_wire);
        });
        for (std::size_t i = 0; i < block_wire.size(); ++i) {
            auto altered = block_wire; altered[i] ^= 1;
            rejects(ErrorCode::Integrity, [&] { verify_block_cid(stored_cid, altered); });
        }
        const Bytes oversized(maximum_cid_block_bytes + 1, 0);
        rejects(ErrorCode::Budget, [&] { verify_block_cid(stored_cid, oversized); });
        std::cout << "block CID tests passed: " << assertions << " deterministic assertions\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
