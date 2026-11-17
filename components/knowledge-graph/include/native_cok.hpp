#pragma once

#include "reasoning.hpp"

namespace axiom::knowledge {

// NREF is a locator into an independently checked existing native certificate,
// not a new statement/meaning schema. All indices retain that input's scope.
enum class NativeReferenceKind : std::uint8_t {
    Literal = 1, HornRule, ProofStep, GivenInput, BuiltinOperation, MathResult,
    QuantityValue, Binding, Unit, Equation, EvidenceView, QuantityStep, Answer, Conflict
};
struct NativeReference {
    NativeReferenceKind kind;
    ObjectId certificate;
    std::uint32_t section = UINT32_MAX, index = 0;
    bool operator==(const NativeReference&) const = default;
};
NativeReference decode_native_reference(std::span<const std::uint8_t> payload);
struct NativeCoKLimits {
    std::size_t objects = maximum_items;
    std::size_t bytes = 16 * 1024 * 1024;
};
struct NativeCoK {
    std::vector<Object> objects;
    std::vector<ObjectId> derivations, conclusions;
};

// Pure, bounded materialization: verifies the full request/reply pair first.
// No authoritative journal assessments or empirical truth are manufactured.
// Missing/current dependency policy remains the verification engine's job.
// Explicit Requires prerequisites remain conditional derivation assumptions.
NativeCoK materialize_native(const Object& goal, const Object& certificate,
                            NativeCoKLimits limits = {});

} // namespace axiom::knowledge
