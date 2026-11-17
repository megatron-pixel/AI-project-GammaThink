#include "task_ir.hpp"
#include "problem_wire.hpp"

#include <filesystem>
#include <iostream>
#include <source_location>
#include <stdlib.h>
#include <unistd.h>

namespace {
using namespace axiom;
using namespace axiom::knowledge;
std::size_t checks = 0;
void check(bool value, const std::source_location where = std::source_location::current()) {
    ++checks;
    if (!value) throw std::runtime_error("Task IR assertion at line " + std::to_string(where.line()));
}
template<class F> void rejects(F&& action) {
    ++checks;
    try { action(); } catch (const std::exception&) { return; }
    throw std::runtime_error("Task IR expected rejection");
}
struct TemporaryDirectory {
    std::filesystem::path path;
    TemporaryDirectory() {
        auto pattern = (std::filesystem::temp_directory_path() / "task-ir-XXXXXX").string();
        const auto created = mkdtemp(pattern.data());
        if (!created) throw std::runtime_error("Cannot create Task IR test directory");
        path = created;
    }
    ~TemporaryDirectory() { std::filesystem::remove_all(path); }
};
ProblemSourceCatalog catalog(ProblemLimits limits = {}) {
    ProblemContext context;
    context.unit_registry_id = "task-test";
    context.unit_registry_version = "1";
    context.views.push_back(make_evidence_view("question", PremiseBasis::Question,
        "Robin flies. Is Robin flying? Ask the same question twice.",
        "", "question", "literal-v1"));
    return ProblemSourceCatalog(std::move(context), limits);
}
std::string dsl(bool negative = false, bool missing = false) {
    std::string result = "GAMMATHINK_DSL 1\nENTITY \"robin\"\nPREDICATE [\"flies\",1]\n";
    if (!missing) result += "RULE [0,[0,false,[[\"c\",0]]],[],[\"s0\"]]\n";
    if (negative) result += "RULE [0,[0,true,[[\"c\",0]]],[],[\"s0\"]]\n";
    result += "GOAL {\"goal\":[\"atom\",[0,false,[[\"c\",0]]]],\"supports\":[\"s0\"]}\n";
    result += "GOAL {\"goal\":[\"atom\",[0,false,[[\"c\",0]]]],\"supports\":[\"s0\"]}\nEND\n";
    return result;
}
AdmittedTask task(const ProblemSourceCatalog& source, const std::string& proposal) {
    return std::get<AdmittedTask>(admit_task_dsl(source, source.snapshot(), proposal));
}
std::string handle(const ProblemSourceCatalog& source, std::string_view selected) {
    for (std::size_t i = 0; i < source.entries().size(); ++i) {
        const auto& span = source.entries()[i].span;
        if (source.context().views[span.view].text.substr(span.offset, span.length) == selected) {
            return "\"" + ProblemSourceCatalog::source_handle(i) + "\"";
        }
    }
    throw std::runtime_error("Missing literal in quantity fixture");
}
}

int main() {
    try {
        const auto source = catalog();
        const auto admitted = task(source, dsl());
        check(admitted.snapshot() == source.snapshot() && admitted.dsl() == dsl());
        check(admitted.request().goals.size() == 2);
        check(admitted.request().goals[0] == admitted.request().goals[1]);
        Journal empty(":memory:");
        check(empty.objects().objects.empty());
        const auto persisted = persist_task(empty, admitted);
        check(persisted.requests.size() == 2 && persisted.requests[0] == persisted.requests[1]);
        check(persisted.plan.nodes.size() == 3 && persisted.plan.nodes[0].methods[0].size() == 2);
        check(!empty.assessment(persisted.ordered_request));
        check(empty.by_kind(ObjectKind::TestResult).objects.empty());
        check(decode_problem_goals_request(empty.get(persisted.ordered_request)->payload) == admitted.request());
        const auto count_before_restore = empty.objects().objects.size();
        const auto restored = restore_task(empty, persisted.ordered_request);
        check(restored.request() == admitted.request() && restored.dsl() == admitted.dsl());
        check(restored.snapshot() == admitted.snapshot());
        check(empty.objects().objects.size() == count_before_restore);
        rejects([&] { restore_task(empty, ObjectId{digest(Bytes{7, 8, 9})}); });
        auto altered_order = *empty.get(persisted.ordered_request);
        auto altered_request = decode_problem_goals_request(altered_order.payload);
        altered_request.goals.pop_back();
        altered_order.payload = encode_problem_goals_request(altered_request);
        const auto altered_id = empty.put(altered_order);
        const auto count_after_altered = empty.objects().objects.size();
        rejects([&] { restore_task(empty, altered_id); });
        check(empty.objects().objects.size() == count_after_altered);
        const auto executed = execute_task(empty, admitted);
        check(executed.evaluation.root_satisfied && executed.slots.size() == 2);
        check(!executed.slots[0].execution.reused && executed.slots[1].execution.reused);
        check(executed.slots[0].ordinal == 0 && executed.slots[1].ordinal == 1);
        check(executed.slots[0].execution.check.valid && executed.slots[0].execution.check.conclusive);
        check(!executed.slots[0].conclusions.empty() && !executed.slots[0].derivations.empty());
        const auto certificate = empty.get(*executed.slots[0].execution.certificate);
        check(certificate && certificate->kind == ObjectKind::TestResult);
        for (const auto& id : executed.slots[0].derivations) {
            const auto object = empty.get(id);
            check(object && object->derivation && !empty.assessment(id));
            check(object->sources == empty.get(executed.task.requests[0])->sources);
        }
        const auto assessment = empty.assessment(*executed.slots[0].execution.certificate);
        check(assessment && assessment->logical == LogicalValidity::Valid
            && assessment->evidence == EvidenceAssessment::Unassessed);
        empty.invalidate(persisted.context, "test source no longer reusable");
        const auto unavailable = execute_task(empty, admitted);
        check(!unavailable.evaluation.root_satisfied);
        check(!unavailable.slots[0].execution.unavailable.empty());
        check(!unavailable.slots[0].execution.certificate);
        check(unavailable.evaluation.states[1] == GoalState::UnresolvedOutcome);

        Journal conflict_store(":memory:");
        const auto conflict = execute_task(conflict_store, task(source, dsl(true)));
        check(!conflict.evaluation.root_satisfied);
        check(conflict.slots[0].execution.check.conflict);
        check(!conflict_store.by_kind(ObjectKind::Contradiction).objects.empty());
        Journal missing_store(":memory:");
        const auto unresolved = execute_task(missing_store, task(source, dsl(false, true)));
        check(!unresolved.evaluation.root_satisfied && !unresolved.slots[0].execution.check.conclusive);
        check(unresolved.evaluation.states[1] == GoalState::UnresolvedOutcome);
        check(unresolved.slots[0].conclusions.empty());
        const auto clarify = admit_task_dsl(source, source.snapshot(),
            "GAMMATHINK_DSL 1\nCLARIFY \"Which object does it refer to?\"\nEND\n");
        check(std::get<TaskDecline>(clarify).kind == TaskDeclineKind::Clarify);
        const auto unsupported = admit_task_dsl(source, source.snapshot(),
            "GAMMATHINK_DSL 1\nUNSUPPORTED \"Continuous probability is outside this profile.\"\nEND\n");
        check(std::get<TaskDecline>(unsupported).kind == TaskDeclineKind::Unsupported);
        rejects([&] { admit_task_dsl(source, "stale-snapshot", dsl()); });
        rejects([&] { admit_task_dsl(source, source.snapshot(), "GAMMATHINK_DSL 1"); });
        rejects([&] { admit_task_dsl(source, source.snapshot(), "GAMMATHINK_DSL 1\nEND\n"); });
        auto malformed = dsl();
        malformed.replace(malformed.find("\"s0\""), 4, "\"s999999\"");
        rejects([&] { task(source, malformed); });
        const auto tight = catalog({1, 1});
        rejects([&] { task(tight, dsl()); });
        Journal bounded(":memory:");
        rejects([&] { execute_task(bounded, admitted, {}, {1, 1024}); });

        ProblemContext context;
        context.unit_registry_id = "task-quantities";
        context.unit_registry_version = "1";
        context.units = {
            {"kilogram", "kg", {1,0,0,0,0,0,0}, 1, {"kg"}},
            {"gram", "g", {1,0,0,0,0,0,0}, Rational(1,1000), {"g"}}
        };
        context.views.push_back(make_evidence_view("question", PremiseBasis::Question,
            "In run morning, box has mass exactly 2 kg. What is its mass in g?",
            "", "question", "literal-v1"));
        const ProblemSourceCatalog quantity_source(context);
        const auto quantity_dsl = "GAMMATHINK_DSL 1\nENTITY \"box\"\n"
            "QUANTITY [\"mass\",0,[true,[[\"run\",\"morning\"]]],0]\nNUMBER ["
            + handle(quantity_source, "2") + "," + handle(quantity_source, "kg")
            + ",0,\"exact\"," + handle(quantity_source, "exactly") + "]\n"
            "BINDING [0,0,[\"s0\"]]\n"
            "GOAL {\"goal\":[\"q\",0,1],\"supports\":[\"s0\"]}\nEND\n";
        Journal quantity_store(":memory:");
        const auto quantity = execute_task(quantity_store, task(quantity_source, quantity_dsl));
        check(quantity.evaluation.root_satisfied && quantity.slots.size() == 1);
        const auto answer = decode_problem_result(quantity.slots[0].execution.reply);
        check(answer.answer == Rational(2000) && answer.answer_unit == 1);
        check(answer.request.proposal.quantities[0].scope.qualifiers[0].second == "morning");
        check(answer.request.proposal.numbers[0].precision == QuantityPrecision::Exact);
        check(!quantity.slots[0].derivations.empty());

        TemporaryDirectory directory;
        const auto path = directory.path / "journal.db";
        ObjectId saved;
        {
            Journal journal(path);
            saved = execute_task(journal, admitted).task.ordered_request;
        }
        Journal reopened(path);
        check(reopened.contains(saved));
        const auto resumed = execute_task(reopened, restore_task(reopened, saved));
        check(resumed.evaluation.root_satisfied && resumed.task.ordered_request == saved);
        check(resumed.slots[0].execution.reused && resumed.slots[1].execution.reused);
        std::cout << "task IR integration passed: " << checks << " deterministic assertions\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
