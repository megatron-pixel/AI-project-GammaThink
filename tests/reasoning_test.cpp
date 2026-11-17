#include "reasoning.hpp"
#include "journal.hpp"

#include "core.hpp"
#include "problem_fixture.hpp"
#include "problem_wire.hpp"
#include "relational.hpp"

#include <iostream>
#include <algorithm>
#include <stdexcept>
#include <stdlib.h>
#include <unistd.h>

namespace {
std::size_t assertions = 0;
void check(bool value) {
    ++assertions;
    if (!value) throw std::runtime_error("Reasoning certificate assertion failed");
}
template<class F> void rejects(axiom::knowledge::ErrorCode expected, F&& operation) {
    ++assertions;
    try {
        operation();
    } catch (const axiom::knowledge::Error& error) {
        if (error.code == expected) return;
        throw;
    }
    throw std::runtime_error("Expected native reuse rejection");
}

struct Directory {
    std::filesystem::path path;
    Directory() {
        auto pattern = (std::filesystem::temp_directory_path() / "gammathink-reuse-XXXXXX").string();
        std::vector<char> name(pattern.begin(), pattern.end());
        name.push_back(0);
        const auto* created = mkdtemp(name.data());
        if (!created) throw std::runtime_error("Cannot create native reuse test directory");
        path = created;
    }
    ~Directory() { std::filesystem::remove_all(path); }
};

void assessment_dependency_reuse() {
    using namespace axiom::knowledge;
    Journal journal(":memory:");
    Object source;
    source.kind = ObjectKind::Evidence;
    source.label = "Source with versioned validation prerequisites";
    source.format = "text/plain; charset=utf-8";
    const auto source_id = journal.put(source);
    Object staged;
    staged.kind = ObjectKind::TestResult;
    staged.label = "Certificate initially only in another archive";
    staged.format = "text/plain; charset=utf-8";
    const auto staged_id = identify(staged);
    Object goal;
    goal.kind = ObjectKind::Goal;
    goal.format = native_request_format;
    goal.payload = axiom::encode_request(axiom::compile({"eval", "2+3"}));
    goal.links = {{Relationship::Requires, source_id}};
    const auto goal_id = journal.put(goal);
    check(run_native(journal, goal_id).check.conclusive);
    Assessment reviewed;
    reviewed.checker = "synthetic-input-validator";
    reviewed.checked_dependencies = {staged_id};
    journal.record_assessment(source_id, reviewed);
    check(run_native(journal, goal_id).unavailable == std::vector<ObjectId>{staged_id});
    reviewed.checked_dependencies.clear();
    reviewed.certificate = staged_id;
    journal.record_assessment(source_id, reviewed);
    check(run_native(journal, goal_id).unavailable == std::vector<ObjectId>{staged_id});
    journal.put(staged);
    const auto restored = run_native(journal, goal_id);
    check(restored.reused && restored.check.conclusive);
    // Goal outcome metadata can reference its own current execution record.
    reviewed.certificate = restored.certificate;
    reviewed.checked_dependencies = {source_id};
    journal.record_assessment(goal_id, reviewed);
    check(run_native(journal, goal_id).reused);
    journal.invalidate(staged_id, "validation certificate unavailable");
    const auto blocked = run_native(journal, goal_id);
    check(!blocked.check.conclusive && blocked.reply.empty() && !blocked.unavailable.empty());
}

void persistent_reuse() {
    using namespace axiom::knowledge;
    Directory directory;
    const auto path = directory.path / "reuse.sqlite";
    ObjectId goal_id, source_id, certificate_id;
    Bytes original;
    {
        Journal journal(path);
        Object source;
        source.kind = ObjectKind::Evidence;
        source.label = "Synthetic formal declarations";
        source.format = "text/plain; charset=utf-8";
        source.payload = {'a', ',', 'b'};
        source_id = journal.put(source);
        Object goal;
        goal.kind = ObjectKind::Goal;
        goal.format = native_request_format;
        goal.payload = axiom::encode_request(axiom::compile_proof(axiom::parse_module(
            "axioms 1 persistent 1\nfact a | assumed-a\nfact b | assumed-b\n"
            "rule a & b -> c | joint\n"), "c"));
        goal.links = {{Relationship::Requires, source_id}};
        goal_id = journal.put(goal);
        const auto cold = run_native(journal, goal_id);
        check(cold.check.valid && cold.check.conclusive && !cold.reused);
        check(cold.certificate.has_value() && cold.unavailable.empty());
        certificate_id = *cold.certificate;
        original = cold.reply;
        const auto assessment = journal.assessment(certificate_id);
        check(assessment->logical == LogicalValidity::Valid);
        check(assessment->evidence == EvidenceAssessment::Unassessed);
        check(assessment->checked_dependencies.size() == 2);
        rejects(ErrorCode::Budget, [&] { run_native(journal, goal_id, {1, 64}); });
        rejects(ErrorCode::Budget, [&] { run_native(journal, goal_id, {maximum_items, 0}); });
    }
    {
        Journal journal(path);
        const auto warm = run_native(journal, goal_id);
        check(warm.reused && warm.check.conclusive && warm.certificate == certificate_id);
        check(warm.reply == original);
        check(journal.assessment_history(certificate_id).records.size() == 2);
        journal.invalidate(source_id, "synthetic source withdrawn");
        const auto blocked = run_native(journal, goal_id);
        check(!blocked.check.valid && blocked.reply.empty() && !blocked.certificate);
        check(std::find(blocked.unavailable.begin(), blocked.unavailable.end(), source_id)
              != blocked.unavailable.end());
        check(journal.get(certificate_id)->payload == original);
        check(journal.assessment(certificate_id)->logical == LogicalValidity::Valid);
        check(journal.assessment(certificate_id)->state == KnowledgeState::Invalidated);
        Assessment readmitted;
        readmitted.checker = "synthetic-source-review";
        journal.record_assessment(source_id, readmitted);
        journal.record_assessment(goal_id, readmitted);
        const auto repaired = run_native(journal, goal_id);
        check(!repaired.reused && repaired.reply == original);
        check(journal.assessment(certificate_id)->state == KnowledgeState::Verified);

        // A trusted status row cannot substitute a foreign proof for this input.
        auto forged = *journal.get(certificate_id);
        forged.payload = axiom::run_binary(axiom::encode_request(axiom::compile_proof(
            axiom::parse_module("axioms 1 foreign 1\nfact x | unrelated\n"), "x")));
        const auto forged_id = journal.put(forged);
        Assessment misleading;
        misleading.checker = "synthetic-wrong-checker";
        misleading.state = KnowledgeState::Verified;
        misleading.logical = LogicalValidity::Valid;
        journal.record_assessment(forged_id, misleading);
        journal.invalidate(certificate_id, "force independent candidate review");
        const auto rejected = run_native(journal, goal_id);
        check(!rejected.reused && rejected.reply == original);
        check(rejected.rejected_certificates == std::vector<ObjectId>{forged_id});
        check(journal.get(forged_id)->payload == forged.payload);

        Object unknown;
        unknown.kind = ObjectKind::Goal;
        unknown.format = native_request_format;
        unknown.payload = axiom::encode_request(axiom::compile_proof(
            axiom::parse_module("axioms 1 openworld 1\nrule a -> a | cycle\n"), "a"));
        const auto unknown_id = journal.put(unknown);
        const auto unresolved = run_native(journal, unknown_id);
        check(unresolved.check.valid && !unresolved.check.conclusive && !unresolved.reused);
        check(journal.assessment(*unresolved.certificate)->state == KnowledgeState::Unresolved);
        check(journal.assessment(*unresolved.certificate)->logical == LogicalValidity::Unchecked);
        check(!run_native(journal, unknown_id).reused);

        const ObjectId absent{digest(Bytes{4, 5, 6})};
        unknown.links = {{Relationship::Requires, absent}};
        const auto absent_goal = journal.put(unknown);
        const auto missing = run_native(journal, absent_goal);
        check(missing.unavailable == std::vector<ObjectId>{absent});
        check(missing.reply.empty() && !missing.certificate && !missing.check.conclusive);
        check(!journal.contains(absent));
        check(run_native(journal, absent).unavailable == std::vector<ObjectId>{absent});
    }
}
}

int main() {
    try {
        using axiom::knowledge::check_native_reply;
        const auto arithmetic = axiom::encode_request(axiom::compile({"eval", "2+3"}));
        const auto arithmetic_reply = axiom::run_binary(arithmetic);
        const auto arithmetic_check = check_native_reply(arithmetic, arithmetic_reply);
        check(arithmetic_check.valid && arithmetic_check.conclusive && !arithmetic_check.conflict);
        const auto foreign_arithmetic = axiom::encode_request(axiom::compile({"eval", "2+4"}));
        check(!check_native_reply(foreign_arithmetic, arithmetic_reply).valid);
        auto wrong_value = axiom::decode_result(arithmetic_reply);
        wrong_value.values[0] = 99;
        check(!check_native_reply(arithmetic, axiom::encode_result(wrong_value)).valid);

        const auto module = axiom::parse_module(
            "axioms 1 reuse 1\nfact a | given-a\nfact b | given-b\n"
            "rule a & b -> c | joint\n");
        const auto proof_request = axiom::compile_proof(module, "c");
        const auto proof = axiom::encode_request(proof_request);
        const auto proof_reply = axiom::run_binary(proof);
        check(check_native_reply(proof, proof_reply).conclusive);
        auto foreign_module = proof_request;
        foreign_module.module.version = "2";
        check(!check_native_reply(axiom::encode_request(foreign_module), proof_reply).valid);
        const auto foreign_goal = axiom::encode_request(axiom::compile_proof(module, "a"));
        check(!check_native_reply(foreign_goal, proof_reply).valid);
        auto removed_premise = proof_request;
        removed_premise.module.rules[0].provenance = "different-source";
        check(!check_native_reply(axiom::encode_request(removed_premise), proof_reply).valid);
        auto small_budget = proof_request;
        small_budget.limits.events = 1;
        check(!check_native_reply(axiom::encode_request(small_budget), proof_reply).valid);

        const auto conflict_request = axiom::encode_request(axiom::compile_proof(
            axiom::parse_module("axioms 1 conflict 1\nfact a | one\nfact !a | two\n"), "a"));
        const auto conflict = check_native_reply(conflict_request, axiom::run_binary(conflict_request));
        check(conflict.valid && conflict.conclusive && conflict.conflict);
        const auto unknown_request = axiom::encode_request(axiom::compile_proof(
            axiom::parse_module("axioms 1 absent 1\nrule a -> a | cyclic\n"), "a"));
        const auto unknown = check_native_reply(unknown_request, axiom::run_binary(unknown_request));
        check(unknown.valid && !unknown.conclusive && !unknown.conflict);

        const auto relational = axiom::compile_relational(
            axiom::load_relational_module("modules/relations.axioms"), "ancestor(ana,cy)");
        const auto relational_request = axiom::encode_relational_request(relational);
        const auto relational_reply = axiom::encode_relational_result(axiom::execute_relational(relational));
        check(check_native_reply(relational_request, relational_reply).conclusive);
        auto foreign_relation = relational;
        foreign_relation.module.version = "other";
        check(!check_native_reply(axiom::encode_relational_request(foreign_relation),
                                  relational_reply).valid);

        const auto problem = problem_fixture::request();
        const auto problem_request = axiom::encode_problem_request(problem);
        const auto problem_reply = axiom::run_problem_binary(problem_request);
        check(check_native_reply(problem_request, problem_reply).conclusive);
        auto foreign_problem = problem;
        foreign_problem.context.unit_registry_version = "other";
        check(!check_native_reply(axiom::encode_problem_request(foreign_problem),
                                  problem_reply).valid);
        auto missing = problem;
        missing.proposal.bindings.clear();
        const auto missing_request = axiom::encode_problem_request(missing);
        const auto missing_check = check_native_reply(missing_request,
                                                       axiom::run_problem_binary(missing_request));
        check(missing_check.valid && !missing_check.conclusive);
        check(!check_native_reply(proof, problem_reply).valid);
        check(!check_native_reply(problem_request, relational_reply).valid);
        {
            axiom::knowledge::Journal journal(":memory:");
            for (const auto& input : {arithmetic, relational_request, problem_request}) {
                axiom::knowledge::Object goal;
                goal.kind = axiom::knowledge::ObjectKind::Goal;
                goal.format = axiom::knowledge::native_request_format;
                goal.payload = input;
                const auto id = journal.put(goal);
                const auto cold = axiom::knowledge::run_native(journal, id);
                const auto warm = axiom::knowledge::run_native(journal, id);
                check(cold.check.conclusive && !cold.reused);
                check(warm.check.conclusive && warm.reused && warm.reply == cold.reply);
            }
        }
        for (std::size_t length = 0; length < arithmetic_reply.size(); ++length) {
            check(!check_native_reply(arithmetic, std::span(arithmetic_reply).first(length)).valid);
        }
        auto corrupt = arithmetic_reply;
        corrupt[0] = 0;
        check(!check_native_reply(arithmetic, corrupt).valid);
        auto oversized = arithmetic_reply;
        oversized.resize(axiom::max_wire_bytes + 1);
        check(!check_native_reply(arithmetic, oversized).valid);
        persistent_reuse();
        assessment_dependency_reuse();
        std::cout << "reasoning certificate tests passed: " << assertions
                  << " deterministic assertions\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
