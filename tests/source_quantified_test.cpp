#include "source_reasoner.hpp"
#include "virtual_drive.hpp"
#include "task_plan.hpp"
#include "problem_wire.hpp"

#include <json-c/json.h>
#include <algorithm>
#include <cstdlib>
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
    if (!value) throw std::runtime_error("Quantified source assertion at " + std::to_string(where.line()));
}
template<class F> void rejects(ErrorCode code, F operation) {
    ++checks;
    try { operation(); }
    catch (const Error& error) { if (error.code == code) return; throw; }
    throw std::runtime_error("Expected quantified source rejection");
}
std::string quote(std::string_view text) {
    auto* value = json_object_new_string_len(text.data(), static_cast<int>(text.size()));
    if (!value) throw std::runtime_error("Cannot quote synthetic input");
    const std::string encoded = json_object_to_json_string_ext(value, JSON_C_TO_STRING_PLAIN);
    json_object_put(value);
    return encoded;
}
struct Directory {
    std::filesystem::path path;
    Directory() {
        auto pattern = (std::filesystem::temp_directory_path() / "quantified-source-XXXXXX").string();
        const auto* created = mkdtemp(pattern.data());
        if (!created) throw std::runtime_error("Cannot create quantified source directory");
        path = created;
    }
    ~Directory() { std::filesystem::remove_all(path); }
};
struct Fixture {
    Object source;
    ProblemContext host;
    CompiledSourceKnowledge compiled;
    BlockId root;
};
Fixture fixture(const std::filesystem::path& directory, ProblemSelectorMode mode) {
    std::filesystem::create_directory(directory);
    Fixture result;
    const std::string text =
        "For every eligible object, output equals 2 times input. A is eligible. A input is 3. B input is 7.";
    result.source.kind = ObjectKind::Evidence;
    result.source.label = "Synthetic quantified source";
    result.source.format = "source.zim.raw.v1:text/plain";
    result.source.payload.assign(text.begin(), text.end());
    result.source.sources = {{"synthetic", "1", "quantified-law", "libzim-raw-entry-v1",
        digest(result.source.payload), 0, text.size()}};
    result.host.unit_registry_id = "synthetic";
    result.host.unit_registry_version = "1";
    result.host.units = {{"one", "1", {}, 1, {"1"}}};
    result.host.views = {make_evidence_view("law", PremiseBasis::Source, text,
        "synthetic", "quantified-law", "libzim-raw-entry-v1")};
    const auto selector = [&](std::string_view selected) {
        if (mode == ProblemSelectorMode::QuoteV1)
            return "[0,null," + quote(selected) + ",null]";
        const auto offset = text.find(selected);
        if (offset == std::string::npos) throw std::runtime_error("Missing synthetic source span");
        return "[0," + std::to_string(offset) + "," + std::to_string(selected.size()) + ",null]";
    };
    const auto support = "[" + selector(text) + "]";
    std::string dsl = "GAMMATHINK_DSL 2\nENTITY \"A\"\nENTITY \"B\"\n"
        "QUANTITY [\"input\",0,[true,[]],0]\nQUANTITY [\"output\",0,[true,[]],0]\n"
        "QUANTITY [\"input\",1,[true,[]],0]\nQUANTITY [\"output\",1,[true,[]],0]\n";
    for (const auto numeral : {"2", "3", "7"})
        dsl += "NUMBER [" + selector(numeral) + ",null,0,\"exact\",null]\n";
    dsl += "BINDING [0,1," + support + "]\nBINDING [2,2," + support + "]\n"
        "PREDICATE [\"eligible\",1]\nRULE [0,[0,false,[[\"c\",0]]],[]," + support + "]\n"
        "QUANTIFIED_EQUATION [1,"
        "[[\"input\",[\"v\",0],[true,[]],0],[\"output\",[\"v\",0],[true,[]],0]],"
        "[[\"q\",1],[\"q\",0],[\"n\",0],[\"mul\",1,2]],0,3,[true,[]],"
        "[0,false,[[\"v\",0]]]," + support + "]\nEND\n";
    const auto translation = compile_source(result.source, identify(result.source), dsl);
    check(translation.kind == SourceCompilationKind::Candidate && translation.candidate.has_value());
    check(translation.candidate->format == "gammathink.archive-dsl.v2");
    result.compiled = materialize_source(result.source, *translation.candidate, result.host, {}, {}, mode);
    Journal library(directory / "library.db");
    library.put(result.source);
    library.put(result.compiled.context);
    library.put(result.compiled.module);
    for (const auto& member : result.compiled.members) library.put(member);
    result.root = compile_snapshot(library, directory / "snapshot", "quantified", "2").root;
    return result;
}
void task_admission(const Fixture& value) {
    ProblemContext question;
    question.unit_registry_id = "synthetic";
    question.unit_registry_version = "1";
    question.views = {make_evidence_view("question", PremiseBasis::Question,
        "Find A output.", "", "question", "literal-v1")};
    TaskPlanContext context(ProblemSourceCatalog(question), 0, {
        {"module", {identify(value.compiled.module), ObjectKind::Claim, value.compiled.module.format}}});
    const auto support = "[" + quote(ProblemSourceCatalog::source_handle(0)) + "]";
    const auto dsl = "GAMMATHINK_TASK_IR 1\nCONTEXT " + quote(context.snapshot())
        + "\nROOT \"root\"\nENTITY [\"query\",\"source-query\",[\"module\",[\"q\",1,0]],"
        + support + "]\nENTITY [\"output\",\"artifact\",null," + support + "]\n"
        "GOAL [\"root\",[],[],[],[]," + support + "]\n"
        "ACTION [\"solve\",\"root\",\"source-reason\",[\"query\"],[\"output\"]," + support + "]\nEND\n";
    const auto admitted = admit_task_plan(context, context.snapshot(), dsl);
    check(std::holds_alternative<AdmittedTaskPlan>(admitted));
    check(evaluate_task_plan(context, std::get<AdmittedTaskPlan>(admitted), {}, {}).ready_actions
        == std::vector<std::string>{"solve"});
}
Object oversized_candidate(const Fixture& value, std::uint32_t variables = 4) {
    std::string dsl(value.compiled.module.payload.begin(), value.compiled.module.payload.end());
    const auto quantities = dsl.find("QUANTITY ");
    if (quantities == std::string::npos) throw std::runtime_error("Missing synthetic quantities");
    dsl.insert(quantities, "ENTITY \"C\"\nENTITY \"D\"\n");
    const auto declaration = dsl.find("QUANTIFIED_EQUATION [1,");
    if (declaration == std::string::npos) throw std::runtime_error("Missing synthetic declaration");
    dsl.replace(declaration, std::string("QUANTIFIED_EQUATION [1,").size(),
        "QUANTIFIED_EQUATION [" + std::to_string(variables) + ",");
    return *compile_source(value.source, identify(value.source), dsl).candidate;
}
}

int main(int argc, char** argv) {
    try {
        if (argc == 2 || (argc == 3 && std::string_view(argv[2]) == "--oversized")) {
            const std::filesystem::path directory(argv[1]);
            const auto value = fixture(directory, ProblemSelectorMode::OffsetV1);
            auto module = identify(value.compiled.module);
            auto root = value.root;
            const std::string snapshot = argc == 3 ? "oversized-snapshot" : "snapshot";
            if (argc == 3) {
                auto candidate = oversized_candidate(value);
                candidate.links.push_back({Relationship::DependsOn, identify(value.compiled.context)});
                Journal unadmitted(directory / "unadmitted.db");
                unadmitted.put(value.source);
                unadmitted.put(value.compiled.context);
                module = unadmitted.put(candidate);
                root = compile_snapshot(unadmitted, directory / snapshot, "oversized", "1").root;
            }
            std::ofstream host(directory / "host.json");
            host << serialize_problem_context(value.host);
            host.close();
            std::ofstream goal(directory / "goal.json");
            goal << "[\"q\",1,0]";
            goal.close();
            if (!host || !goal) throw std::runtime_error("Cannot write quantified source inputs");
            std::cout << "{\"module\":\"" << hex(module)
                << "\",\"root\":\"" << hex(root) << "\",\"snapshot\":" << quote(snapshot) << "}\n";
            return 0;
        }
        if (argc != 1) throw std::runtime_error("Expected no arguments or a new fixture directory");
        Directory temporary;
        for (const auto mode : {ProblemSelectorMode::OffsetV1, ProblemSelectorMode::QuoteV1}) {
            const auto directory = temporary.path / (mode == ProblemSelectorMode::OffsetV1 ? "offset" : "quote");
            const auto value = fixture(directory, mode);
            const auto module = identify(value.compiled.module);
            check(value.compiled.members.size() == 4);
            const auto found = std::find_if(value.compiled.members.begin(), value.compiled.members.end(),
                [](const auto& member) {
                    return std::string(member.payload.begin(), member.payload.end()) == "/theory/quantified_equations/0";
                });
            check(found != value.compiled.members.end() && found->kind == ObjectKind::Rule);
            const auto record = source_member_json(value.compiled.module, value.compiled.context, *found);
            auto* declaration = json_tokener_parse(record.c_str());
            check(declaration && json_object_is_type(declaration, json_type_array)
                && json_object_array_length(declaration) == 8);
            json_object_put(declaration);
            auto downgraded = value.compiled.module;
            downgraded.format = "gammathink.archive-dsl.v1";
            rejects(ErrorCode::Integrity, [&] {
                (void)source_member_json(downgraded, value.compiled.context, *found);
            });
            auto wrong_member = *found;
            wrong_member.kind = ObjectKind::Claim;
            rejects(ErrorCode::Schema, [&] {
                (void)source_member_json(value.compiled.module, value.compiled.context, wrong_member);
            });
            Journal overlay(directory / "overlay.db");
            GksArchive archive(directory / "snapshot", value.root);
            JournalArchive local(overlay);
            UnifiedKnowledge knowledge({&local, &archive});
            const auto run = run_source_quantity(knowledge, overlay, module, value.host, {1, 0});
            check(run.outcome.status == KnowledgeRunStatus::Checked && run.request.has_value());
            check(run.member_queries == 4 && !run.outcome.execution.reused);
            const auto reply = decode_problem_result(run.outcome.execution.reply);
            check(reply.answer == Rational(6) && reply.source_interpretations);
            check(verify_problem_result(reply.request, reply));
            check(overlay.contains(identify(*found)) && !overlay.assessment(module));
            const auto replay = run_source_quantity(knowledge, overlay, module, value.host, {1, 0});
            check(replay.outcome.status == KnowledgeRunStatus::Checked && replay.outcome.execution.reused);
            const auto unresolved = run_source_quantity(knowledge, overlay, module, value.host, {3, 0});
            check(unresolved.outcome.status == KnowledgeRunStatus::Unresolved);
            const auto absent = decode_problem_result(unresolved.outcome.execution.reply);
            check(!absent.answer && verify_problem_result(absent.request, absent));
            rejects(ErrorCode::Schema, [&] {
                (void)run_source_logic(knowledge, overlay, module, value.host, {0, false, {{TermKind::Constant, 0}}});
            });
            task_admission(value);
            Assessment invalid;
            invalid.state = KnowledgeState::Invalidated;
            invalid.logical = LogicalValidity::Invalid;
            invalid.checker = "synthetic-invalidator";
            overlay.record_assessment(identify(*found), invalid);
            const auto blocked = run_source_quantity(knowledge, overlay, module, value.host, {1, 0});
            check(blocked.outcome.status == KnowledgeRunStatus::RevalidationRequired);
            check(std::find(blocked.outcome.revalidation_required.begin(),
                blocked.outcome.revalidation_required.end(), identify(*found))
                != blocked.outcome.revalidation_required.end());
            Journal incomplete(directory / "incomplete.db"), fresh(directory / "fresh.db");
            incomplete.put(value.source);
            incomplete.put(value.compiled.module);
            incomplete.put(value.compiled.context);
            for (const auto& member : value.compiled.members)
                if (identify(member) != identify(*found)) incomplete.put(member);
            JournalArchive partial(incomplete), empty(fresh);
            UnifiedKnowledge missing({&empty, &partial});
            const auto unavailable = run_source_quantity(missing, fresh, module, value.host, {1, 0});
            check(unavailable.outcome.status == KnowledgeRunStatus::DependenciesUnavailable);
            check(std::find(unavailable.outcome.unavailable.begin(), unavailable.outcome.unavailable.end(),
                identify(*found)) != unavailable.outcome.unavailable.end());
            auto oversized = oversized_candidate(value);
            rejects(ErrorCode::Budget, [&] {
                (void)materialize_source(value.source, oversized, value.host, {}, {}, mode);
            });
            const auto malformed = oversized_candidate(value, 5);
            rejects(ErrorCode::Schema, [&] {
                (void)materialize_source(value.source, malformed, value.host, {}, {}, mode);
            });
            oversized.links.push_back({Relationship::DependsOn, identify(value.compiled.context)});
            auto oversized_member = *found;
            oversized_member.concepts = oversized.concepts;
            oversized_member.links = {{Relationship::DependsOn, identify(oversized)}};
            rejects(ErrorCode::Budget, [&] {
                (void)source_member_json(oversized, value.compiled.context, oversized_member);
            });
            Journal limited(directory / "limited.db");
            limited.put(value.source);
            limited.put(value.compiled.context);
            const auto oversized_id = limited.put(oversized);
            JournalArchive bounded(limited);
            UnifiedKnowledge bounded_knowledge({&bounded});
            const auto exhausted = run_source_quantity(bounded_knowledge, limited, oversized_id, value.host, {1, 0});
            check(exhausted.outcome.status == KnowledgeRunStatus::ResourceLimited);
            check(!exhausted.request && exhausted.outcome.execution.reply.empty());
        }
        std::cout << "Quantified source persistence checks passed: " << checks << "\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << "\n";
        return 1;
    }
}
