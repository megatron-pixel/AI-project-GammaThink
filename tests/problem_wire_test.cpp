#include "problem_wire.hpp"
#include "problem_fixture.hpp"

#include <algorithm>
#include <iostream>
#include <source_location>
#include <stdexcept>

namespace {
std::size_t checks = 0;
void check(bool value, const std::source_location location = std::source_location::current()) {
    ++checks;
    if (!value) throw std::runtime_error("Problem wire assertion at line " + std::to_string(location.line()));
}
template<class F> void rejects(F action) {
    bool rejected = false;
    try { action(); } catch (const std::exception&) { rejected = true; }
    check(rejected);
}
void put32(axiom::Bytes& b, std::size_t offset, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) b.at(offset + i) = static_cast<std::uint8_t>(value >> (8 * i));
}
std::uint32_t get32(const axiom::Bytes& b, std::size_t offset) {
    std::uint32_t n = 0;
    for (unsigned i = 0; i < 4; ++i) n |= static_cast<std::uint32_t>(b.at(offset + i)) << (8 * i);
    return n;
}
void skip_text(const axiom::Bytes& b, std::size_t& offset) { offset += 4 + get32(b, offset); }
void roundtrip(const axiom::ProblemRequest& q) {
    using namespace axiom;
    const auto wire = encode_problem_request(q);
    check(decode_problem_request(wire) == q);
    const auto result = execute_problem(q);
    check(verify_problem_result(q, result));
    const auto bytes = encode_problem_result(result);
    const auto decoded = decode_problem_result(bytes);
    check(verify_problem_result(q, decoded));
    check(encode_problem_result(decoded) == bytes);
    check(run_problem_binary(wire) == bytes);
    check(!render_problem(decoded).empty());
}
}

int main() {
    try {
        using namespace axiom;
        const auto q = problem_fixture::request();
        const auto request = encode_problem_request(q);
        const auto result = execute_problem(q);
        const auto bytes = encode_problem_result(result);
        roundtrip(q);
        check(result.answer == Rational(17,5));
        check(request[4] == 3 && request[5] == 0 && request[6] == 1 && bytes[6] == 2);
        check(get32(request, 8) == request.size() - 12);
        check(encode_relational_result(decode_problem_result(bytes).guards[0].certificate)
            == encode_relational_result(result.guards[0].certificate));

        // Every prefix, including complete headers with short payloads, fails.
        for (std::size_t n = 0; n < request.size(); ++n)
            rejects([&] { static_cast<void>(decode_problem_request(std::span(request).first(n))); });
        for (std::size_t n = 0; n < bytes.size(); ++n)
            rejects([&] { static_cast<void>(decode_problem_result(std::span(bytes).first(n))); });
        // Also repair the envelope length, so truncation reaches each field
        // reader instead of failing only the outer length check.
        for (std::size_t n = 12; n < request.size(); ++n) {
            Bytes prefix(request.begin(), request.begin() + static_cast<std::ptrdiff_t>(n));
            put32(prefix, 8, static_cast<std::uint32_t>(n - 12));
            rejects([&] { static_cast<void>(decode_problem_request(prefix)); });
        }
        for (std::size_t n = 12; n < bytes.size(); ++n) {
            Bytes prefix(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(n));
            put32(prefix, 8, static_cast<std::uint32_t>(n - 12));
            rejects([&] { static_cast<void>(decode_problem_result(prefix)); });
        }
        for (std::size_t offset = 0; offset < 12; ++offset) {
            auto bad = request;
            bad[offset] ^= 0x80;
            rejects([&] { static_cast<void>(decode_problem_request(bad)); });
            bad = bytes;
            bad[offset] ^= 0x80;
            rejects([&] { static_cast<void>(decode_problem_result(bad)); });
        }
        rejects([&] { static_cast<void>(decode_problem_request(bytes)); });
        rejects([&] { static_cast<void>(decode_problem_result(request)); });
        auto bad = request;
        bad.push_back(0);
        rejects([&] { static_cast<void>(decode_problem_request(bad)); });
        put32(bad, 8, static_cast<std::uint32_t>(bad.size() - 12));
        rejects([&] { static_cast<void>(decode_problem_request(bad)); });
        bad = bytes;
        bad.push_back(0);
        put32(bad, 8, static_cast<std::uint32_t>(bad.size() - 12));
        rejects([&] { static_cast<void>(decode_problem_result(bad)); });
        bad.assign(max_wire_bytes + 1, 0);
        rejects([&] { static_cast<void>(decode_problem_request(bad)); });
        rejects([&] { static_cast<void>(decode_problem_result(bad)); });
        bad = request;
        put32(bad, 12, UINT32_MAX);
        rejects([&] { static_cast<void>(decode_problem_request(bad)); });
        std::size_t unit_count = 12;
        skip_text(request, unit_count); skip_text(request, unit_count);
        bad = request;
        put32(bad, unit_count, 65);
        rejects([&] { static_cast<void>(decode_problem_request(bad)); });
        std::size_t scale = unit_count + 4;
        skip_text(request, scale); skip_text(request, scale);
        scale += 7 * 4;
        // First scale is 1/1: sign, u32 numerator length, 1,
        // u32 denominator length, 1. Reject invalid sign, unreduced 2/2,
        // zero denominator, redundant magnitude, and excessive magnitude.
        for (const auto sign : {2, 255}) {
            bad = request; bad[scale] = sign;
            rejects([&] { static_cast<void>(decode_problem_request(bad)); });
        }
        bad = request; bad[scale + 5] = 2; bad[scale + 10] = 2;
        rejects([&] { static_cast<void>(decode_problem_request(bad)); });
        bad = request; bad[scale + 10] = 0;
        rejects([&] { static_cast<void>(decode_problem_request(bad)); });
        bad = request; bad[scale] = 1; bad[scale + 5] = 0;
        rejects([&] { static_cast<void>(decode_problem_request(bad)); });
        bad = request; put32(bad, scale + 1, 513);
        rejects([&] { static_cast<void>(decode_problem_request(bad)); });
        bad = request;
        bad.insert(bad.begin() + static_cast<std::ptrdiff_t>(scale + 6), 0);
        put32(bad, scale + 1, 2);
        put32(bad, 8, static_cast<std::uint32_t>(bad.size() - 12));
        rejects([&] { static_cast<void>(decode_problem_request(bad)); });
        // The result embeds the complete original request body directly.
        const auto status = request.size();
        for (const auto offset : {status, status + 1, bytes.size() - 11,
                                  bytes.size() - 10, bytes.size() - 9}) {
            bad = bytes; bad[offset] = 255;
            rejects([&] { static_cast<void>(decode_problem_result(bad)); });
        }
        bad = bytes; put32(bad, status + 2, 257);
        rejects([&] { static_cast<void>(decode_problem_result(bad)); });
        bad = bytes; bad.back() ^= 0x80;
        rejects([&] { static_cast<void>(decode_problem_result(bad)); });

        auto changed = result;
        changed.answer = Rational(18,5);
        rejects([&] { static_cast<void>(encode_problem_result(changed)); });
        changed = result; changed.source_interpretations = false;
        rejects([&] { static_cast<void>(encode_problem_result(changed)); });
        changed = result; changed.steps[0].canonical_value += 1;
        rejects([&] { static_cast<void>(encode_problem_result(changed)); });
        changed = result;
        std::get<ProblemEquationStep>(changed.steps.back().justification).dependencies.clear();
        rejects([&] { static_cast<void>(encode_problem_result(changed)); });
        changed = result; changed.guards[0].certificate.instances.clear();
        rejects([&] { static_cast<void>(encode_problem_result(changed)); });
        const auto nested = encode_relational_result(result.guards[0].certificate);
        const auto at = std::search(bytes.begin(), bytes.end(), nested.begin(), nested.end());
        check(at != bytes.end());
        bad = bytes;
        bad[static_cast<std::size_t>(at - bytes.begin()) + 4] = 1;
        rejects([&] { static_cast<void>(decode_problem_result(bad)); });

        auto scenario = q;
        scenario.context.views[0].digest[0] ^= 1;
        rejects([&] { static_cast<void>(encode_problem_request(scenario)); });
        scenario = q; scenario.limits.work = 1;
        roundtrip(scenario);
        check(execute_problem(scenario).status == ProblemStatus::Limited);
        scenario = q; scenario.proposal.numbers[0].precision = QuantityPrecision::Approximate;
        roundtrip(scenario);
        check(execute_problem(scenario).status == ProblemStatus::Unresolved);
        scenario = q;
        scenario.proposal.goal = ProblemAtomGoal{problem_fixture::atom(2,{0,2},true)};
        roundtrip(scenario);
        check(execute_problem(scenario).guards.back().certificate.proof.status == Status::Disproved);
        scenario = q;
        scenario.proposal.goal = ProblemQuantityGoal{3,6};
        roundtrip(scenario);
        check(execute_problem(scenario).answer == Rational(3400));
        scenario = q;
        scenario.proposal.bindings.push_back({3,2,{PremiseBasis::Formal,"alternate_total",{}}});
        roundtrip(scenario);
        check(execute_problem(scenario).status == ProblemStatus::Conflict);
        // Exercise locators, optional spans, question origins, unary/number nodes,
        // and absence of a relational module or guard.
        scenario = q;
        scenario.proposal.logic = {};
        scenario.proposal.logic_origins.clear();
        scenario.proposal.equations[0].guard.reset();
        scenario.proposal.nodes.push_back(ProblemNumberNode{0});
        scenario.proposal.nodes.push_back(ProblemNegateNode{6});
        scenario.proposal.bindings[0].origin = problem_fixture::origin(scenario,2,"question_assumption");
        for (auto& view : scenario.context.views)
            view.locators.push_back({LocatorRole::Paragraph,0,
                static_cast<std::uint32_t>(view.text.size()),{},{},{},{}});
        for (auto& number : scenario.proposal.numbers) {
            number.numeral.locator = 0;
            if (number.unit_span) number.unit_span->locator = 0;
            if (number.qualifier) number.qualifier->locator = 0;
        }
        for (auto& binding : scenario.proposal.bindings)
            for (auto& span : binding.origin.supports) span.locator = 0;
        roundtrip(scenario);
        std::cout << "problem wire tests passed: " << checks << " checks\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
