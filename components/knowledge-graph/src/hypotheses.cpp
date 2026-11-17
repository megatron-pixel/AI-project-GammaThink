#include "hypotheses.hpp"
#include "forward_reasoning.hpp"

#include <algorithm>
#include <map>
#include <set>

namespace axiom::knowledge {
namespace {
struct Obligation {
    std::uint32_t literal;
    std::vector<std::uint32_t> ancestors;
};
struct Branch {
    std::vector<Obligation> pending;
    std::vector<std::uint32_t> assumptions, rules;
    bool contradicts_known = false;
};
}

HypothesisReport propose_hypotheses(const Object& goal, HypothesisLimits limits) {
    if (goal.kind != ObjectKind::Goal || goal.format != native_request_format) {
        throw Error(ErrorCode::Schema, "Hypotheses require an admitted native formal goal");
    }
    if (!limits.plans || !limits.frontier || !limits.obligations || !limits.assumptions ||
        !limits.hypotheses ||
        limits.plans > maximum_items || limits.frontier > maximum_items ||
        limits.obligations > maximum_items || limits.assumptions > maximum_items ||
        limits.hypotheses > maximum_items || limits.depth > 128 ||
        limits.plans > maximum_items / limits.assumptions ||
        limits.frontier > (maximum_items * 16) / limits.obligations) {
        throw Error(ErrorCode::Budget, "Invalid hypothesis frontier limits");
    }
    const auto input = decode_request(goal.payload);
    if (input.operation != Operation::Prove || input.gpu) {
        throw Error(ErrorCode::Schema, "Hypotheses require CPU finite-Horn input");
    }
    HypothesisReport report;
    report.request = identify(goal);
    const auto closure = reason_forward(input, limits.work);
    report.work = closure.work;
    if (closure.reply.limited) {
        report.limited = true;
        return report;
    }
    const auto count = input.module.symbols.size() * 2;
    std::vector<bool> known(count, false);
    for (const auto& step : closure.reply.proof) known[step.literal] = true;
    report.formally_supported = known[input.goal];
    report.existing_contradiction = known[input.goal] && known[input.goal ^ 1U];
    if (report.formally_supported) return report;
    const auto charge = [&] {
        if (report.work == limits.work) {
            report.limited = true;
            return false;
        }
        ++report.work;
        return true;
    };
    std::vector<std::vector<std::uint32_t>> heads(count);
    for (std::uint32_t i = 0; i < input.module.rules.size(); ++i) {
        if (!charge()) return report;
        heads[input.module.rules[i].head].push_back(i);
    }
    std::vector<Branch> frontier{{{{input.goal, {}}}, {}, {}, false}};
    std::set<std::pair<std::vector<std::uint32_t>, std::vector<std::uint32_t>>> emitted;
    std::map<std::uint32_t, ObjectId> identities;
    while (!frontier.empty()) {
        if (!charge()) break;
        auto branch = std::move(frontier.back());
        frontier.pop_back();
        if (branch.pending.empty()) {
            if (branch.assumptions.empty() ||
                !emitted.emplace(branch.assumptions, branch.rules).second) continue;
            if (report.alternatives.size() == limits.plans) {
                report.limited = true;
                break;
            }
            HypothesisPlan plan;
            std::set<std::uint32_t> additional;
            for (const auto literal : branch.assumptions) {
                if (!identities.contains(literal)) additional.insert(literal);
            }
            if (additional.size() > limits.hypotheses - identities.size()) {
                report.limited = true;
                continue;
            }
            plan.source_rules = std::move(branch.rules);
            plan.contradicts_known = branch.contradicts_known;
            for (const auto literal : branch.assumptions) {
                auto found = identities.find(literal);
                if (found == identities.end()) {
                    auto proposed = input;
                    proposed.goal = literal;
                    Object candidate;
                    candidate.kind = ObjectKind::Hypothesis;
                    candidate.visibility = goal.visibility;
                    candidate.label = "Unverified candidate premise";
                    candidate.format = native_request_format;
                    candidate.payload = encode_request(proposed);
                    candidate.sources = goal.sources;
                    candidate.links = {{Relationship::DependsOn, report.request}};
                    const auto id = identify(candidate);
                    report.hypotheses.push_back(std::move(candidate));
                    found = identities.emplace(literal, id).first;
                }
                plan.jointly_required.push_back(found->second);
            }
            report.alternatives.push_back(std::move(plan));
            continue;
        }
        auto obligation = std::move(branch.pending.front());
        branch.pending.erase(branch.pending.begin());
        if (known[obligation.literal]) {
            frontier.push_back(std::move(branch));
            continue;
        }
        if (obligation.ancestors.size() > limits.depth) {
            report.limited = true;
            continue;
        }
        if (std::find(obligation.ancestors.begin(), obligation.ancestors.end(),
                      obligation.literal) != obligation.ancestors.end()) continue;
        const auto& rules = heads[obligation.literal];
        if (rules.empty()) {
            // Assuming the requested conclusion itself would be circular,
            // not a discovered prerequisite or an explanation.
            if (obligation.literal == input.goal || branch.rules.empty()) continue;
            if (branch.assumptions.size() == limits.assumptions) {
                report.limited = true;
                continue;
            }
            branch.assumptions.push_back(obligation.literal);
            branch.contradicts_known = branch.contradicts_known || known[obligation.literal ^ 1U] ||
                std::find(branch.assumptions.begin(), branch.assumptions.end(),
                          obligation.literal ^ 1U) != branch.assumptions.end();
            frontier.push_back(std::move(branch));
            continue;
        }
        obligation.ancestors.push_back(obligation.literal);
        if (branch.rules.size() == limits.obligations) {
            report.limited = true;
            continue;
        }
        // Reverse stack insertion visits alternatives in their source order.
        for (auto rule = rules.rbegin(); rule != rules.rend(); ++rule) {
            if (!charge()) break;
            const auto& premises = input.module.rules[*rule].premises;
            if (premises.size() + branch.pending.size() > limits.obligations ||
                frontier.size() == limits.frontier) {
                report.limited = true;
                continue;
            }
            auto next = branch;
            next.rules.push_back(*rule);
            std::vector<Obligation> pending;
            for (const auto premise : premises) {
                pending.push_back({premise, obligation.ancestors});
            }
            pending.insert(pending.end(), next.pending.begin(), next.pending.end());
            next.pending = std::move(pending);
            frontier.push_back(std::move(next));
        }
    }
    return report;
}

} // namespace axiom::knowledge
