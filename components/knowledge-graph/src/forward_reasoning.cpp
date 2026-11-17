#include "forward_reasoning.hpp"
#include "knowledge.hpp"

#include <algorithm>

namespace axiom::knowledge {

ForwardRun reason_forward(const Request& request, std::uint64_t maximum_work) {
    const auto input = decode_request(encode_request(request));
    if (input.operation != Operation::Prove) {
        throw Error(ErrorCode::Schema, "Forward inference requires an existing finite-Horn request");
    }
    if (input.gpu) {
        throw Error(ErrorCode::Permission, "Forward inference is CPU-only");
    }
    ForwardRun run;
    auto& result = run.reply;
    result.operation = Operation::Prove;
    result.module = input.module;
    result.goal = input.goal;
    const auto count = input.module.symbols.size() * 2;
    std::vector<std::uint32_t> proved(count, no_step), depth(count, 0);
    std::vector<bool> depth_blocked(count, false);
    const auto charge = [&] {
        if (run.work == maximum_work) {
            result.limited = true;
            return false;
        }
        ++run.work;
        return true;
    };
    const auto event = [&](EventKind kind, std::uint32_t literal,
                           std::uint32_t rule, std::uint32_t level) {
        if (result.history.size() == input.limits.events) {
            result.limited = true;
            return false;
        }
        result.history.push_back({kind, literal, rule, level});
        return true;
    };
    bool changed = true, stopped = false;
    std::uint32_t round = 0;
    while (changed && !stopped) {
        changed = false;
        for (std::uint32_t id = 0; id < input.module.rules.size(); ++id) {
            if (!charge()) {
                stopped = true;
                break;
            }
            const auto& rule = input.module.rules[id];
            if (proved[rule.head] != no_step) continue;
            if (!event(EventKind::TryRule, rule.head, id, 0)) {
                stopped = true;
                break;
            }
            std::vector<std::uint32_t> dependencies;
            std::uint32_t level = 0;
            bool ready = true;
            for (const auto premise : rule.premises) {
                if (!charge()) {
                    stopped = true;
                    ready = false;
                    break;
                }
                if (proved[premise] == no_step) {
                    ready = false;
                    break;
                }
                dependencies.push_back(proved[premise]);
                level = std::max(level, depth[premise] + 1);
            }
            if (stopped) break;
            if (!ready) continue;
            if (level > input.limits.depth) {
                depth_blocked[rule.head] = true;
                continue;
            }
            // Breadth by derivation depth: a long earlier rule must not pin
            // a deeper memo entry and hide a later shorter alternative.
            if (level > round) continue;
            if (!event(EventKind::Derived, rule.head, id, level)) {
                stopped = true;
                break;
            }
            proved[rule.head] = static_cast<std::uint32_t>(result.proof.size());
            depth[rule.head] = level;
            result.proof.push_back({rule.head, id, std::move(dependencies)});
            changed = true;
        }
        if (proved[input.goal] != no_step && proved[input.goal ^ 1U] != no_step) break;
        ++round;
    }
    for (std::size_t literal = 0; literal < count; ++literal) {
        if (depth_blocked[literal] && proved[literal] == no_step) result.limited = true;
    }
    result.positive_root = proved[input.goal];
    result.negative_root = proved[input.goal ^ 1U];
    if (result.positive_root != no_step && result.negative_root != no_step) {
        result.status = Status::Contradiction;
    } else if (result.limited) {
        result.status = Status::Unknown;
    } else if (result.positive_root != no_step) {
        result.status = Status::Proved;
    } else if (result.negative_root != no_step) {
        result.status = Status::Disproved;
    } else {
        result.status = Status::Unknown;
    }
    // Reuse the unchanged native proof/status checker, never the producer's
    // own conclusion alone. Native wire bounds cap proof and trace storage.
    result = decode_result(encode_result(result));
    return run;
}

} // namespace axiom::knowledge
