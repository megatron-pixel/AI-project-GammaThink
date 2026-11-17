#include "source_reasoner.hpp"
#include "virtual_drive.hpp"

#include <iostream>
#include <fstream>
#include <algorithm>
#include <functional>
#include <unistd.h>

namespace {
using namespace axiom;
using namespace axiom::knowledge;
std::size_t assertions = 0;
void check(bool value) {
    ++assertions;
    if (!value) throw std::runtime_error("Source reasoner assertion " + std::to_string(assertions));
}
template<class Function> void rejects(ErrorCode code, Function function) {
    try { function(); }
    catch (const Error& error) { check(error.code == code); return; }
    throw std::runtime_error("Expected source reasoner rejection");
}
struct Directory {
    std::filesystem::path path;
    Directory() {
        auto pattern = (std::filesystem::temp_directory_path() / "gammathink-source-reasoner-XXXXXX").string();
        const auto* made = mkdtemp(pattern.data());
        if (!made) throw std::runtime_error("Cannot create source reasoner fixture");
        path = made;
    }
    ~Directory() { std::filesystem::remove_all(path); }
};
struct Fixture {
    Object source;
    ProblemContext host;
    CompiledSourceKnowledge compiled;
    BlockId root;
    std::filesystem::path snapshot;
};
class RevisionArchive final : public KnowledgeArchive {
public:
    RevisionArchive(GksArchive& archive, ObjectId trigger, std::function<void()> change)
        : archive_(archive), trigger_(trigger), change_(std::move(change)) {}
    std::optional<KnowledgeRecord> get(const ObjectId& id) override {
        if (id == trigger_ && change_) { auto change = std::move(change_); change(); }
        return archive_.get(id);
    }
    ObjectPage query(const KnowledgeQuery& query, std::optional<ObjectId> after, std::size_t limit) override {
        return archive_.query(query, after, limit);
    }
private:
    GksArchive& archive_;
    ObjectId trigger_;
    std::function<void()> change_;
};
struct FixtureOptions {
    bool red = true, alternative = true, negative = false;
    std::optional<std::size_t> missing;
    bool invalid = false, variable = false, cycle = false;
};
Fixture fixture(const std::filesystem::path& directory, FixtureOptions options = {}) {
    std::filesystem::create_directory(directory);
    Fixture result;
    const std::string text = "Synthetic formal source: alpha is seed. Alpha is red when declared. "
        "Seed and seed and red jointly imply goal. Seed independently implies goal when declared. "
        "Unused is separately declared. Explicit negative goal is separate when declared.";
    result.source.kind = ObjectKind::Evidence; result.source.visibility = Visibility::Public;
    result.source.label = "Synthetic scoped source";
    result.source.format = "source.zim.raw.v1:text/plain";
    result.source.payload.assign(text.begin(), text.end());
    result.source.sources = {{"synthetic-zim", "1", "A/logic", "libzim-raw-entry-v1",
        digest(result.source.payload), 0, text.size()}};
    result.host.unit_registry_id = "synthetic-host"; result.host.unit_registry_version = "1";
    result.host.views = {make_evidence_view("source", PremiseBasis::Source, text,
        "synthetic-zim", "A/logic", "libzim-raw-entry-v1")};
    const auto supports = "[[0,0," + std::to_string(text.size()) + ",null]]";
    std::string dsl = "GAMMATHINK_DSL 1\nENTITY \"alpha\"\n";
    if (options.variable) dsl += "ENTITY \"beta\"\n";
    dsl += "PREDICATE [\"seed\",1]\nPREDICATE [\"red\",1]\nPREDICATE [\"goal\",1]\nPREDICATE [\"unused\",1]\n";
    const auto atom = [&](unsigned predicate) {
        return "[" + std::to_string(predicate) + ",false,["
            + (options.variable ? "[\"v\",0]" : "[\"c\",0]") + "]]";
    };
    const auto variables = options.variable ? "1" : "0";
    dsl += "RULE [" + std::string(variables) + "," + atom(2) + ",[" + atom(0) + "," + atom(0)
        + "," + atom(1) + "]," + supports + "]\n";
    dsl += "RULE [0,[0,false,[[\"c\",0]]],"
        + std::string(options.cycle ? "[[2,false,[[\"c\",0]]]]" : "[]") + "," + supports + "]\n";
    if (options.red) dsl += "RULE [0,[1,false,[[\"c\",0]]],[]," + supports + "]\n";
    dsl += "RULE [0,[3,false,[[\"c\",0]]],[]," + supports + "]\n";
    if (options.alternative) dsl += "RULE [" + std::string(variables) + "," + atom(2) + ",[" + atom(0) + "]," + supports + "]\n";
    if (options.negative) dsl += "RULE [0,[2,true,[[\"c\",0]]],[]," + supports + "]\n";
    if (options.cycle) dsl += "RULE [0,[0,false,[[\"c\",0]]],[]," + supports + "]\n";
    dsl += "END\n";
    const auto candidate = *compile_source(result.source, identify(result.source), dsl).candidate;
    result.compiled = materialize_source(result.source, candidate, result.host);
    Journal library(directory / "library.db");
    library.put(result.source); library.put(result.compiled.context); library.put(result.compiled.module);
    for (std::size_t i = 0; i < result.compiled.members.size(); ++i)
        if (options.missing != i) library.put(result.compiled.members[i]);
    if (!options.missing || *options.missing != 1) {
        Assessment observed;
        observed.checker = "synthetic-publisher-not-local";
        observed.state = options.invalid ? KnowledgeState::Invalidated : KnowledgeState::Verified;
        observed.logical = LogicalValidity::Valid;
        library.record_assessment(identify(result.compiled.members[1]), observed);
    }
    result.snapshot = directory / "snapshot";
    result.root = compile_snapshot(library, result.snapshot, "scoped-library", "1").root;
    return result;
}
}
int main(int argc, char** argv) {
    try {
        if (argc == 2) {
            const auto value = fixture(argv[1]);
            std::ofstream host(std::filesystem::path(argv[1]) / "host.json");
            host << serialize_problem_context(value.host); host.close();
            std::ofstream goal(std::filesystem::path(argv[1]) / "goal.json");
            goal << "[\"atom\",[2,false,[[\"c\",0]]]]"; goal.close();
            if (!host || !goal) throw Error(ErrorCode::Storage, "Cannot write synthetic source query inputs");
            std::cout << "{\"module\":\"" << hex(identify(value.compiled.module))
                << "\",\"root\":\"" << hex(value.root) << "\"}\n";
            return 0;
        }
        if (argc != 1) throw Error(ErrorCode::Schema, "Expected no arguments or a new synthetic fixture directory");
        Directory directory;
        const Atom requested{2, false, {{TermKind::Constant, 0}}};
        const auto complete = fixture(directory.path / "complete");
        Journal overlay(directory.path / "overlay.db");
        JournalArchive local(overlay);
        GksArchive archive(complete.snapshot, complete.root);
        UnifiedKnowledge knowledge({&local, &archive});
        const auto run = run_source_logic(knowledge, overlay, identify(complete.compiled.module), complete.host, requested);
        check(run.outcome.status == KnowledgeRunStatus::Checked && run.request.has_value());
        check(run.rule_queries == 3 && run.work > run.rule_queries);
        check(run.outcome.execution.check.valid && run.outcome.execution.check.conclusive);
        check(!run.outcome.execution.check.conflict && !run.outcome.execution.reused);
        check(run.outcome.execution.certificate.has_value() && !run.outcome.derivations.empty());
        const auto proof = decode_relational_result(run.outcome.execution.reply);
        const auto& root = proof.proof.proof.at(proof.proof.positive_root);
        check(root.dependencies.size() == 3 && root.dependencies[0] == root.dependencies[1]);
        check(proof.request.module.name == "source_" + hex(identify(complete.compiled.module)));
        check(proof.request.module.rules.size() == complete.compiled.members.size());
        check(!overlay.contains(identify(complete.compiled.members[3])));
        check(!overlay.contains(identify(complete.compiled.members[4]))); // Unused OR branch is not prefetched.
        check(!overlay.assessment(identify(complete.compiled.members[1]))); // Publisher metadata stays observed.
        const auto executable = overlay.get(*run.request);
        check(executable->visibility == Visibility::Private && executable->sources == complete.source.sources);
        check(check_native_reply(executable->payload, run.outcome.execution.reply).valid);
        const auto certificate = overlay.assessment(*run.outcome.execution.certificate);
        check(certificate->state == KnowledgeState::Verified && certificate->logical == LogicalValidity::Valid);
        const auto reused = run_knowledge_goal(knowledge, overlay, *run.request);
        check(reused.status == KnowledgeRunStatus::Checked && reused.execution.reused);
        check(overlay.assessment(identify(complete.compiled.members[1])) == std::nullopt);
        {
            const auto alternative = fixture(directory.path / "alternative", {.red = false});
            Journal target(":memory:"); JournalArchive target_local(target);
            GksArchive stored(alternative.snapshot, alternative.root);
            UnifiedKnowledge source({&target_local, &stored});
            const auto result = run_source_logic(source, target, identify(alternative.compiled.module), alternative.host, requested);
            check(result.outcome.status == KnowledgeRunStatus::Checked);
            const auto checked = decode_relational_result(result.outcome.execution.reply);
            check(checked.proof.proof.at(checked.proof.positive_root).dependencies.size() == 1);
            check(result.rule_queries == 3);
        }
        {
            const auto missing = fixture(directory.path / "missing", {.alternative = false, .missing = 2});
            Journal target(":memory:"); JournalArchive target_local(target);
            GksArchive stored(missing.snapshot, missing.root); UnifiedKnowledge source({&target_local, &stored});
            const auto result = run_source_logic(source, target, identify(missing.compiled.module), missing.host, requested);
            check(result.outcome.status == KnowledgeRunStatus::DependenciesUnavailable);
            check(result.outcome.unavailable == std::vector<ObjectId>{identify(missing.compiled.members[2])});
            check(result.outcome.execution.check.valid && !result.outcome.execution.check.conclusive);
            check(target.assessment(*result.outcome.execution.certificate)->state == KnowledgeState::Unresolved);
        }
        {
            const auto conflict = fixture(directory.path / "conflict", {.alternative = false, .negative = true});
            Journal target(":memory:"); JournalArchive target_local(target);
            GksArchive stored(conflict.snapshot, conflict.root); UnifiedKnowledge source({&target_local, &stored});
            const auto result = run_source_logic(source, target, identify(conflict.compiled.module), conflict.host, requested);
            check(result.outcome.status == KnowledgeRunStatus::Conflict && result.outcome.execution.check.conflict);
            const auto checked = decode_relational_result(result.outcome.execution.reply);
            check(checked.proof.positive_root != no_step && checked.proof.negative_root != no_step);
        }
        {
            const auto negative = fixture(directory.path / "negative", {.red = false, .alternative = false, .negative = true});
            Journal target(":memory:"); JournalArchive target_local(target);
            GksArchive stored(negative.snapshot, negative.root); UnifiedKnowledge source({&target_local, &stored});
            const auto denied = run_source_logic(source, target, identify(negative.compiled.module), negative.host, requested);
            check(denied.outcome.status == KnowledgeRunStatus::Checked && !denied.outcome.execution.check.conflict);
            const auto denied_proof = decode_relational_result(denied.outcome.execution.reply);
            check(denied_proof.proof.status == Status::Disproved && denied_proof.proof.negative_root != no_step);
            auto signed_goal = requested; signed_goal.negative = true;
            const auto supported = run_source_logic(source, target, identify(negative.compiled.module), negative.host, signed_goal);
            check(supported.outcome.status == KnowledgeRunStatus::Checked);
            const auto supported_proof = decode_relational_result(supported.outcome.execution.reply);
            check(supported_proof.request.goal.negative && supported_proof.proof.status == Status::Proved);
        }
        {
            const auto invalid = fixture(directory.path / "invalid", {.alternative = false, .invalid = true});
            Journal target(":memory:"); JournalArchive target_local(target);
            GksArchive stored(invalid.snapshot, invalid.root); UnifiedKnowledge source({&target_local, &stored});
            const auto result = run_source_logic(source, target, identify(invalid.compiled.module), invalid.host, requested);
            check(result.outcome.status == KnowledgeRunStatus::RevalidationRequired);
            check(!result.outcome.execution.check.conclusive && !target.assessment(identify(invalid.compiled.members[1])));
        }
        {
            const auto variable = fixture(directory.path / "variable", {.variable = true});
            Journal target(":memory:"); JournalArchive target_local(target);
            GksArchive stored(variable.snapshot, variable.root); UnifiedKnowledge source({&target_local, &stored});
            const auto alpha = run_source_logic(source, target, identify(variable.compiled.module), variable.host, requested);
            check(alpha.outcome.status == KnowledgeRunStatus::Checked);
            const auto beta = run_source_logic(source, target, identify(variable.compiled.module), variable.host,
                Atom{2, false, {{TermKind::Constant, 1}}});
            check(beta.outcome.status == KnowledgeRunStatus::Unresolved && beta.outcome.unavailable.empty());
            check(beta.outcome.execution.check.valid && !beta.outcome.execution.check.conclusive);
            check(alpha.request != beta.request);
        }
        {
            const auto cyclic = fixture(directory.path / "cycle", {.cycle = true});
            Journal target(":memory:"); JournalArchive target_local(target);
            GksArchive stored(cyclic.snapshot, cyclic.root); UnifiedKnowledge source({&target_local, &stored});
            const auto result = run_source_logic(source, target, identify(cyclic.compiled.module), cyclic.host, requested);
            check(result.outcome.status == KnowledgeRunStatus::Checked);
            const auto checked = decode_relational_result(result.outcome.execution.reply);
            check(std::any_of(checked.proof.history.begin(), checked.proof.history.end(),
                [](const SearchEvent& event) { return event.kind == EventKind::Cycle; }));
        }
        {
            const auto separate = fixture(directory.path / "separate-scope", {.red = false, .alternative = false});
            Journal target(":memory:"); JournalArchive target_local(target);
            GksArchive other(separate.snapshot, separate.root);
            UnifiedKnowledge source({&target_local, &archive, &other});
            const auto scoped = run_source_logic(source, target, identify(separate.compiled.module), separate.host, requested);
            check(scoped.outcome.status == KnowledgeRunStatus::Unresolved && scoped.outcome.unavailable.empty());
            const auto original = run_source_logic(source, target, identify(complete.compiled.module), complete.host, requested);
            check(original.outcome.status == KnowledgeRunStatus::Checked);
            check(scoped.request != original.request);
        }
        {
            const auto path = directory.path / "concurrent.db";
            Journal target(path), writer(path);
            JournalArchive target_local(target);
            RevisionArchive revisions(archive, identify(complete.compiled.members[0]), [&] {
                Assessment changed;
                changed.state = KnowledgeState::Invalidated;
                changed.checker = "synthetic-concurrent-correction";
                writer.record_assessment(identify(complete.compiled.module), changed);
            });
            UnifiedKnowledge source({&target_local, &revisions});
            const auto result = run_source_logic(source, target, identify(complete.compiled.module), complete.host, requested);
            check(result.outcome.status == KnowledgeRunStatus::DependenciesUnavailable);
            check(result.outcome.execution.certificate.has_value());
            check(!target.assessment(*result.outcome.execution.certificate));
            check(target.assessment(identify(complete.compiled.module))->state == KnowledgeState::Invalidated);
            const auto rechecked = run_source_logic(source, target, identify(complete.compiled.module), complete.host, requested);
            check(rechecked.outcome.status == KnowledgeRunStatus::RevalidationRequired && rechecked.rule_queries == 0);
        }
        {
            Journal target(":memory:"); JournalArchive target_local(target);
            UnifiedKnowledge source({&target_local, &archive});
            SourceLogicLimits limits; limits.rule_queries = 1;
            const auto limited = run_source_logic(source, target, identify(complete.compiled.module), complete.host, requested, limits);
            check(limited.outcome.status == KnowledgeRunStatus::ResourceLimited && limited.rule_queries == 1);
            check(!limited.outcome.execution.certificate);
            limits = {}; limits.search.events = 2;
            const auto event_limited = run_source_logic(source, target, identify(complete.compiled.module), complete.host, requested, limits);
            check(event_limited.outcome.status == KnowledgeRunStatus::ResourceLimited);
            check(event_limited.outcome.execution.check.valid && !event_limited.outcome.execution.check.conclusive);
            limits = {}; limits.work = 1;
            check(run_source_logic(source, target, identify(complete.compiled.module), complete.host, requested, limits)
                .outcome.status == KnowledgeRunStatus::ResourceLimited);
            auto host = complete.host; host.unit_registry_version = "other";
            rejects(ErrorCode::Integrity, [&] {
                run_source_logic(source, target, identify(complete.compiled.module), host, requested);
            });
            rejects(ErrorCode::Schema, [&] {
                run_source_logic(source, target, identify(complete.compiled.module), complete.host,
                    Atom{99, false, {{TermKind::Constant, 0}}});
            });
        }
        std::cout << "Source reasoning passed: " << assertions << " deterministic assertions\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
