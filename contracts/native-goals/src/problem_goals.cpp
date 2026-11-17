#include "problem_goals.hpp"
#include "problem_wire.hpp"

#include <sstream>
#include <stdexcept>

namespace axiom {
namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
std::string digest(const Bytes& bytes) {
    return evidence_digest(std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}
}

std::vector<ProblemRequest> admit_problem_goals(const ProblemGoalsRequest& request) {
    const auto count = request.goals.size();
    require(count > 0 && count <= max_problem_goals, "Invalid ordered goal count");
    require(request.first.proposal.goal == request.goals.front().goal, "First goal mismatch");
    require(request.first.limits.work >= count && request.first.limits.work <= 4000000
        && request.first.limits.steps >= count && request.first.limits.steps <= 256,
        "Invalid aggregate goal limits");
    std::vector<ProblemRequest> slots;
    for (std::size_t i = 0; i < count; ++i) {
        auto slot = request.first;
        slot.proposal.goal = request.goals[i].goal;
        slot.limits.work = request.first.limits.work / count
            + (i < request.first.limits.work % count);
        slot.limits.steps = static_cast<std::uint32_t>(request.first.limits.steps / count
            + (i < request.first.limits.steps % count));
        (void)admit_problem(slot);
        const auto& supports = request.goals[i].supports;
        require(!supports.empty() && supports.size() <= 16, "Invalid goal support count");
        for (const auto& span : supports) {
            require(span.view < slot.context.views.size(), "Unknown goal support view");
            const auto& view = slot.context.views[span.view];
            const auto boundary = [&](std::size_t offset) {
                return offset == view.text.size()
                    || (static_cast<unsigned char>(view.text[offset]) & 0xc0) != 0x80;
            };
            require(view.basis == PremiseBasis::Question && span.offset <= view.text.size()
                && span.length > 0 && span.length <= view.text.size() - span.offset
                && boundary(span.offset) && boundary(span.offset + span.length),
                "Goal support is not a valid question span");
            if (view.locators.empty()) require(span.locator == no_step, "Unknown goal locator");
            else {
                require(span.locator < view.locators.size(), "Missing goal locator");
                const auto& locator = view.locators[span.locator];
                require(span.offset >= locator.offset
                    && span.offset + span.length <= locator.offset + locator.length,
                    "Goal support crosses locator");
            }
        }
        slots.push_back(std::move(slot));
    }
    return slots;
}

ProblemGoalsResult execute_problem_goals(const ProblemGoalsRequest& request) {
    const auto admitted = admit_problem_goals(request);
    ProblemGoalsResult result{digest(encode_problem_goals_request(request)), {}};
    for (std::size_t i = 0; i < admitted.size(); ++i) {
        auto slot = decode_problem_result(run_problem_binary(encode_problem_request(admitted[i])));
        require(verify_problem_result(admitted[i], slot), "Ordered goal native verification failed");
        result.slots.push_back({static_cast<std::uint32_t>(i), std::move(slot)});
    }
    return result;
}

bool verify_problem_goals_result(const ProblemGoalsRequest& request,
                                const ProblemGoalsResult& result) {
    try {
        const auto admitted = admit_problem_goals(request);
        if (result.request_digest != digest(encode_problem_goals_request(request))
            || result.slots.size() != admitted.size()) return false;
        for (std::size_t i = 0; i < admitted.size(); ++i) {
            const auto& slot = result.slots[i];
            if (slot.ordinal != i || !(slot.result.request == admitted[i])
                || !verify_problem_result(admitted[i], slot.result)) return false;
        }
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

std::string render_problem_goals(const ProblemGoalsRequest& request,
                               const ProblemGoalsResult& result) {
    require(verify_problem_goals_result(request, result), "Cannot render unverified ordered goals");
    std::ostringstream output;
    for (const auto& slot : result.slots)
        output << "goal[" << slot.ordinal << "]\n" << render_problem(slot.result);
    return output.str();
}

Bytes run_problem_goals_binary(std::span<const std::uint8_t> bytes) {
    return encode_problem_goals_result(execute_problem_goals(decode_problem_goals_request(bytes)));
}
} // namespace axiom
