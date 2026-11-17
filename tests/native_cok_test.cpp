#include "native_cok.hpp"
#include "binary_dispatch.hpp"
#include "core.hpp"
#include "problem_fixture.hpp"
#include "problem_wire.hpp"
#include "relational.hpp"

#include <iostream>
#include <map>
#include <stdexcept>

namespace {
using namespace axiom;
using namespace axiom::knowledge;
std::size_t assertions = 0;
void check(bool value) {
    ++assertions;
    if (!value) throw std::runtime_error("Native CoK assertion " + std::to_string(assertions));
}
template<class F> void rejects(ErrorCode code, F&& operation) {
    ++assertions;
    try { operation(); } catch (const Error& error) {
        if (error.code == code) return;
        throw;
    }
    throw std::runtime_error("Expected native CoK rejection");
}
Object goal(const axiom::Bytes& input) {
    Object object;
    object.kind = ObjectKind::Goal;
    object.format = native_request_format;
    object.payload = input;
    return object;
}
Object record(const Object& input) {
    return native_execution_record(input, identify(input), run_request_binary(input.payload));
}
void validate(const Object& input, const Object& certificate, const NativeCoK& graph) {
    Journal journal(":memory:");
    journal.put(input);
    const auto certificate_id = journal.put(certificate);
    for (const auto& object : graph.objects) journal.put(object);
    for (const auto& object : graph.objects) {
        check(object.visibility == input.visibility && object.sources == input.sources);
        const auto reference = decode_native_reference(object.payload);
        check(reference.certificate == certificate_id);
        check(!journal.assessment(identify(object)));
        if (object.derivation) {
            const auto& derivation = *object.derivation;
            check(journal.contains(derivation.conclusion) && journal.contains(derivation.rule));
            for (const auto& premise : derivation.premises) check(journal.contains(premise));
            check(!journal.related(derivation.conclusion, Relationship::Produces).objects.empty());
        }
    }
}
}

int main() {
    try {
        const auto input = goal(encode_request(compile_proof(parse_module(
            "axioms 1 cok 1\nfact a | assumed-a\nfact b | assumed-b\n"
            "rule a & b -> c | joint\n"), "c")));
        const auto certificate = record(input);
        const auto before = encode_object(certificate);
        const auto graph = materialize_native(input, certificate);
        check(graph.derivations.size() == 3 && graph.conclusions.size() == 1);
        std::map<ObjectId, Object> objects;
        for (const auto& object : graph.objects) objects.emplace(identify(object), object);
        const auto& last = objects.at(graph.derivations.back()).derivation.value();
        check(last.premises.size() == 2 && last.assumptions.size() == 1);
        check(last.conclusion == graph.conclusions[0]);
        check(decode_native_reference(objects.at(last.rule).payload).kind == NativeReferenceKind::HornRule);
        check(encode_object(certificate) == before);
        validate(input, certificate, graph);
        const auto conflicting = goal(encode_request(compile_proof(parse_module(
            "axioms 1 inconsistent 1\nfact a | first\nfact !a | second\n"), "a")));
        const auto conflicting_record = record(conflicting);
        const auto both = materialize_native(conflicting, conflicting_record);
        check(both.derivations.size() == 2 && both.conclusions.size() == 1);
        bool found_contradiction = false;
        for (const auto& object : both.objects) {
            if (object.kind == ObjectKind::Contradiction) found_contradiction = true;
        }
        check(found_contradiction);
        validate(conflicting, conflicting_record, both);
        const auto arithmetic = goal(encode_request(compile({"eval", "2+3"})));
        const auto arithmetic_record = record(arithmetic);
        const auto calculated = materialize_native(arithmetic, arithmetic_record);
        check(calculated.derivations.size() == 1 && calculated.conclusions.size() == 1);
        validate(arithmetic, arithmetic_record, calculated);
        const auto relational = goal(encode_relational_request(compile_relational(
            load_relational_module("modules/relations.axioms"), "ancestor(ana,cy)")));
        const auto relational_record = record(relational);
        const auto grounded = materialize_native(relational, relational_record);
        check(!grounded.derivations.empty() && grounded.conclusions.size() == 1);
        validate(relational, relational_record, grounded);
        auto problem = problem_fixture::request();
        const auto sourced = goal(encode_problem_request(problem));
        const auto sourced_record = record(sourced);
        const auto quantities = materialize_native(sourced, sourced_record);
        check(quantities.conclusions.size() == 1 && quantities.derivations.size() > 4);
        std::map<ObjectId, Object> quantity_objects;
        for (const auto& object : quantities.objects) quantity_objects.emplace(identify(object), object);
        const auto& final = quantity_objects.at(quantities.derivations.back()).derivation.value();
        check(final.premises.size() == 2 && final.assumptions.size() == 1);
        const auto output_unit = decode_native_reference(quantity_objects.at(final.premises[1]).payload);
        check(output_unit.kind == NativeReferenceKind::Unit && output_unit.index == 0);
        bool guarded_equation = false, source_evidence = false;
        for (const auto& object : quantities.objects) {
            if (object.kind == ObjectKind::Evidence) source_evidence = true;
            if (object.derivation && decode_native_reference(object.payload).kind ==
                NativeReferenceKind::QuantityStep && object.derivation->premises.size() > 2) {
                guarded_equation = true;
            }
        }
        check(guarded_equation && source_evidence);
        validate(sourced, sourced_record, quantities);

        problem.context.views.push_back(make_evidence_view("conflicting_tare", PremiseBasis::Source,
            "The tare mass is exactly 0.5 kg.", "synthetic", "other_tare", "synthetic-v1"));
        const auto number = static_cast<std::uint32_t>(problem.proposal.numbers.size());
        problem.proposal.numbers.push_back({problem_fixture::span(problem, 3, "0.5"),
            problem_fixture::span(problem, 3, "kg"), 0, QuantityPrecision::Exact,
            problem_fixture::span(problem, 3, "exactly")});
        problem.proposal.bindings.push_back({2, number, problem_fixture::origin(problem, 3, "other-tare")});
        const auto disputed = goal(encode_problem_request(problem));
        const auto disputed_record = record(disputed);
        check(decode_problem_result(disputed_record.payload).status == ProblemStatus::Conflict);
        const auto conflict = materialize_native(disputed, disputed_record);
        check(conflict.conclusions.size() == 1);
        validate(disputed, disputed_record, conflict);
        problem = problem_fixture::request();
        problem.proposal.bindings.clear();
        const auto missing = goal(encode_problem_request(problem));
        const auto missing_record = record(missing);
        check(materialize_native(missing, missing_record).conclusions.empty());
        rejects(ErrorCode::Budget, [&] { materialize_native(input, certificate, {1, 1024}); });
        rejects(ErrorCode::Budget, [&] { materialize_native(input, certificate, {64, 1}); });
        rejects(ErrorCode::Integrity, [&] { materialize_native(input, arithmetic_record); });
        auto forged = certificate;
        forged.sources.push_back({"other", "1", "path", "v1", {}, 0, 0});
        rejects(ErrorCode::Integrity, [&] { materialize_native(input, forged); });
        const auto sample = graph.objects.front().payload;
        for (std::size_t length = 0; length < sample.size(); ++length) {
            rejects(ErrorCode::Schema, [&] {
                decode_native_reference(std::span(sample).first(length));
            });
        }
        auto bad = sample;
        bad[5] = 255;
        rejects(ErrorCode::Schema, [&] { decode_native_reference(bad); });
        std::cout << "native CoK tests passed: " << assertions << " deterministic assertions\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
