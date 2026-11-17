#include "native_cok.hpp"
#include "core.hpp"
#include "problem_wire.hpp"
#include "relational.hpp"

#include <algorithm>
#include <set>
#include <type_traits>

namespace axiom::knowledge {
namespace {
constexpr std::string_view reference_format = "application/x-axiom-native-reference-v1";
void u32(Bytes& bytes, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
        bytes.push_back(static_cast<std::uint8_t>(value >> shift));
    }
}
Bytes encode_reference(NativeReferenceKind kind, const ObjectId& certificate,
                       std::uint32_t section, std::uint32_t index) {
    Bytes result{'N', 'R', 'E', 'F', 1, static_cast<std::uint8_t>(kind)};
    result.insert(result.end(), certificate.digest.begin(), certificate.digest.end());
    u32(result, section);
    u32(result, index);
    return result;
}

struct Builder {
    const Object& goal;
    ObjectId request, certificate;
    NativeCoKLimits limits;
    NativeCoK output;
    std::set<ObjectId> emitted;
    std::size_t bytes = 0;

    ObjectId put(Object object) {
        const auto encoded = encode_object(object);
        const ObjectId id{digest(encoded)};
        if (emitted.contains(id)) return id;
        if (output.objects.size() == limits.objects || encoded.size() > limits.bytes - bytes) {
            throw Error(ErrorCode::Budget, "Native CoK materialization budget exceeded");
        }
        bytes += encoded.size();
        emitted.insert(id);
        output.objects.push_back(std::move(object));
        return id;
    }
    Object reference(ObjectKind kind, NativeReferenceKind role, std::uint32_t section,
                     std::uint32_t index) const {
        Object result;
        result.kind = kind;
        result.visibility = goal.visibility;
        result.label = "Scoped native certificate reference";
        result.format = reference_format;
        result.payload = encode_reference(role, certificate, section, index);
        result.sources = goal.sources;
        result.links = {{Relationship::DependsOn, request}, {Relationship::DependsOn, certificate}};
        return result;
    }
    ObjectId ref(ObjectKind kind, NativeReferenceKind role, std::uint32_t section,
                 std::uint32_t index) {
        return put(reference(kind, role, section, index));
    }
    void derive(NativeReferenceKind role, std::uint32_t section, std::uint32_t index,
                const ObjectId& conclusion, const ObjectId& rule,
                std::vector<ObjectId> premises, std::vector<ObjectId> assumptions,
                std::vector<ObjectId> evidence = {}) {
        for (const auto& link : goal.links)
            if (link.relationship == Relationship::Requires) assumptions.push_back(link.target);
        auto object = reference(ObjectKind::Derivation, role, section, index);
        object.derivation = Derivation{conclusion, rule, std::move(premises),
                                      std::move(assumptions), std::move(evidence)};
        const auto id = put(std::move(object));
        output.derivations.push_back(id);
    }
    std::vector<ObjectId> logic(const Result& result, std::uint32_t section) {
        std::vector<ObjectId> claims;
        for (std::uint32_t i = 0; i < result.proof.size(); ++i) {
            const auto& step = result.proof[i];
            const auto claim = ref(ObjectKind::Claim, NativeReferenceKind::Literal, section, step.literal);
            const auto rule = ref(ObjectKind::Rule, NativeReferenceKind::HornRule, section, step.rule);
            std::vector<ObjectId> premises;
            for (const auto dependency : step.dependencies) premises.push_back(claims.at(dependency));
            // The input theory declares this rule; source truth is not inferred
            // merely because native rule application is formally valid.
            derive(NativeReferenceKind::ProofStep, section, i, claim, rule,
                   std::move(premises), {rule});
            claims.push_back(claim);
        }
        std::vector<ObjectId> roots;
        if (result.positive_root != no_step) roots.push_back(claims.at(result.positive_root));
        if (result.negative_root != no_step) roots.push_back(claims.at(result.negative_root));
        return roots;
    }
    ObjectId conflict(std::uint32_t section, std::vector<ObjectId> statements) {
        auto object = reference(ObjectKind::Contradiction, NativeReferenceKind::Conflict, section, 0);
        for (const auto& statement : statements) {
            object.links.push_back({Relationship::Contradicts, statement});
        }
        return put(std::move(object));
    }
    std::vector<ObjectId> evidence(const ProblemOrigin& origin) {
        std::vector<ObjectId> result;
        std::set<std::uint32_t> views;
        for (const auto& support : origin.supports) {
            if (views.insert(support.view).second) {
                result.push_back(ref(ObjectKind::Evidence, NativeReferenceKind::EvidenceView,
                                     no_step, support.view));
            }
        }
        return result;
    }
    void problem(const ProblemResult& result) {
        std::vector<std::vector<ObjectId>> guards;
        for (std::uint32_t i = 0; i < result.guards.size(); ++i) {
            guards.push_back(logic(result.guards[i].certificate.proof, i));
        }
        std::vector<ObjectId> values;
        for (std::uint32_t i = 0; i < result.steps.size(); ++i) {
            const auto& step = result.steps[i];
            const auto claim = ref(ObjectKind::Claim, NativeReferenceKind::QuantityValue, no_step, i);
            std::vector<ObjectId> premises, assumptions, supports;
            ObjectId rule;
            if (const auto* binding = std::get_if<ProblemBindingStep>(&step.justification)) {
                const auto& input = result.request.proposal.bindings.at(binding->binding);
                const auto given = ref(ObjectKind::Claim, NativeReferenceKind::Binding,
                                       no_step, binding->binding);
                const auto unit = ref(ObjectKind::Claim, NativeReferenceKind::Unit, no_step,
                    result.request.proposal.numbers.at(input.number).unit);
                rule = ref(ObjectKind::Rule, NativeReferenceKind::BuiltinOperation, no_step, 1);
                premises = {given, unit};
                assumptions = premises;
                supports = evidence(input.origin);
            } else {
                const auto& equation = std::get<ProblemEquationStep>(step.justification);
                rule = ref(ObjectKind::Rule, NativeReferenceKind::Equation, no_step, equation.equation);
                for (const auto dependency : equation.dependencies) premises.push_back(values.at(dependency));
                if (equation.guard) {
                    const auto& proof = result.guards.at(*equation.guard).certificate.proof;
                    if (proof.positive_root == no_step) {
                        throw Error(ErrorCode::Integrity, "Quantity step lacks its checked guard");
                    }
                    premises.push_back(guards.at(*equation.guard).front());
                }
                assumptions = {rule};
                supports = evidence(result.request.proposal.equations.at(equation.equation).origin);
            }
            derive(NativeReferenceKind::QuantityStep, no_step, i, claim, rule,
                   std::move(premises), std::move(assumptions), std::move(supports));
            values.push_back(claim);
        }
        if (result.status == ProblemStatus::Checked && result.goal_step) {
            const auto answer = ref(ObjectKind::Claim, NativeReferenceKind::Answer, no_step, 0);
            const auto conversion = ref(ObjectKind::Rule, NativeReferenceKind::BuiltinOperation, no_step, 2);
            const auto unit = ref(ObjectKind::Claim, NativeReferenceKind::Unit, no_step, result.answer_unit);
            derive(NativeReferenceKind::Answer, no_step, 0, answer, conversion,
                   {values.at(*result.goal_step), unit}, {unit});
            output.conclusions.push_back(answer);
        } else if (result.status == ProblemStatus::Checked && result.logical_goal_guard) {
            const auto& roots = guards.at(*result.logical_goal_guard);
            output.conclusions.insert(output.conclusions.end(), roots.begin(), roots.end());
        } else if (result.status == ProblemStatus::Conflict && result.conflict) {
            std::vector<ObjectId> statements;
            std::visit([&](const auto& conflict_value) {
                using T = std::decay_t<decltype(conflict_value)>;
                if constexpr (std::is_same_v<T, ProblemValueConflict>) {
                    statements = {values.at(conflict_value.first), values.at(conflict_value.second)};
                } else if constexpr (std::is_same_v<T, ProblemEquationConflict>) {
                    statements.push_back(ref(ObjectKind::Rule, NativeReferenceKind::Equation,
                                              no_step, conflict_value.equation));
                    for (const auto dependency : conflict_value.dependencies) {
                        statements.push_back(values.at(dependency));
                    }
                    if (conflict_value.guard) {
                        const auto& roots = guards.at(*conflict_value.guard);
                        statements.insert(statements.end(), roots.begin(), roots.end());
                    }
                } else {
                    statements = guards.at(conflict_value.guard);
                }
            }, *result.conflict);
            output.conclusions.push_back(conflict(no_step, std::move(statements)));
        }
    }
};
} // namespace

NativeReference decode_native_reference(std::span<const std::uint8_t> payload) {
    if (payload.size() != 46 || payload[0] != 'N' || payload[1] != 'R' ||
        payload[2] != 'E' || payload[3] != 'F' || payload[4] != 1 ||
        payload[5] < 1 || payload[5] > static_cast<std::uint8_t>(NativeReferenceKind::Conflict)) {
        throw Error(ErrorCode::Schema, "Invalid native reference locator");
    }
    NativeReference result{static_cast<NativeReferenceKind>(payload[5]), {}, no_step, 0};
    std::copy_n(payload.begin() + 6, 32, result.certificate.digest.begin());
    const auto number = [&](std::size_t offset) {
        std::uint32_t value = 0;
        for (unsigned shift = 0; shift < 32; shift += 8) {
            value |= static_cast<std::uint32_t>(payload[offset + shift / 8]) << shift;
        }
        return value;
    };
    result.section = number(38);
    result.index = number(42);
    return result;
}

NativeCoK materialize_native(const Object& goal, const Object& certificate, NativeCoKLimits limits) {
    if (!limits.objects || limits.objects > maximum_items || !limits.bytes ||
        limits.bytes > 64 * 1024 * 1024) {
        throw Error(ErrorCode::Budget, "Invalid native materialization limits");
    }
    const auto request = identify(goal), record = identify(certificate);
    if (goal.kind != ObjectKind::Goal || goal.format != native_request_format ||
        identify(native_execution_record(goal, request, certificate.payload)) != record ||
        !check_native_reply(goal.payload, certificate.payload).valid) {
        throw Error(ErrorCode::Integrity, "Native materialization requires an exact checked certificate");
    }
    Builder builder{goal, request, record, limits, {}, {}, 0};
    const auto version = static_cast<unsigned>(goal.payload[4] |
        (static_cast<unsigned>(goal.payload[5]) << 8));
    if (version == 1) {
        const auto result = decode_result(certificate.payload);
        if (result.operation == Operation::Prove) {
            auto roots = builder.logic(result, no_step);
            if (result.status == Status::Contradiction) {
                builder.output.conclusions.push_back(builder.conflict(no_step, std::move(roots)));
            } else if (result.status != Status::Unknown) {
                builder.output.conclusions = std::move(roots);
            }
        } else {
            const auto given = builder.ref(ObjectKind::Claim, NativeReferenceKind::GivenInput, no_step, 0);
            const auto rule = builder.ref(ObjectKind::Rule, NativeReferenceKind::BuiltinOperation,
                                           no_step, static_cast<std::uint32_t>(result.operation));
            const auto answer = builder.ref(ObjectKind::Claim, NativeReferenceKind::MathResult, no_step, 0);
            builder.derive(NativeReferenceKind::MathResult, no_step, 0, answer, rule, {given}, {given});
            builder.output.conclusions.push_back(answer);
        }
    } else if (version == 2) {
        const auto result = decode_relational_result(certificate.payload);
        auto roots = builder.logic(result.proof, no_step);
        if (result.proof.status == Status::Contradiction) {
            builder.output.conclusions.push_back(builder.conflict(no_step, std::move(roots)));
        } else if (result.proof.status != Status::Unknown) {
            builder.output.conclusions = std::move(roots);
        }
    } else {
        builder.problem(decode_problem_result(certificate.payload));
    }
    return std::move(builder.output);
}

} // namespace axiom::knowledge
