#include "source_reasoner.hpp"
#include "virtual_drive.hpp"
#include "problem_wire.hpp"

#include <fstream>
#include <iostream>
#include <source_location>
#include <unistd.h>

namespace {
using namespace axiom;
using namespace axiom::knowledge;
std::size_t checks = 0;
void check(bool value, const std::source_location where = std::source_location::current()) {
    ++checks;
    if (!value) throw std::runtime_error("Source quantity assertion at " + std::to_string(where.line()));
}
template<class F> void rejects(ErrorCode code, F&& operation) {
    ++checks;
    try { operation(); }
    catch (const Error& error) { if (error.code == code) return; throw; }
    throw std::runtime_error("Expected source quantity rejection");
}
struct Directory {
    std::filesystem::path path;
    Directory() {
        auto pattern = (std::filesystem::temp_directory_path() / "source-quantity-XXXXXX").string();
        const auto* created = mkdtemp(pattern.data());
        if (!created) throw std::runtime_error("Cannot create quantity test directory");
        path = created;
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
Fixture fixture(const std::filesystem::path& directory, bool conflict = false,
    bool approximate = false, std::optional<std::size_t> missing = {}, bool invalid = false, int guarded = 0) {
    std::filesystem::create_directory(directory);
    Fixture result;
    const std::string qualifier = approximate ? "approximately" : "exactly";
    const std::string text = "The first speed is " + qualifier
        + " 7 m/s. The second speed equals the first speed. A conflicting report is exactly 9 m/s.";
    result.source.kind = ObjectKind::Evidence; result.source.visibility = Visibility::Public;
    result.source.label = "Synthetic scoped quantities";
    result.source.format = "source.zim.raw.v1:text/plain";
    result.source.payload.assign(text.begin(), text.end());
    const auto archive = "synthetic:" + directory.filename().string();
    result.source.sources = {{archive, "1", "A/speeds", "libzim-raw-entry-v1",
                              digest(result.source.payload), 0, text.size()}};
    result.host.unit_registry_id = "host-units"; result.host.unit_registry_version = "1";
    result.host.units = {
        {"m_per_s", "m/s", {0,1,-1,0,0,0,0}, Rational(1), {"m/s"}},
        {"km_per_h", "km/h", {0,1,-1,0,0,0,0}, Rational(5,18), {"km/h"}},
        {"kg", "kg", {1,0,0,0,0,0,0}, Rational(1), {"kg"}}};
    result.host.views = {make_evidence_view("source", PremiseBasis::Source, text,
        archive, "A/speeds", "libzim-raw-entry-v1")};
    const auto support = "[[0,0," + std::to_string(text.size()) + ",null]]";
    const auto number = [&](char value, std::string precision, std::string marker) {
        return "NUMBER [[0," + std::to_string(text.find(value)) + ",1,null],[0,"
            + std::to_string(text.find("m/s")) + ",3,null],0,\"" + precision + "\",[0,"
            + std::to_string(text.find(marker)) + "," + std::to_string(marker.size()) + ",null]]\n";
    };
    std::string dsl = "GAMMATHINK_DSL 1\nENTITY \"cart\"\n"
        "QUANTITY [\"first_speed\",0,[true,[]],0]\nQUANTITY [\"second_speed\",0,[true,[]],0]\n"
        "QUANTITY [\"unknown_speed\",0,[true,[]],0]\n"
        + number('7', approximate ? "approximate" : "exact", qualifier);
    if (conflict) dsl += number('9', "exact", "exactly");
    dsl += "BINDING [0,0," + support + "]\n";
    if (conflict) dsl += "BINDING [0,1," + support + "]\n";
    if (guarded) dsl += "PREDICATE [\"allowed\",1]\n";
    if (guarded == 1) dsl += "RULE [0,[0,false,[[\"c\",0]]],[]," + support + "]\n";
    dsl += "NODE [\"q\",0]\nNODE [\"q\",1]\nEQUATION [1,0,[true,[]],"
        + std::string(guarded ? "[0,false,[[\"c\",0]]]" : "null") + "," + support + "]\nEND\n";
    const auto candidate = *compile_source(result.source, identify(result.source), dsl).candidate;
    result.compiled = materialize_source(result.source, candidate, result.host);
    Journal library(directory / "library.db");
    library.put(result.source); library.put(result.compiled.context); library.put(result.compiled.module);
    for (std::size_t i = 0; i < result.compiled.members.size(); ++i)
        if (missing != i) library.put(result.compiled.members[i]);
    if (invalid) {
        Assessment observed;
        observed.state = KnowledgeState::Invalidated; observed.logical = LogicalValidity::Invalid;
        observed.checker = "synthetic-publisher";
        library.record_assessment(identify(result.compiled.members.front()), observed);
    }
    result.snapshot = directory / "snapshot";
    result.root = compile_snapshot(library, result.snapshot, "quantity-library", "1").root;
    return result;
}
}

int main(int argc, char** argv) {
    try {
        if (argc == 2) {
            const auto directory = std::filesystem::path(argv[1]);
            const auto value = fixture(directory);
            std::ofstream host(directory / "host.json"); host << serialize_problem_context(value.host); host.close();
            std::ofstream goal(directory / "goal.json"); goal << "[\"q\",1,1]"; goal.close();
            if (!host || !goal) throw Error(ErrorCode::Storage, "Cannot write synthetic quantity inputs");
            std::cout << "{\"module\":\"" << hex(identify(value.compiled.module)) << "\",\"root\":\""
                      << hex(value.root) << "\"}\n";
            return 0;
        }
        if (argc != 1) throw Error(ErrorCode::Schema, "Expected no arguments or a new fixture directory");
        Directory temporary;
        const auto complete = fixture(temporary.path / "complete");
        Journal overlay(temporary.path / "overlay.db");
        GksArchive archive(complete.snapshot, complete.root);
        JournalArchive local(overlay);
        UnifiedKnowledge knowledge({&local, &archive});
        const auto module = identify(complete.compiled.module);
        const auto run = run_source_quantity(knowledge, overlay, module, complete.host, {1, 1});
        check(run.outcome.status == KnowledgeRunStatus::Checked && run.request.has_value());
        check(run.member_queries == 2 && !run.outcome.execution.reused);
        const auto reply = decode_problem_result(run.outcome.execution.reply);
        check(reply.answer == Rational(126, 5) && reply.answer_unit == 1 && reply.source_interpretations);
        check(verify_problem_result(reply.request, reply));
        check(run.outcome.execution.certificate && !run.outcome.conclusions.empty() && !run.outcome.derivations.empty());
        check(!overlay.assessment(module) && !overlay.assessment(identify(complete.source)));
        const auto replay = run_source_quantity(knowledge, overlay, module, complete.host, {1, 1});
        check(replay.outcome.status == KnowledgeRunStatus::Checked && replay.outcome.execution.reused);
        const auto direct = run_knowledge_goal(knowledge, overlay, *run.request);
        check(direct.status == KnowledgeRunStatus::Checked && direct.execution.reused);
        const auto missing_value = run_source_quantity(knowledge, overlay, module, complete.host, {2, 0});
        check(missing_value.outcome.status == KnowledgeRunStatus::Unresolved);
        check(!decode_problem_result(missing_value.outcome.execution.reply).answer);
        rejects(ErrorCode::Schema, [&] { (void)run_source_quantity(knowledge, overlay, module, complete.host, {99, 0}); });
        rejects(ErrorCode::Schema, [&] { (void)run_source_quantity(knowledge, overlay, module, complete.host, {1, 2}); });
        auto changed = complete.host; changed.unit_registry_version = "wrong";
        rejects(ErrorCode::Integrity, [&] { (void)run_source_quantity(knowledge, overlay, module, changed, {1, 0}); });
        KnowledgeRunLimits tiny; tiny.retrieval.objects = 3;
        check(run_source_quantity(knowledge, overlay, module, complete.host, {1, 0}, tiny).outcome.status
              == KnowledgeRunStatus::ResourceLimited);
        tiny = {}; tiny.retrieval.encoded_bytes = 1;
        check(run_source_quantity(knowledge, overlay, module, complete.host, {1, 0}, tiny).outcome.status
              == KnowledgeRunStatus::ResourceLimited);
        (void)overlay.invalidate(identify(complete.compiled.members[0]), "Explicit source correction");
        const auto invalidated = run_source_quantity(knowledge, overlay, module, complete.host, {1, 0});
        check(invalidated.outcome.status == KnowledgeRunStatus::RevalidationRequired && !invalidated.request);
        check(invalidated.outcome.execution.reply.empty());
        for (const auto& mode : {"missing", "invalid", "conflict", "approximate", "guarded", "blocked-guard"}) {
            const auto value = fixture(temporary.path / mode, std::string_view(mode) == "conflict",
                std::string_view(mode) == "approximate",
                std::string_view(mode) == "missing" ? std::optional<std::size_t>{1} : std::nullopt,
                std::string_view(mode) == "invalid",
                std::string_view(mode) == "guarded" ? 1 : std::string_view(mode) == "blocked-guard" ? 2 : 0);
            Journal target(temporary.path / (std::string(mode) + ".db"));
            GksArchive other(value.snapshot, value.root);
            UnifiedKnowledge selected({&other});
            const auto result = run_source_quantity(selected, target, identify(value.compiled.module), value.host, {1, 0});
            if (std::string_view(mode) == "missing") {
                check(result.outcome.status == KnowledgeRunStatus::DependenciesUnavailable && !result.request);
                check(result.outcome.unavailable == std::vector<ObjectId>{identify(value.compiled.members[1])});
            } else if (std::string_view(mode) == "invalid") {
                check(result.outcome.status == KnowledgeRunStatus::RevalidationRequired && !result.request);
            } else if (std::string_view(mode) == "conflict") {
                check(result.outcome.status == KnowledgeRunStatus::Conflict && result.outcome.execution.check.conflict);
            } else if (std::string_view(mode) == "guarded") {
                check(result.outcome.status == KnowledgeRunStatus::Checked && result.member_queries == 3);
                check(!decode_problem_result(result.outcome.execution.reply).guards.empty());
            } else {
                check(result.outcome.status == KnowledgeRunStatus::Unresolved);
                check(!decode_problem_result(result.outcome.execution.reply).answer);
            }
        }
        std::cout << "Source quantity checks passed: " << checks << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
