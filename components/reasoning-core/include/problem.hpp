#pragma once

#include "relational.hpp"

#include <array>
#include <optional>
#include <string_view>
#include <utility>
#include <variant>

namespace axiom {

// SI axes: mass, length, time, current, temperature, amount, luminous intensity.
using ProblemDimension = std::array<std::int32_t, 7>;
enum class PremiseBasis : std::uint8_t { Formal = 1, Question, Source };
enum class LocatorRole : std::uint8_t { Paragraph = 1, TableHeader, TableCell };
enum class QuantityPrecision : std::uint8_t { Exact = 1, Approximate, Bound, Unspecified };

struct EvidenceLocator {
    LocatorRole role = LocatorRole::Paragraph;
    std::uint32_t offset = 0;
    std::uint32_t length = 0;
    std::string table, row, column;
    std::vector<std::uint32_t> headers;
    bool operator==(const EvidenceLocator&) const = default;
};
struct EvidenceView {
    std::string id;
    PremiseBasis basis = PremiseBasis::Source;
    std::string archive_id, entry_path, extractor_version, digest, text;
    std::vector<EvidenceLocator> locators;
    bool operator==(const EvidenceView&) const = default;
};
struct EvidenceSpan {
    std::uint32_t view = 0;
    std::uint32_t offset = 0;
    std::uint32_t length = 0;
    std::uint32_t locator = no_step;
    bool operator==(const EvidenceSpan&) const = default;
};
struct ProblemOrigin {
    PremiseBasis basis = PremiseBasis::Formal;
    std::string label;
    std::vector<EvidenceSpan> supports;
    bool operator==(const ProblemOrigin&) const = default;
};
struct ProblemScope {
    bool resolved = true;
    std::vector<std::pair<std::string, std::string>> qualifiers;
    bool operator==(const ProblemScope&) const = default;
};
struct ProblemUnit {
    std::string id, symbol;
    ProblemDimension dimension{};
    Rational scale = 1;
    std::vector<std::string> spellings;
    bool operator==(const ProblemUnit&) const = default;
};
struct ProblemNumber {
    EvidenceSpan numeral;
    std::optional<EvidenceSpan> unit_span;
    std::uint32_t unit = 0;
    QuantityPrecision precision = QuantityPrecision::Unspecified;
    std::optional<EvidenceSpan> qualifier;
    bool operator==(const ProblemNumber&) const = default;
};
struct ProblemQuantity {
    std::string property;
    std::uint32_t entity = 0;
    ProblemScope scope;
    std::uint32_t unit = 0;
    bool operator==(const ProblemQuantity&) const = default;
};
struct ProblemBinding {
    std::uint32_t quantity = 0;
    std::uint32_t number = 0;
    ProblemOrigin origin;
    bool operator==(const ProblemBinding&) const = default;
};
struct ProblemQuantityNode {
    std::uint32_t quantity = 0;
    bool operator==(const ProblemQuantityNode&) const = default;
};
struct ProblemNumberNode {
    std::uint32_t number = 0;
    bool operator==(const ProblemNumberNode&) const = default;
};
struct ProblemNegateNode {
    std::uint32_t operand = 0;
    bool operator==(const ProblemNegateNode&) const = default;
};
struct ProblemBinaryNode {
    Code operation = Code::Add;
    std::uint32_t left = 0, right = 0;
    bool operator==(const ProblemBinaryNode&) const = default;
};
using ProblemNode = std::variant<ProblemQuantityNode, ProblemNumberNode,
                                 ProblemNegateNode, ProblemBinaryNode>;
struct ProblemEquation {
    std::uint32_t left = 0, right = 0;
    ProblemScope scope;
    std::optional<Atom> guard;
    ProblemOrigin origin;
    bool operator==(const ProblemEquation&) const = default;
};
struct ProblemQuantityGoal {
    std::uint32_t quantity = 0, unit = 0;
    bool operator==(const ProblemQuantityGoal&) const = default;
};
struct ProblemAtomGoal {
    Atom atom;
    bool operator==(const ProblemAtomGoal&) const = default;
};
using ProblemGoal = std::variant<ProblemQuantityGoal, ProblemAtomGoal>;

// Context identity, unit definitions, limits and formal origins are host-owned.
// A learned proposal adapter must not accept replacements for these fields.
struct ProblemContext {
    std::string unit_registry_id, unit_registry_version;
    std::vector<ProblemUnit> units;
    std::vector<EvidenceView> views;
    bool operator==(const ProblemContext&) const = default;
};
struct ProblemProposal {
    std::vector<std::string> entities;
    std::vector<ProblemQuantity> quantities;
    std::vector<ProblemNumber> numbers;
    std::vector<ProblemBinding> bindings;
    std::vector<ProblemNode> nodes;
    std::vector<ProblemEquation> equations;
    RelationalModule logic;
    std::vector<ProblemOrigin> logic_origins;
    ProblemGoal goal;
    bool operator==(const ProblemProposal&) const = default;
};
struct ProblemLimits {
    std::uint64_t work = 1000000;
    std::uint32_t steps = 256;
    bool operator==(const ProblemLimits&) const = default;
};
struct ProblemRequest {
    ProblemContext context;
    ProblemProposal proposal;
    ProblemLimits limits;
    bool operator==(const ProblemRequest&) const = default;
};
struct AdmittedProblem {
    std::vector<Rational> lexical_numbers;
    std::vector<Rational> canonical_numbers;
    std::vector<ProblemDimension> node_dimensions;
    std::uint64_t admission_work = 0;
};

enum class ProblemStatus : std::uint8_t { Checked = 1, Unresolved, Conflict, Unsupported, Limited };
enum class ProblemIssue : std::uint8_t {
    None = 0, MissingPremise, NonExact, NonAffine, Undefined, WorkLimit, WireLimit
};
enum class ProblemPremiseKind : std::uint8_t { Number = 1, Binding, Equation, Rule };
struct ProblemPremiseRef {
    ProblemPremiseKind kind = ProblemPremiseKind::Number;
    std::uint32_t index = 0;
    bool operator==(const ProblemPremiseRef&) const = default;
};
struct ProblemBindingStep {
    std::uint32_t binding = 0;
    Rational lexical_value = 0, unit_scale = 1;
    bool operator==(const ProblemBindingStep&) const = default;
};
struct ProblemEquationStep {
    std::uint32_t equation = 0;
    std::vector<std::uint32_t> dependencies;
    std::optional<std::uint32_t> guard;
    bool operator==(const ProblemEquationStep&) const = default;
};
struct ProblemStep {
    std::uint32_t quantity = 0;
    Rational canonical_value = 0;
    std::variant<ProblemBindingStep, ProblemEquationStep> justification;
    bool operator==(const ProblemStep&) const = default;
};
struct ProblemGuardProof {
    Atom goal;
    RelationalResult certificate;
};
struct ProblemValueConflict {
    std::uint32_t first = 0, second = 0;
    bool operator==(const ProblemValueConflict&) const = default;
};
struct ProblemEquationConflict {
    std::uint32_t equation = 0;
    std::vector<std::uint32_t> dependencies;
    std::optional<std::uint32_t> guard;
    bool operator==(const ProblemEquationConflict&) const = default;
};
struct ProblemGuardConflict {
    std::uint32_t guard = 0;
    bool operator==(const ProblemGuardConflict&) const = default;
};
using ProblemConflict = std::variant<ProblemValueConflict, ProblemEquationConflict, ProblemGuardConflict>;
struct ProblemPendingEquation {
    std::uint32_t equation = 0;
    ProblemIssue issue = ProblemIssue::MissingPremise;
    bool operator==(const ProblemPendingEquation&) const = default;
};
struct ProblemResult {
    ProblemRequest request;
    ProblemStatus status = ProblemStatus::Unresolved;
    ProblemIssue issue = ProblemIssue::MissingPremise;
    std::vector<ProblemStep> steps;
    std::vector<ProblemGuardProof> guards;
    std::optional<ProblemConflict> conflict;
    std::optional<std::uint32_t> goal_step, logical_goal_guard;
    std::optional<Rational> answer;
    std::uint32_t answer_unit = no_step;
    std::vector<ProblemPendingEquation> pending_equations;
    std::vector<ProblemPremiseRef> used;
    bool formal_assumptions = false, question_assumptions = false, source_interpretations = false;
    std::uint64_t work = 0;
};

std::string evidence_digest(std::string_view bytes);
EvidenceView make_evidence_view(std::string id, PremiseBasis basis, std::string text,
                               std::string archive_id, std::string entry_path,
                               std::string extractor_version);
Rational parse_evidence_number(std::string_view numeral);
Rational checked_problem_rational(Rational value);
AdmittedProblem admit_problem(const ProblemRequest& request);
// Source-only validation of the same theory/context contract. The request's
// goal field is ignored; no query is invented and no reasoning is performed.
// Executable requests must still pass admit_problem, including its goal checks.
AdmittedProblem admit_problem_theory(const ProblemRequest& request);
ProblemResult execute_problem(const ProblemRequest& request);
bool verify_problem_result(const ProblemRequest& request, const ProblemResult& result);

struct ProblemCorrectionAttempt {
    ProblemResult result;
    std::string correction_reason;
    std::optional<std::uint32_t> rejected_previous_step;
};

// Corrections are supplied by a reviewer/checker, not inferred from certificate
// validity. Each attempt recomputes a fixed-context snapshot without old values.
class ProblemCorrectionSession {
public:
    explicit ProblemCorrectionSession(const ProblemRequest& initial, std::uint32_t attempts = 3);
    void correct(const ProblemProposal& proposal, std::string reason,
                 std::optional<std::uint32_t> rejected_previous_step = std::nullopt);
    const std::vector<ProblemCorrectionAttempt>& attempts() const { return attempts_; }
    std::uint64_t work() const { return work_; }

private:
    void append(const ProblemProposal& proposal, std::string reason,
                std::optional<std::uint32_t> rejected_previous_step);
    ProblemRequest initial_;
    std::uint32_t limit_;
    std::uint64_t work_ = 0;
    std::vector<ProblemCorrectionAttempt> attempts_;
};

} // namespace axiom
