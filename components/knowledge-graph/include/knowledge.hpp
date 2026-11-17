#pragma once

#include <array>
#include <compare>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace axiom::knowledge {

using Bytes = std::vector<std::uint8_t>;
using Digest = std::array<std::uint8_t, 32>;

// An object identity is not an IPFS block CID.
struct ObjectId {
    Digest digest{};
    auto operator<=>(const ObjectId&) const = default;
};

inline constexpr std::uint16_t schema_version = 1;
inline constexpr std::size_t maximum_object_bytes = 4 * 1024 * 1024;
inline constexpr std::size_t maximum_text_bytes = 65536;
inline constexpr std::size_t maximum_items = 4096;
inline constexpr std::size_t maximum_assessment_dependencies = 4 * maximum_items + 1;

enum class ObjectKind : std::uint8_t {
    Claim = 1, Fact, Rule, Assumption, Hypothesis, Derivation,
    Evidence, Goal, Action, Experiment, TestResult, Contradiction
};
enum class Relationship : std::uint8_t {
    Requires = 1, Implies, Supports, Contradicts, DependsOn, Produces, Tests, AlternativeTo
};
enum class KnowledgeState : std::uint8_t {
    Provisional = 1, Verified, Disputed, Invalidated, Unresolved, Archived
};
enum class LogicalValidity : std::uint8_t { Unchecked = 1, Valid, Invalid };
enum class EvidenceAssessment : std::uint8_t {
    Unassessed = 1, Supported, Challenged, Conflicting
};
// Public describes source disclosure, not permission to publish a contribution.
enum class Visibility : std::uint8_t { Private = 1, Public };
enum class ErrorCode { Schema, Integrity, Storage, Missing, Budget, Permission, Unavailable };

std::string_view name(ObjectKind value);
std::string_view name(Relationship value);
std::string_view name(KnowledgeState value);
std::string_view name(LogicalValidity value);
std::string_view name(EvidenceAssessment value);
std::string_view name(Visibility value);

struct Error : std::runtime_error {
    ErrorCode code;
    Error(ErrorCode value, std::string message)
        : std::runtime_error(std::move(message)), code(value) {}
};

struct SourceReference {
    std::string archive_id, archive_version, entry_path, extractor_version;
    Digest source_digest{};
    std::uint64_t offset = 0, length = 0;
    bool operator==(const SourceReference&) const = default;
};
struct Link {
    Relationship relationship = Relationship::DependsOn;
    ObjectId target;
    bool operator==(const Link&) const = default;
};
struct Derivation {
    ObjectId conclusion, rule;
    // Ordered, jointly necessary premises; never flattened into alternatives.
    std::vector<ObjectId> premises, assumptions, evidence;
    bool operator==(const Derivation&) const = default;
};
struct Object {
    std::uint16_t version = schema_version;
    ObjectKind kind = ObjectKind::Claim;
    Visibility visibility = Visibility::Private;
    std::string label;
    // Identifies an existing payload contract, not a replacement meaning schema.
    // Formal payloads retain the existing admitted IR/binary representation.
    std::string format;
    Bytes payload;
    std::vector<std::string> concepts;
    std::vector<SourceReference> sources;
    std::vector<Link> links;
    std::optional<Derivation> derivation;
    bool operator==(const Object&) const = default;
};

// Assessments are versioned separately: revalidation never rewrites object IDs.
// Formal validity does not imply empirical truth or dependency availability.
struct Assessment {
    std::uint64_t version = 0;
    KnowledgeState state = KnowledgeState::Provisional;
    LogicalValidity logical = LogicalValidity::Unchecked;
    EvidenceAssessment evidence = EvidenceAssessment::Unassessed;
    std::string checker, reason;
    std::optional<ObjectId> certificate;
    std::vector<ObjectId> checked_dependencies;
    bool operator==(const Assessment&) const = default;
};

Digest digest(std::span<const std::uint8_t> bytes);
void validate_text(std::string_view value, std::size_t maximum = maximum_text_bytes);
// Shared input to the journal and snapshot unicode61 text indexes.
std::string indexed_text(const Object& object);
std::string hex(const ObjectId& id);
ObjectId parse_id(std::string_view text);
Bytes encode_object(const Object& object);
Object decode_object(std::span<const std::uint8_t> bytes);
ObjectId identify(const Object& object);
Object decode_object(const ObjectId& expected, std::span<const std::uint8_t> bytes);

// These return unique identities without altering ordered source/IR arrays.
std::vector<ObjectId> references(const Object& object);
std::vector<ObjectId> prerequisites(const Object& object);
// Include structured derivation fields in graph traversal/indexing without
// rewriting the canonical object or flattening its ordered premise list.
std::vector<Link> relationships(const Object& object);

} // namespace axiom::knowledge
