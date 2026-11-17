#include "reasoning.hpp"

#include "core.hpp"
#include "binary_dispatch.hpp"
#include "forward_reasoning.hpp"
#include "problem_wire.hpp"
#include "relational.hpp"

#include <algorithm>
#include <stdexcept>
#include <set>

namespace axiom::knowledge {
namespace {

std::uint16_t family(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < 12 || bytes.size() > max_wire_bytes ||
        bytes[0] != 'A' || bytes[1] != 'X' || bytes[2] != 'R' || bytes[3] != 'M') {
        throw std::runtime_error("Invalid native certificate envelope");
    }
    return static_cast<std::uint16_t>(bytes[4] |
        (static_cast<std::uint16_t>(bytes[5]) << 8));
}

NativeReplyCheck proof_status(const Result& result) {
    return {true, result.status != Status::Unknown,
            result.status == Status::Contradiction};
}

} // namespace

NativeReplyCheck check_native_reply(std::span<const std::uint8_t> request,
                                    std::span<const std::uint8_t> reply) {
    try {
        const auto version = family(request);
        if (version != family(reply)) return {};
        switch (version) {
        case 1: {
            const auto input = decode_request(request);
            const auto result = decode_result(reply);
            if (input.operation != result.operation || input.gpu != result.gpu) return {};
            if (input.operation == Operation::Prove) {
                auto echoed = input;
                echoed.module = result.module;
                echoed.goal = result.goal;
                if (encode_request(echoed) != encode_request(input) ||
                    result.history.size() > input.limits.events ||
                    std::any_of(result.history.begin(), result.history.end(),
                        [&](const auto& event) { return event.depth > input.limits.depth + 1; })) {
                    return {};
                }
                // decode_result independently checks every proof step, both
                // signed roots, status and contradiction closure.
                return proof_status(result);
            }
            // Legacy arithmetic replies contain no input echo or independently
            // replayable arithmetic proof. Recompute rather than trust values;
            // this path makes no claim of arithmetic cache acceleration.
            if (encode_result(execute(input)) != Bytes(reply.begin(), reply.end())) return {};
            return {true, true, false};
        }
        case 2: {
            const auto input = decode_relational_request(request);
            const auto result = decode_relational_result(reply);
            if (!verify_relational_result(input, result)) return {};
            return proof_status(result.proof);
        }
        case 3: {
            const auto input = decode_problem_request(request);
            const auto result = decode_problem_result(reply);
            if (!verify_problem_result(input, result)) return {};
            return {true,
                result.status == ProblemStatus::Checked || result.status == ProblemStatus::Conflict,
                result.status == ProblemStatus::Conflict};
        }
        default:
            return {};
        }
    } catch (const std::runtime_error&) {
        return {};
    }
}

Object native_execution_record(const Object& goal, const ObjectId& request,
                                std::span<const std::uint8_t> reply) {
    Object record;
    record.kind = ObjectKind::TestResult;
    record.visibility = goal.visibility;
    record.label = "Native execution certificate";
    record.format = native_reply_format;
    record.payload.assign(reply.begin(), reply.end());
    record.sources = goal.sources;
    record.links = {{Relationship::Tests, request}, {Relationship::DependsOn, request}};
    return record;
}

NativeRun run_native(Journal& journal, const ObjectId& request, NativeRunLimits limits) {
    if (limits.maximum_dependencies == 0 || limits.maximum_dependencies > maximum_items ||
        limits.maximum_certificates == 0 || limits.maximum_certificates > maximum_items ||
        (limits.strategy != NativeStrategy::Existing && limits.strategy != NativeStrategy::Forward)) {
        throw Error(ErrorCode::Budget, "Invalid native reuse budget");
    }
    NativeRun run;
    const auto goal = journal.get(request);
    if (!goal) {
        run.unavailable.push_back(request);
        return run;
    }
    if (goal->kind != ObjectKind::Goal || goal->format != native_request_format) {
        throw Error(ErrorCode::Schema, "Expected an admitted native formal goal");
    }
    if (limits.strategy == NativeStrategy::Forward) {
        const auto version = family(goal->payload);
        if (version == 1) {
            const auto input = decode_request(goal->payload);
            if (input.operation != Operation::Prove || input.gpu) {
                throw Error(ErrorCode::Schema, "Forward strategy requires CPU finite-Horn input");
            }
        } else if (version == 2) {
            if (decode_relational_request(goal->payload).gpu) {
                throw Error(ErrorCode::Schema, "Forward strategy requires CPU finite-Horn input");
            }
        } else {
            throw Error(ErrorCode::Schema, "Forward strategy supports native finite-Horn requests only");
        }
    }
    std::vector<ObjectId> pending{request};
    std::set<ObjectId> discovered{request};
    std::vector<ObservedAssessment> observations;
    while (!pending.empty()) {
        const auto id = pending.back();
        pending.pop_back();
        const auto object = id == request ? goal : journal.get(id);
        const auto assessment = journal.assessment(id);
        observations.push_back({id, assessment ? assessment->version : 0});
        if (!object) {
            run.unavailable.push_back(id);
            continue;
        }
        if (assessment && (assessment->state == KnowledgeState::Invalidated ||
                           assessment->logical == LogicalValidity::Invalid)) {
            run.unavailable.push_back(id);
        }
        auto dependencies = prerequisites(*object);
        if (assessment) {
            dependencies.insert(dependencies.end(), assessment->checked_dependencies.begin(),
                                assessment->checked_dependencies.end());
            if (assessment->certificate && *assessment->certificate != id) {
                dependencies.push_back(*assessment->certificate);
            }
        }
        for (const auto& dependency : dependencies) {
            if (discovered.insert(dependency).second) {
                if (discovered.size() > limits.maximum_dependencies) {
                    throw Error(ErrorCode::Budget, "Native prerequisite closure exceeded budget");
                }
                pending.push_back(dependency);
            }
        }
    }
    if (!run.unavailable.empty()) {
        std::sort(run.unavailable.begin(), run.unavailable.end());
        return run;
    }
    const auto record_for = [&](const Bytes& reply) {
        return native_execution_record(*goal, request, reply);
    };
    const auto record_check = [&](const ObjectId& id, const NativeReplyCheck& checked,
                                  std::optional<std::uint64_t> previous) {
        Assessment value;
        value.state = checked.conclusive ? KnowledgeState::Verified : KnowledgeState::Unresolved;
        value.logical = checked.conclusive ? LogicalValidity::Valid : LogicalValidity::Unchecked;
        value.checker = "native-request-bound-replay-v1";
        value.certificate = id;
        for (const auto& observation : observations) {
            value.checked_dependencies.push_back(observation.object);
        }
        auto guarded = observations;
        const auto prior = std::find_if(guarded.begin(), guarded.end(),
            [&](const auto& observation) { return observation.object == id; });
        if (prior == guarded.end()) {
            guarded.push_back({id, previous.value_or(0)});
        } else if (prior->version != previous.value_or(0)) {
            throw Error(ErrorCode::Unavailable, "Certificate assessment changed during verification");
        }
        journal.record_revalidated_assessment(id, std::move(value), guarded);
    };
    std::optional<ObjectId> after;
    std::size_t examined = 0;
    for (;;) {
        const auto page = journal.related(request, Relationship::Tests, after,
                                          std::min<std::size_t>(64, limits.maximum_certificates));
        for (const auto& id : page.objects) {
            if (++examined > limits.maximum_certificates) {
                throw Error(ErrorCode::Budget, "Native certificate lookup exceeded budget");
            }
            const auto candidate = journal.get(id);
            if (!candidate || candidate->kind != ObjectKind::TestResult ||
                candidate->format != native_reply_format) continue;
            const auto assessment = journal.assessment(id);
            if (!assessment || assessment->state != KnowledgeState::Verified ||
                assessment->logical != LogicalValidity::Valid) continue;
            const auto checked = check_native_reply(goal->payload, candidate->payload);
            if (!checked.valid || !checked.conclusive || identify(record_for(candidate->payload)) != id) {
                run.rejected_certificates.push_back(id);
                continue;
            }
            record_check(id, checked, assessment->version);
            run.check = checked;
            run.reply = candidate->payload;
            run.certificate = id;
            run.reused = true;
            return run;
        }
        if (page.complete) break;
        after = page.next;
    }
    if (limits.strategy == NativeStrategy::Existing) {
        run.reply = run_request_binary(goal->payload);
    } else if (family(goal->payload) == 1) {
        auto forward = reason_forward(decode_request(goal->payload), limits.forward_work);
        run.producer_work = forward.work;
        run.reply = encode_result(forward.reply);
    } else if (family(goal->payload) == 2) {
        const auto input = decode_relational_request(goal->payload);
        auto grounded = ground_relational(input);
        auto forward = reason_forward(grounded.request, limits.forward_work);
        run.producer_work = forward.work;
        run.reply = encode_relational_result({input, std::move(grounded.instances),
                                               std::move(forward.reply)});
    } else {
        throw Error(ErrorCode::Schema, "Forward strategy supports native finite-Horn requests only");
    }
    run.check = check_native_reply(goal->payload, run.reply);
    if (!run.check.valid) {
        throw Error(ErrorCode::Integrity, "Native execution failed request-bound verification");
    }
    const auto id = journal.put(record_for(run.reply));
    const auto previous = journal.assessment(id);
    record_check(id, run.check, previous ? std::optional(previous->version) : std::nullopt);
    run.certificate = id;
    return run;
}

} // namespace axiom::knowledge
