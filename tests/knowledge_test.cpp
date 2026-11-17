#include "knowledge.hpp"

#include <algorithm>
#include <iostream>
#include <limits>

namespace {
using namespace axiom::knowledge;
std::size_t checks = 0;

void check(bool condition) {
    ++checks;
    if (!condition) {
        throw std::runtime_error("Knowledge assertion " + std::to_string(checks));
    }
}

template<class F> void rejects(ErrorCode expected, F&& operation) {
    ++checks;
    try {
        operation();
    } catch (const Error& error) {
        if (error.code == expected) {
            return;
        }
        throw;
    }
    throw std::runtime_error("Expected knowledge rejection " + std::to_string(checks));
}

ObjectId id(std::string_view value) {
    return {digest({reinterpret_cast<const std::uint8_t*>(value.data()), value.size()})};
}

Object fixture() {
    Object result;
    result.format = "application/octet-stream";
    result.label = "source-bound claim";
    result.payload = {0, 255, 1, 128, 0};
    result.concepts = {"scope", "negative", "scope"};
    result.sources = {{"archive", "version-1", "A/source", "extractor-1",
                       id("source").digest, 10, 20}};
    return result;
}

void encoding() {
    check(hex(id("abc")) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    check(parse_id(hex(id("abc"))) == id("abc"));
    rejects(ErrorCode::Schema, [] { static_cast<void>(parse_id("a")); });
    rejects(ErrorCode::Schema, [] { static_cast<void>(parse_id(std::string(64, 'A'))); });
    rejects(ErrorCode::Schema, [] { static_cast<void>(parse_id(std::string(64, 'g'))); });

    Object minimal;
    minimal.format = "x";
    const Bytes golden = {
        'G', 'K', 'O', 'B', 1, 0, 1, 1, 0, 0, 0, 0,
        1, 0, 0, 0, 'x', 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0
    };
    check(encode_object(minimal) == golden);
    check(decode_object(golden) == minimal);

    auto object = fixture();
    for (unsigned kind = 1; kind <= 12; ++kind) {
        object.kind = static_cast<ObjectKind>(kind);
        object.derivation.reset();
        if (object.kind == ObjectKind::Derivation) {
            object.derivation = Derivation{id("conclusion"), id("rule"),
                {id("p"), id("q"), id("p")}, {id("assumption")}, {id("evidence")}};
        }
        object.links.clear();
        for (unsigned relationship = 1; relationship <= 8; ++relationship) {
            object.links.push_back({static_cast<Relationship>(relationship), id("linked")});
        }
        const auto wire = encode_object(object);
        check(decode_object(wire) == object);
        check(encode_object(decode_object(wire)) == wire);
        check(decode_object(identify(object), wire) == object);
        for (std::size_t n = 0; n < wire.size(); ++n) {
            rejects(ErrorCode::Schema, [&] {
                static_cast<void>(decode_object(std::span<const std::uint8_t>(wire).first(n)));
            });
        }
    }

    object = fixture();
    const auto original = identify(object);
    auto changed = object;
    changed.label += " changed";
    check(identify(changed) != original);
    changed = object;
    changed.payload[0] ^= 1;
    check(identify(changed) != original);
    changed = object;
    changed.format += "+v2";
    check(identify(changed) != original);
    changed = object;
    changed.visibility = Visibility::Public;
    check(identify(changed) != original);
    changed = object;
    changed.sources.front().offset += 1;
    check(identify(changed) != original);
    changed = object;
    changed.concepts.push_back("new");
    check(identify(changed) != original);
    changed = object;
    changed.links.push_back({Relationship::Contradicts, original});
    check(identify(changed) != original);
    changed = object;
    changed.label = "\xcf\x80";
    check(decode_object(encode_object(changed)) == changed);

    auto wire = encode_object(object);
    wire.back() = 2;
    rejects(ErrorCode::Schema, [&] { static_cast<void>(decode_object(wire)); });
    rejects(ErrorCode::Integrity, [&] { static_cast<void>(decode_object(original, wire)); });
    wire = encode_object(object);
    wire.push_back(0);
    rejects(ErrorCode::Schema, [&] { static_cast<void>(decode_object(wire)); });
    for (const auto [position, value] : std::vector<std::pair<std::size_t, std::uint8_t>>{
            {0, 0}, {4, 2}, {6, 0}, {6, 13}, {7, 0}, {7, 3}}) {
        wire = encode_object(object);
        wire[position] = value;
        rejects(ErrorCode::Schema, [&] { static_cast<void>(decode_object(wire)); });
    }
    wire = golden;
    std::fill(wire.begin() + 17, wire.begin() + 21, 255);
    rejects(ErrorCode::Budget, [&] { static_cast<void>(decode_object(wire)); });
}

void bounds() {
    auto object = fixture();
    object.format.clear();
    rejects(ErrorCode::Schema, [&] { static_cast<void>(encode_object(object)); });
    object = fixture();
    object.derivation.emplace();
    rejects(ErrorCode::Schema, [&] { static_cast<void>(encode_object(object)); });
    object.derivation.reset();
    object.kind = ObjectKind::Derivation;
    rejects(ErrorCode::Schema, [&] { static_cast<void>(encode_object(object)); });
    object = fixture();
    object.version = 2;
    rejects(ErrorCode::Schema, [&] { static_cast<void>(encode_object(object)); });
    object = fixture();
    object.links = {{static_cast<Relationship>(9), id("other")}};
    rejects(ErrorCode::Schema, [&] { static_cast<void>(encode_object(object)); });
    object = fixture();
    object.sources.front().offset = std::numeric_limits<std::uint64_t>::max();
    rejects(ErrorCode::Schema, [&] { static_cast<void>(encode_object(object)); });

    for (const auto& invalid : std::vector<std::string>{
            std::string("x\0y", 3), "\x80", "\xc0\xaf", "\xe0\x80\xaf",
            "\xed\xa0\x80", "\xf4\x90\x80\x80", "\xf5\x80\x80\x80", "\xe2\x82"}) {
        object = fixture();
        object.label = invalid;
        rejects(ErrorCode::Schema, [&] { static_cast<void>(encode_object(object)); });
    }
    object = fixture();
    object.label.assign(maximum_text_bytes + 1, 'x');
    rejects(ErrorCode::Budget, [&] { static_cast<void>(encode_object(object)); });
    object = fixture();
    object.concepts.assign(maximum_items + 1, "x");
    rejects(ErrorCode::Budget, [&] { static_cast<void>(encode_object(object)); });
    object = fixture();
    object.payload.assign(maximum_object_bytes, 0);
    rejects(ErrorCode::Budget, [&] { static_cast<void>(encode_object(object)); });
    const Bytes oversized(maximum_object_bytes + 1);
    rejects(ErrorCode::Budget, [&] { static_cast<void>(decode_object(oversized)); });
}

void dependencies() {
    auto object = fixture();
    object.kind = ObjectKind::Derivation;
    object.derivation = Derivation{id("conclusion"), id("rule"),
        {id("p"), id("q"), id("p")}, {id("assumption")}, {id("evidence")}};
    object.links = {{Relationship::Supports, id("optional-support")},
                    {Relationship::AlternativeTo, id("alternative")},
                    {Relationship::Contradicts, id("opposite")},
                    {Relationship::Requires, id("p")},
                    {Relationship::DependsOn, id("dependency")}};
    auto expected = std::vector<ObjectId>{id("p"), id("q"), id("rule"), id("assumption"),
                                         id("evidence"), id("dependency")};
    std::sort(expected.begin(), expected.end());
    check(prerequisites(object) == expected);
    for (const auto name : {"conclusion", "optional-support", "alternative", "opposite"}) {
        expected.push_back(id(name));
    }
    std::sort(expected.begin(), expected.end());
    check(references(object) == expected);
    const auto wire = encode_object(object);
    check(decode_object(wire).derivation->premises ==
          std::vector<ObjectId>({id("p"), id("q"), id("p")}));
    auto reordered = object;
    std::swap(reordered.derivation->premises[0], reordered.derivation->premises[1]);
    check(identify(reordered) != identify(object));
    auto unsupported = object;
    unsupported.links.pop_back();
    check(identify(unsupported) != identify(object));
    check(Assessment{}.logical == LogicalValidity::Unchecked);
    check(Assessment{}.evidence == EvidenceAssessment::Unassessed);
    check(Assessment{}.state == KnowledgeState::Provisional);
    const auto edges = relationships(object);
    check(edges.size() == 12);
    check(edges[5] == Link{Relationship::Produces, id("conclusion")});
    check(edges[6] == Link{Relationship::DependsOn, id("rule")});
    check(std::count_if(edges.begin(), edges.end(), [](const auto& edge) {
        return edge.relationship == Relationship::Requires;
    }) == 5);
}

template<class T> void protocol_names(const std::vector<std::string_view>& expected) {
    for (std::size_t i = 0; i < expected.size(); ++i) {
        check(name(static_cast<T>(i + 1)) == expected[i]);
    }
    rejects(ErrorCode::Schema, [] { static_cast<void>(name(static_cast<T>(0))); });
    rejects(ErrorCode::Schema, [] { static_cast<void>(name(static_cast<T>(255))); });
}

void names_and_shared_text() {
    protocol_names<ObjectKind>({"CLAIM", "FACT", "RULE", "ASSUMPTION", "HYPOTHESIS",
        "DERIVATION", "EVIDENCE", "GOAL", "ACTION", "EXPERIMENT", "TEST_RESULT", "CONTRADICTION"});
    protocol_names<Relationship>({"REQUIRES", "IMPLIES", "SUPPORTS", "CONTRADICTS",
        "DEPENDS_ON", "PRODUCES", "TESTS", "ALTERNATIVE_TO"});
    protocol_names<KnowledgeState>({"PROVISIONAL", "VERIFIED", "DISPUTED",
        "INVALIDATED", "UNRESOLVED", "ARCHIVED"});
    protocol_names<LogicalValidity>({"UNCHECKED", "VALID", "INVALID"});
    protocol_names<EvidenceAssessment>({"UNASSESSED", "SUPPORTED", "CHALLENGED", "CONFLICTING"});
    protocol_names<Visibility>({"PRIVATE", "PUBLIC"});
    const std::string large_text(maximum_text_bytes + 1, 'x');
    rejects(ErrorCode::Budget, [&] { validate_text(large_text); });
    validate_text(large_text, maximum_object_bytes);
    rejects(ErrorCode::Budget, [] { validate_text("x", maximum_object_bytes + 1); });
    rejects(ErrorCode::Schema, [] { validate_text("\xed\xa0\x80", maximum_object_bytes); });
}
} // namespace

int main() {
    try {
        encoding();
        bounds();
        dependencies();
        names_and_shared_text();
        std::cout << "knowledge tests passed: " << checks << " deterministic assertions\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
