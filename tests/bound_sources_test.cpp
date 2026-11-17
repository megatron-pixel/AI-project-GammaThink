#include "bound_sources.hpp"
#include "virtual_drive.hpp"
#include "problem_wire.hpp"

#include <algorithm>
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
    if (!value) throw std::runtime_error("Bound sources assertion at " + std::to_string(where.line()));
}
template<class F> void rejects(ErrorCode code, F&& operation) {
    ++checks;
    try { operation(); }
    catch (const Error& error) { if (error.code == code) return; throw; }
    throw std::runtime_error("Expected bound source rejection");
}
struct Directory {
    std::filesystem::path path;
    Directory() {
        auto pattern = (std::filesystem::temp_directory_path() / "bound-sources-XXXXXX").string();
        const auto* created = mkdtemp(pattern.data());
        if (!created) throw std::runtime_error("Cannot create bound source test directory");
        path = created;
    }
    ~Directory() { std::filesystem::remove_all(path); }
};
struct Built {
    SourceBindingScope scope;
    std::vector<Object> members;
};
Built compile(std::string archive, std::string text, std::string body, bool quantity = false) {
    Object source;
    source.kind = ObjectKind::Evidence; source.visibility = Visibility::Public;
    source.label = "Synthetic complete source"; source.format = "source.zim.raw.v1:text/plain";
    source.payload.assign(text.begin(), text.end());
    source.sources = {{archive, "1", "A/source", "libzim-raw-entry-v1", digest(source.payload), 0, text.size()}};
    ProblemContext host;
    host.unit_registry_id = "shared-host-registry"; host.unit_registry_version = "1";
    if (quantity) host.units = {
        {"metre", "m", {0,1,0,0,0,0,0}, Rational(1), {"m"}},
        {"centimetre", "cm", {0,1,0,0,0,0,0}, Rational(1,100), {"cm"}}};
    host.views = {make_evidence_view("source", PremiseBasis::Source, text,
                                   archive, "A/source", "libzim-raw-entry-v1")};
    const auto support = "[[0,0," + std::to_string(text.size()) + ",null]]";
    for (std::size_t at = 0; (at = body.find("SUPPORT", at)) != std::string::npos; at += support.size())
        body.replace(at, 7, support);
    const auto candidate = *compile_source(source, identify(source), "GAMMATHINK_DSL 1\n" + body + "END\n").candidate;
    const auto compiled = materialize_source(source, candidate, host);
    return {{source, compiled.module, compiled.context, host}, compiled.members};
}
struct Fixture {
    std::vector<SourceBindingScope> scopes;
    Object assumption;
    BoundSourceGoal goal;
    std::map<ObjectId, ProblemContext> hosts;
    std::filesystem::path snapshot;
    BlockId root;
};
Fixture fixture(const std::filesystem::path& path, bool quantity = false, int mapping = 2,
    bool domain = false, bool conflict = false, bool missing = false, bool different_registry = false) {
    std::filesystem::create_directory(path);
    Built left, right;
    if (quantity) {
        const std::string text = "Cart distance is exactly 7 m.";
        left = compile("archive:left", text,
            "ENTITY \"cart\"\nQUANTITY [\"distance\",0,[true,[]],0]\nNUMBER [[0,"
            + std::to_string(text.find('7')) + ",1,null],[0," + std::to_string(text.find(" m") + 1)
            + ",1,null],0,\"exact\",[0," + std::to_string(text.find("exactly"))
            + ",7,null]]\nBINDING [0,0,SUPPORT]\n", true);
        const std::string right_text = conflict
            ? "Cart distance is exactly 9 m. Cart result equals distance plus distance."
            : "Cart result equals distance plus distance.";
        std::string right_body = "ENTITY \"cart\"\nQUANTITY [\"distance\",0,[true,[]],0]\n"
                                 "QUANTITY [\"result\",0,[true,[]],0]\n";
        if (conflict) right_body += "NUMBER [[0," + std::to_string(right_text.find('9'))
            + ",1,null],[0," + std::to_string(right_text.find(" m") + 1)
            + ",1,null],0,\"exact\",[0," + std::to_string(right_text.find("exactly"))
            + ",7,null]]\nBINDING [0,0,SUPPORT]\n";
        right_body += "NODE [\"q\",0]\nNODE [\"add\",0,0]\nNODE [\"q\",1]\n"
                      "EQUATION [2,1,[true,[]],null,SUPPORT]\n";
        right = compile("archive:right", right_text, right_body, true);
    } else {
        left = compile("archive:left", "Alpha is seed. Seed and seed imply ready for this source's entities.",
            "ENTITY \"alpha\"\nPREDICATE [\"seed\",1]\nPREDICATE [\"ready\",1]\n"
            "RULE [0,[0,false,[[\"c\",0]]],[],SUPPORT]\n"
            "RULE [1,[1,false,[[\"v\",0]]],[[0,false,[[\"v\",0]]],[0,false,[[\"v\",0]]]],SUPPORT]\n");
        std::string body = "ENTITY \"alpha\"\nENTITY \"outside\"\nPREDICATE [\"ready\",1]\n"
            "PREDICATE [\"goal\",1]\nPREDICATE [\"seed\",1]\nRULE [0,[1,false,[[\"c\","
            + std::string(domain ? "1" : "0") + "]]],[[0,false,[[\"c\","
            + std::string(domain ? "1" : "0") + "]]]],SUPPORT]\n";
        if (domain) body += "RULE [0,[2,false,[[\"c\",1]]],[],SUPPORT]\n";
        if (conflict) body += "RULE [0,[1,true,[[\"c\",0]]],[],SUPPORT]\n";
        right = compile("archive:right", "Ready implies goal. Outside seed and negative goal are separate declarations.", body);
    }
    if (different_registry) {
        right.scope.host_context.unit_registry_id = "another-host-registry";
        auto candidate = right.scope.module; candidate.links.pop_back();
        const auto compiled = materialize_source(right.scope.source, candidate, right.scope.host_context);
        right.scope.module = compiled.module; right.scope.context = compiled.context; right.members = compiled.members;
    }
    Fixture result;
    result.scopes = {left.scope, right.scope};
    const auto a = identify(left.scope.module), b = identify(right.scope.module);
    std::vector<SourceIdentityBinding> bindings;
    if (mapping != 0) bindings.push_back({{a, SourceSymbolKind::Entity, 0}, {b, SourceSymbolKind::Entity, 0}});
    if (mapping != 1) bindings.push_back({
        {a, quantity ? SourceSymbolKind::Quantity : SourceSymbolKind::Predicate, quantity ? 0U : 1U},
        {b, quantity ? SourceSymbolKind::Quantity : SourceSymbolKind::Predicate, 0}});
    if (domain) bindings.push_back({{a, SourceSymbolKind::Predicate, 0}, {b, SourceSymbolKind::Predicate, 2}});
    result.assumption = propose_source_bindings(result.scopes, bindings, "Explicit synthetic identity assumptions");
    result.goal = {b, quantity ? ProblemGoal{ProblemQuantityGoal{1, 1}} :
        ProblemGoal{ProblemAtomGoal{Atom{1, false, {{TermKind::Constant, domain ? 1U : 0U}}}}}};
    result.hosts = {{a, left.scope.host_context}, {b, right.scope.host_context}};
    Journal library(path / "library.db");
    for (const auto* item : {&left, &right}) {
        library.put(item->scope.source); library.put(item->scope.context); library.put(item->scope.module);
        for (std::size_t i = 0; i < item->members.size(); ++i)
            if (!missing || item != &right || i != 0) library.put(item->members[i]);
    }
    library.put(result.assumption);
    result.snapshot = path / "snapshot";
    result.root = compile_snapshot(library, result.snapshot, "bound-sources", "1").root;
    return result;
}
}

int main(int argc, char** argv) {
    try {
        if (argc == 2 || (argc == 3 && std::string_view(argv[2]) == "quantity")) {
            const auto path = std::filesystem::path(argv[1]);
            const auto value = fixture(path, argc == 3);
            std::ofstream hosts(path / "hosts.json"); hosts << '[';
            bool first = true;
            for (const auto& [id, context] : value.hosts) {
                if (!first) hosts << ',';
                first = false;
                hosts << "[\"" << hex(id) << "\"," << serialize_problem_context(context) << ']';
            }
            hosts << ']'; hosts.close();
            std::ofstream goal(path / "goal.json");
            goal << (argc == 3 ? "[\"q\",1,1]" : "[\"atom\",[1,false,[[\"c\",0]]]]");
            goal.close();
            if (!hosts || !goal) throw Error(ErrorCode::Storage, "Cannot write synthetic bound source inputs");
            std::cout << "{\"binding\":\"" << hex(identify(value.assumption)) << "\",\"goal_module\":\""
                << hex(value.goal.module) << "\",\"root\":\"" << hex(value.root) << "\"}\n";
            return 0;
        }
        if (argc != 1) throw Error(ErrorCode::Schema, "Expected no arguments");
        Directory directory;
        for (const auto& mode : {"logic", "quantity", "quantity-conflict", "no-predicate", "same-name", "no-quantity",
                                 "finite-domain", "conflict", "missing"}) {
            const std::string_view selected = mode;
            const bool quantity = selected == "quantity" || selected == "no-quantity" || selected == "quantity-conflict";
            const auto value = fixture(directory.path / mode, quantity,
                selected == "no-predicate" || selected == "no-quantity" ? 1 : selected == "same-name" ? 0 : 2,
                selected == "finite-domain", selected == "conflict" || selected == "quantity-conflict", selected == "missing");
            Journal overlay(directory.path / (std::string(mode) + ".db"));
            JournalArchive local(overlay);
            GksArchive archive(value.snapshot, value.root);
            UnifiedKnowledge knowledge({&local, &archive});
            const auto binding = identify(value.assumption);
            rejects(ErrorCode::Permission, [&] {
                (void)run_bound_sources(knowledge, overlay, binding, value.hosts, {}, value.goal);
            });
            const auto run = run_bound_sources(knowledge, overlay, binding, value.hosts, {binding}, value.goal);
            if (selected == "missing") {
                check(run.outcome.status == KnowledgeRunStatus::DependenciesUnavailable && !run.request);
                check(run.outcome.execution.reply.empty());
                continue;
            }
            check(run.request && run.scopes == 2);
            const auto result = decode_problem_result(run.outcome.execution.reply);
            check(verify_problem_result(result.request, result));
            check(result.request.context.views.size() == 2 &&
                  result.request.context.views[0].id != result.request.context.views[1].id);
            check(std::all_of(result.request.proposal.logic.rules.begin(), result.request.proposal.logic.rules.end(),
                              [](const auto& rule) { return rule.variables == 0; }));
            if (selected == "logic" || selected == "quantity") {
                check(run.outcome.status == KnowledgeRunStatus::Checked);
                if (quantity) check(result.answer == Rational(1400) && result.answer_unit == 1);
                else {
                    check(result.logical_goal_guard.has_value());
                    const auto& proof = result.guards.at(*result.logical_goal_guard).certificate.proof;
                    check(std::any_of(proof.proof.begin(), proof.proof.end(),
                        [](const auto& step) { return step.dependencies.size() == 2 &&
                            step.dependencies[0] == step.dependencies[1]; }));
                }
                for (const auto& id : run.outcome.derivations) {
                    const auto object = overlay.get(id);
                    check(object && object->derivation &&
                        std::find(object->derivation->assumptions.begin(), object->derivation->assumptions.end(), binding)
                        != object->derivation->assumptions.end());
                }
                const auto denied_replay = run_knowledge_goal(knowledge, overlay, *run.request);
                check(denied_replay.status == KnowledgeRunStatus::RevalidationRequired &&
                      denied_replay.execution.reply.empty());
                const auto replay = run_knowledge_goal(knowledge, overlay, *run.request, {}, {binding});
                check(replay.status == KnowledgeRunStatus::Checked && replay.execution.reused);
                const auto repeated = run_bound_sources(knowledge, overlay, binding, value.hosts, {binding}, value.goal);
                check(repeated.outcome.status == KnowledgeRunStatus::Checked && repeated.outcome.execution.reused);
                KnowledgeRunLimits tiny; tiny.retrieval.objects = 2;
                check(run_bound_sources(knowledge, overlay, binding, value.hosts, {binding}, value.goal, tiny).outcome.status
                      == KnowledgeRunStatus::ResourceLimited);
                check(!overlay.assessment(binding));
                overlay.invalidate(binding, "Explicit assumption withdrawal");
                const auto withdrawn = run_bound_sources(knowledge, overlay, binding, value.hosts, {binding}, value.goal);
                check(withdrawn.outcome.status == KnowledgeRunStatus::RevalidationRequired && !withdrawn.request);
            } else if (selected == "conflict" || selected == "quantity-conflict") {
                check(run.outcome.status == KnowledgeRunStatus::Conflict && run.outcome.execution.check.conflict);
            } else {
                check(run.outcome.status == KnowledgeRunStatus::Unresolved);
                check(!result.answer);
                if (result.logical_goal_guard) {
                    const auto& proof = result.guards.at(*result.logical_goal_guard).certificate.proof;
                    check(proof.positive_root == no_step && proof.negative_root == no_step);
                }
            }
        }
        {
            const auto value = fixture(directory.path / "different-registry", false, 2, false, false, false, true);
            Journal overlay(directory.path / "registry-overlay.db");
            GksArchive archive(value.snapshot, value.root);
            UnifiedKnowledge knowledge({&archive});
            const auto binding = identify(value.assumption);
            rejects(ErrorCode::Schema, [&] {
                (void)run_bound_sources(knowledge, overlay, binding, value.hosts, {binding}, value.goal);
            });
            check(overlay.objects().objects.empty());
        }
        std::cout << "Bound source checks passed: " << checks << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
