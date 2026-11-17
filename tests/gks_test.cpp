#include "gks.hpp"

#include <algorithm>
#include <iostream>
#include <zstd.h>

namespace {
using namespace axiom::knowledge;
std::size_t assertions = 0;
void check(bool condition) {
    ++assertions;
    if (!condition) throw std::runtime_error("GKS assertion " + std::to_string(assertions));
}
template<class Function> void rejects(ErrorCode expected, Function function) {
    try { function(); }
    catch (const Error& error) { check(error.code == expected); return; }
    throw std::runtime_error("Expected GKS rejection");
}
void number(Bytes& bytes, std::size_t offset, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes.at(offset + i) = static_cast<std::uint8_t>(value >> (8 * i));
}
Bytes body(const Bytes& wire) {
    Bytes result(gks_max_block_bytes);
    const auto size = ZSTD_decompress(result.data(), result.size(), wire.data() + 16, wire.size() - 16);
    check(!ZSTD_isError(size));
    result.resize(size);
    return result;
}
Bytes frame(const Bytes& payload, std::uint8_t kind) {
    Bytes wire(16 + ZSTD_compressBound(payload.size()));
    const auto size = ZSTD_compress(wire.data() + 16, wire.size() - 16, payload.data(), payload.size(), 3);
    check(!ZSTD_isError(size));
    wire.resize(16 + size);
    wire[0] = 'G'; wire[1] = 'K'; wire[2] = 'S'; wire[3] = 'B';
    wire[4] = 1; wire[5] = 0; wire[6] = kind; wire[7] = 1;
    number(wire, 8, payload.size()); number(wire, 12, size);
    return wire;
}
Object object(std::string label) {
    Object result;
    result.label = std::move(label);
    result.format = "text/plain; charset=utf-8";
    result.payload = {0, 1, 255};
    result.concepts = {"flow", "heat"};
    result.sources = {{"zim-source", "v1", "A/Heat", "compiler-v1", {}, 5, 12}};
    return result;
}
}

int main() {
    try {
        ObjectChunk objects{{{object("first"), {}}, {object("second"), {}}}};
        std::sort(objects.records.begin(), objects.records.end(), [](const auto& a, const auto& b) {
            return identify(a.object) < identify(b.object);
        });
        Assessment observed;
        observed.version = 7; observed.checker = "synthetic-checker"; observed.reason = "observed only";
        observed.state = KnowledgeState::Verified;
        observed.logical = LogicalValidity::Valid;
        observed.evidence = EvidenceAssessment::Challenged;
        observed.certificate = identify(objects.records.back().object);
        observed.checked_dependencies = {*observed.certificate, *observed.certificate};
        objects.records.front().assessment = observed;
        const auto wire = encode_object_chunk(objects);
        const auto id = identify_block(wire);
        check(parse_block_id(hex(id)) == id);
        check(decode_object_chunk(id, wire).records.size() == 2);
        const auto decoded = decode_object_chunk(id, wire);
        check(decoded.records.front().object == objects.records.front().object);
        check(decoded.records.back().object == objects.records.back().object);
        check(decoded.records.front().assessment == objects.records.front().assessment);
        check(!decoded.records.back().assessment);
        check(encode_object_chunk(decoded) == wire);
        auto reassessed = objects;
        ++reassessed.records.front().assessment->version;
        check(identify_block(encode_object_chunk(reassessed)) != id);
        check(identify(reassessed.records.front().object) == identify(objects.records.front().object));
        reassessed.records.front().assessment->checked_dependencies.assign(
            4 * maximum_items + 1, *observed.certificate);
        const auto large_observation = encode_object_chunk(reassessed);
        check(decode_object_chunk(identify_block(large_observation), large_observation)
                  .records.front().assessment == reassessed.records.front().assessment);
        reassessed.records.front().assessment->checked_dependencies.push_back(*observed.certificate);
        rejects(ErrorCode::Budget, [&] { encode_object_chunk(reassessed); });
        rejects(ErrorCode::Integrity, [&] { decode_object_chunk(BlockId{}, wire); });
        rejects(ErrorCode::Schema, [&] { decode_index_chunk(id, wire); });
        rejects(ErrorCode::Schema, [&] { encode_object_chunk({}); });
        auto changed = objects;
        std::reverse(changed.records.begin(), changed.records.end());
        rejects(ErrorCode::Schema, [&] { encode_object_chunk(changed); });
        changed = objects; changed.records.back() = changed.records.front();
        rejects(ErrorCode::Schema, [&] { encode_object_chunk(changed); });
        changed = objects; changed.records.front().assessment->version = 0;
        rejects(ErrorCode::Schema, [&] { encode_object_chunk(changed); });
        changed = objects; changed.records.front().assessment->checker.clear();
        rejects(ErrorCode::Schema, [&] { encode_object_chunk(changed); });
        GksLimits limits;
        limits.records = 1;
        rejects(ErrorCode::Budget, [&] { encode_object_chunk(objects, limits); });
        rejects(ErrorCode::Budget, [&] { decode_object_chunk(id, wire, limits); });
        const auto raw = body(wire);
        limits = {}; limits.block_bytes = raw.size();
        check(encode_object_chunk(objects, limits) == wire);
        check(decode_object_chunk(id, wire, limits).records.size() == 2);
        --limits.block_bytes;
        rejects(ErrorCode::Budget, [&] { encode_object_chunk(objects, limits); });
        rejects(ErrorCode::Budget, [&] { decode_object_chunk(id, wire, limits); });
        limits = {}; limits.block_bytes = gks_max_block_bytes + 1;
        rejects(ErrorCode::Budget, [&] { decode_object_chunk(id, wire, limits); });

        // Rehash malformed stored bytes so decoding, not just SHA256, is exercised.
        for (std::size_t i = 0; i < wire.size(); ++i) {
            const Bytes cut(wire.begin(), wire.begin() + i);
            rejects(i < 16 ? ErrorCode::Budget : ErrorCode::Schema,
                    [&] { decode_object_chunk(identify_block(cut), cut); });
        }
        for (const std::size_t offset : {0, 4, 6, 7, 16}) {
            auto bad = wire; bad[offset] ^= 127;
            rejects(ErrorCode::Schema, [&] { decode_object_chunk(identify_block(bad), bad); });
        }
        auto bad = wire; number(bad, 8, 0xffffffffU);
        rejects(ErrorCode::Budget, [&] { decode_object_chunk(identify_block(bad), bad); });
        bad = wire; number(bad, 8, raw.size() - 1);
        rejects(ErrorCode::Schema, [&] { decode_object_chunk(identify_block(bad), bad); });
        bad = wire; bad.insert(bad.end(), wire.begin() + 16, wire.end()); number(bad, 12, bad.size() - 16);
        rejects(ErrorCode::Schema, [&] { decode_object_chunk(identify_block(bad), bad); });
        auto malformed = raw; malformed[4] ^= 1; bad = frame(malformed, 1);
        rejects(ErrorCode::Integrity, [&] { decode_object_chunk(identify_block(bad), bad); });
        malformed = raw; number(malformed, 0, maximum_items + 1); bad = frame(malformed, 1);
        rejects(ErrorCode::Budget, [&] { decode_object_chunk(identify_block(bad), bad); });
        malformed = raw; number(malformed, 36, maximum_object_bytes + 1); bad = frame(malformed, 1);
        rejects(ErrorCode::Budget, [&] { decode_object_chunk(identify_block(bad), bad); });
        malformed = raw; malformed.push_back(0); bad = frame(malformed, 1);
        rejects(ErrorCode::Schema, [&] { decode_object_chunk(identify_block(bad), bad); });
        ObjectChunk single{{{object("single"), {}}}};
        malformed = body(encode_object_chunk(single)); malformed.back() = 2; bad = frame(malformed, 1);
        rejects(ErrorCode::Schema, [&] { decode_object_chunk(identify_block(bad), bad); });
        malformed = raw;
        malformed.at(40 + encode_object(objects.records.front().object).size() + 1 + 8) = 0;
        bad = frame(malformed, 1);
        rejects(ErrorCode::Schema, [&] { decode_object_chunk(identify_block(bad), bad); });

        const auto first = identify(objects.records.front().object), last = identify(objects.records.back().object);
        IndexChunk leaf{"object-id-v1", 0, {{{{}, first}, id, 0}, {{{}, last}, id, 1}}, {}};
        auto index_wire = encode_index_chunk(leaf);
        const auto leaf_id = identify_block(index_wire);
        check(decode_index_chunk(leaf_id, index_wire) == leaf);
        auto invalid_index = leaf; invalid_index.entries.back().position = leaf.entries.front().position;
        rejects(ErrorCode::Schema, [&] { encode_index_chunk(invalid_index); });
        invalid_index = leaf; invalid_index.entries.front().ordinal = maximum_items;
        rejects(ErrorCode::Schema, [&] { encode_index_chunk(invalid_index); });
        invalid_index = leaf; invalid_index.key_format.clear();
        rejects(ErrorCode::Schema, [&] { encode_index_chunk(invalid_index); });
        invalid_index = leaf;
        for (auto& entry : invalid_index.entries) entry.position.key = {1, 2};
        const auto keyed_wire = encode_index_chunk(invalid_index);
        limits = {}; limits.key_bytes = 1;
        rejects(ErrorCode::Budget, [&] { encode_index_chunk(invalid_index, limits); });
        rejects(ErrorCode::Budget, [&] { decode_index_chunk(identify_block(keyed_wire), keyed_wire, limits); });
        IndexChunk branch{"object-id-v1", 1, {}, {{leaf.entries.front().position, leaf.entries.back().position, leaf_id}}};
        index_wire = encode_index_chunk(branch);
        check(decode_index_chunk(identify_block(index_wire), index_wire) == branch);
        limits = {}; limits.index_depth = 0;
        rejects(ErrorCode::Budget, [&] { encode_index_chunk(branch, limits); });
        rejects(ErrorCode::Budget, [&] { decode_index_chunk(identify_block(index_wire), index_wire, limits); });
        branch.children.push_back(branch.children.front());
        rejects(ErrorCode::Schema, [&] { encode_index_chunk(branch); });
        IndexChunk left{"object-id-v1", 0, {leaf.entries.front()}, {}};
        IndexChunk right{"object-id-v1", 0, {leaf.entries.back()}, {}};
        branch.children = {{left.entries.front().position, left.entries.front().position,
                            identify_block(encode_index_chunk(left))},
                           {right.entries.front().position, right.entries.front().position,
                            identify_block(encode_index_chunk(right))}};
        index_wire = encode_index_chunk(branch);
        check(decode_index_chunk(identify_block(index_wire), index_wire) == branch);

        GksManifest manifest{"synthetic", "r1", schema_version, 2,
                             {{IndexKind::Objects, "object-id-v1", 0, leaf_id}}, {id}};
        const auto manifest_wire = encode_gks_manifest(manifest);
        check(decode_gks_manifest(identify_block(manifest_wire), manifest_wire) == manifest);
        GksManifest empty{"empty", "r1", schema_version, 0, {}, {}};
        const auto empty_wire = encode_gks_manifest(empty);
        check(decode_gks_manifest(identify_block(empty_wire), empty_wire) == empty);
        auto invalid_manifest = manifest; invalid_manifest.indexes.clear();
        rejects(ErrorCode::Schema, [&] { encode_gks_manifest(invalid_manifest); });
        invalid_manifest = manifest; invalid_manifest.indexes.push_back(manifest.indexes.front());
        rejects(ErrorCode::Schema, [&] { encode_gks_manifest(invalid_manifest); });
        invalid_manifest = manifest; invalid_manifest.parents.push_back(id);
        rejects(ErrorCode::Schema, [&] { encode_gks_manifest(invalid_manifest); });
        invalid_manifest = manifest; ++invalid_manifest.object_schema;
        rejects(ErrorCode::Schema, [&] { encode_gks_manifest(invalid_manifest); });
        invalid_manifest = manifest; invalid_manifest.name = std::string(1, '\0');
        rejects(ErrorCode::Schema, [&] { encode_gks_manifest(invalid_manifest); });
        std::cout << "GKS format tests passed: " << assertions << " deterministic assertions\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
