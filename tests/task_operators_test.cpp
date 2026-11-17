#include "task_operators.hpp"
#include "problem_wire.hpp"
#include "binary_dispatch.hpp"

#include <iostream>
#include <json-c/json.h>
#include <source_location>
#include <unistd.h>

namespace {
using namespace axiom;
using namespace axiom::knowledge;
using namespace axiom::neural;
using namespace axiom::runtime;
std::size_t checks = 0;
void check(bool value, const std::source_location where = std::source_location::current()) {
    ++checks;
    if (!value) throw std::runtime_error("Task operator assertion at " + std::to_string(where.line()));
}
template<class F> void rejects(ErrorCode code, F&& operation) {
    ++checks;
    try { operation(); }
    catch (const Error& error) { if (error.code == code) return; throw; }
    throw std::runtime_error("Expected native task operator rejection");
}
struct Directory {
    std::filesystem::path path;
    Directory() {
        auto pattern = (std::filesystem::temp_directory_path() / "task-operators-XXXXXX").string();
        const auto* created = mkdtemp(pattern.data());
        if (!created) throw std::runtime_error("Cannot create native task operator directory");
        path = created;
    }
    ~Directory() { std::filesystem::remove_all(path); }
};
struct MissingArchive final : KnowledgeArchive {
    ObjectId missing{Digest{7}};
    bool unavailable = false;
    std::optional<KnowledgeRecord> get(const ObjectId&) override {
        if (unavailable) throw Error(ErrorCode::Unavailable, "Synthetic offline source");
        return {};
    }
    ObjectPage query(const KnowledgeQuery&, std::optional<ObjectId> after, std::size_t) override {
        if (after && missing <= *after) return {{}, true, {}};
        return {{missing}, true, {}};
    }
};
std::string quote(std::string_view text) {
    const auto value = std::unique_ptr<json_object, decltype(&json_object_put)>(
        json_object_new_string_len(text.data(), static_cast<int>(text.size())), json_object_put);
    return json_object_to_json_string_ext(value.get(), JSON_C_TO_STRING_PLAIN);
}
TaskPlanContext context(std::map<std::string, TaskObject> objects = {},
    std::string question = "Robin flies. Check the supplied module, retrieve facts and explain the result.") {
    ProblemContext input;
    input.unit_registry_id = "host"; input.unit_registry_version = "1";
    input.views = {make_evidence_view("current", PremiseBasis::Question,
        std::move(question),
        "private", "message:1", "literal-v1")};
    return TaskPlanContext(ProblemSourceCatalog(input), 0, std::move(objects),
                           {{"network", GoalComparison::Equal, false}});
}
std::string plan_text(const TaskPlanContext& host, std::string entity, std::string kind,
    bool explain = false) {
    return "GAMMATHINK_TASK_IR 1\nCONTEXT " + quote(host.snapshot())
        + "\nROOT \"" + (explain ? "report" : "work")
        + "\"\nENTITY [\"input\"," + entity + ",[\"s0\"]]\n"
        "ENTITY [\"result\",\"artifact\",null,[\"s0\"]]\n"
        "GOAL [\"work\",[],[],[],[],[\"s0\"]]\n"
        "ACTION [\"compute\",\"work\",\"" + kind + "\",[\"input\"],[\"result\"],[\"s0\"]]\n"
        + (explain ? "ENTITY [\"reply\",\"artifact\",null,[\"s0\"]]\n"
            "GOAL [\"report\",[],[],[],[],[\"s0\"]]\n"
            "ACTION [\"describe\",\"report\",\"explain\",[\"result\"],[\"reply\"],[\"s0\"]]\n" : "")
        + "END\n";
}
AdmittedTaskPlan admit(const TaskPlanContext& host, std::string entity, std::string kind, bool explain = false) {
    return std::get<AdmittedTaskPlan>(admit_task_plan(host, host.snapshot(), plan_text(host, entity, kind, explain)));
}
std::string problem(bool unknown = false) {
    return "GAMMATHINK_DSL 1\nENTITY \"robin\"\nPREDICATE [\"flies\",1]\n"
        + std::string(unknown ? "" : "RULE [0,[0,false,[[\"c\",0]]],[],[\"s0\"]]\n")
        + "GOAL {\"goal\":[\"atom\",[0,false,[[\"c\",0]]]],\"supports\":[\"s0\"]}\nEND\n";
}
}

int main() {
    try {
        Directory directory;
        NativeModelManager model;
        const GoalConditions offline{{"network", false}};
        {
            Journal journal(directory.path / "reason.db");
            JournalArchive local(journal);
            UnifiedKnowledge knowledge({&local});
            NativeTaskOperators operators(knowledge, journal, model);
            const auto host = context();
            const auto plan = admit(host, "\"problem\"," + quote(problem()), "reason");
            TaskExecutionState state;
            rejects(ErrorCode::Permission, [&] {
                (void)operators.execute(host, plan, "compute", state, {{"network", true}});
            });
            check(journal.objects().objects.empty() && state.artifacts.empty());
            const auto result = operators.execute(host, plan, "compute", state, offline);
            check(result.status == KnowledgeRunStatus::Checked && result.outcome.verified && result.outcome.succeeded);
            check(result.native_executions == 1 && result.certificates.size() == 1);
            check(journal.get(result.receipt)->format == "gammathink.task-native-operator.v1");
            check(state.artifacts.at("result") == result.receipt && result.outcome.evidence == result.receipt);
            check(evaluate_task_plan(host, plan, state.outcomes, offline, state.artifacts).goals.root_satisfied);
            check(std::get<bool>(result.outcome.observed.at("native_valid")));
            check(std::get<bool>(result.outcome.observed.at("native_checked")));
            rejects(ErrorCode::Permission, [&] { (void)operators.execute(host, plan, "compute", state, offline); });
            TaskExecutionState fresh;
            const auto replay = operators.execute(host, plan, "compute", fresh, offline);
            check(replay.status == KnowledgeRunStatus::Checked && replay.native_executions == 0);
            auto wrong = state; wrong.capture = Digest{1};
            rejects(ErrorCode::Integrity, [&] { (void)operators.execute(host, plan, "compute", wrong, offline); });
            TaskExecutionState unbound;
            unbound.outcomes = state.outcomes;
            rejects(ErrorCode::Integrity, [&] { (void)operators.execute(host, plan, "compute", unbound, offline); });
            const auto current = context({{"extra", {result.receipt, ObjectKind::TestResult, "gammathink.task-native-operator.v1"}}});
            rejects(ErrorCode::Integrity, [&] { (void)operators.execute(current, plan, "compute", fresh, offline); });
            TaskExecutionState unresolved;
            const auto open = context({}, "Does Robin fly? Explain the result.");
            const auto unavailable = admit(open, "\"problem\"," + quote(problem(true)), "reason", true);
            const auto unknown = operators.execute(open, unavailable, "compute", unresolved, offline);
            check(unknown.status == KnowledgeRunStatus::Unresolved && unknown.outcome.verified && !unknown.outcome.succeeded);
            check(!std::get<bool>(unknown.outcome.observed.at("native_checked")));
            check(evaluate_task_plan(open, unavailable, unresolved.outcomes, offline, unresolved.artifacts).ready_actions
                  == std::vector<std::string>{"describe"});
            const auto prior_objects = journal.objects().objects.size();
            NativeTaskLimits tiny_response; tiny_response.response_bytes = 64;
            rejects(ErrorCode::Budget, [&] {
                (void)operators.execute(open, unavailable, "describe", unresolved, offline, {}, {}, tiny_response);
            });
            check(unresolved.steps.size() == 1 && !unresolved.artifacts.contains("reply") &&
                  journal.objects().objects.size() == prior_objects);
            auto forged = unresolved;
            forged.steps.at("compute").receipt = ObjectId{Digest{1}};
            rejects(ErrorCode::Integrity, [&] { (void)operators.execute(open, unavailable, "describe", forged, offline); });
            auto withdrawn = unresolved;
            const auto explained = operators.execute(open, unavailable, "describe", unresolved, offline);
            check(explained.status == KnowledgeRunStatus::Checked && explained.response.has_value());
            check(explained.native_executions == 0 && explained.certificates.empty());
            check(!std::get<bool>(explained.outcome.observed.at("native_checked")) &&
                  !std::get<bool>(explained.outcome.observed.at("empirical_truth_verified")));
            const auto receipt = journal.get(explained.receipt).value();
            check(receipt.format == "gammathink.task-native-response.v1" && !journal.assessment(explained.receipt));
            auto envelope = std::unique_ptr<json_object, decltype(&json_object_put)>(
                json_tokener_parse(explained.response->c_str()), json_object_put);
            auto* view = json_object_object_get(envelope.get(), "response");
            check(json_object_get_int(json_object_object_get(view, "source_status")) ==
                  static_cast<int>(KnowledgeRunStatus::Unresolved));
            check(!json_object_get_boolean(json_object_object_get(view, "empirical_truth_verified")));
            check(json_object_array_length(json_object_object_get(view, "native_results")) == 1);
            check(evaluate_task_plan(open, unavailable, unresolved.outcomes, offline, unresolved.artifacts).goals.root_satisfied);
            check(!unresolved.outcomes.at("work").succeeded && unresolved.outcomes.at("report").succeeded);
            journal.invalidate(unknown.receipt, "Synthetic response input withdrawal");
            rejects(ErrorCode::Integrity, [&] { (void)operators.execute(open, unavailable, "describe", withdrawn, offline); });
            check(!withdrawn.artifacts.contains("reply"));
            ModelSpec missing{"missing", directory.path / "missing.gguf", Digest{1}, ModelRole::Interpreter};
            rejects(ErrorCode::Storage, [&] { model.load(missing); });
            TaskExecutionState unloaded_required;
            rejects(ErrorCode::Permission, [&] { (void)operators.execute(host, plan, "compute", unloaded_required, offline); });
            model.unload();
        }
        {
            Journal journal(directory.path / "retrieve.db");
            Object fact; fact.kind = ObjectKind::Fact; fact.label = "First observed fixture";
            fact.format = "text/plain"; fact.payload = {'a'};
            const auto first = journal.put(fact);
            JournalArchive local(journal);
            UnifiedKnowledge knowledge({&local});
            NativeTaskOperators operators(knowledge, journal, model);
            const auto host = context();
            const auto plan = admit(host, "\"query\",[\"kind\"," + quote(name(ObjectKind::Fact)) + "]", "retrieve", true);
            TaskExecutionState state;
            const auto result = operators.execute(host, plan, "compute", state, offline);
            check(result.status == KnowledgeRunStatus::Checked && result.results == std::vector<ObjectId>{first});
            check(result.native_executions == 0 && result.certificates.empty());
            check(std::get<bool>(result.outcome.observed.at("retrieval_complete")));
            check(!journal.assessment(first));
            const auto explanation = operators.execute(host, plan, "describe", state, offline);
            auto document = std::unique_ptr<json_object, decltype(&json_object_put)>(
                json_tokener_parse(explanation.response->c_str()), json_object_put);
            auto* view = json_object_object_get(document.get(), "response");
            auto* retrieved = json_object_object_get(view, "retrieved_objects");
            check(json_object_array_length(retrieved) == 1);
            auto* item = json_object_array_get_idx(retrieved, 0);
            check(std::string_view(json_object_get_string(json_object_object_get(item, "id"))) == hex(first));
            constexpr char digits[] = "0123456789abcdef";
            std::string expected_encoding;
            for (const auto byte : encode_object(fact)) {
                expected_encoding.push_back(digits[byte >> 4]); expected_encoding.push_back(digits[byte & 15]);
            }
            check(std::string_view(json_object_get_string(json_object_object_get(item, "canonical_hex"))) == expected_encoding);
            fact.label = "Second observed fixture"; fact.payload = {'b'};
            journal.put(fact);
            TaskExecutionState limited;
            NativeTaskLimits limits; limits.query_page = 1;
            const auto page = operators.execute(host, plan, "compute", limited, offline, {}, {}, limits);
            check(page.status == KnowledgeRunStatus::ResourceLimited && page.results.size() == 1);
            check(page.outcome.verified && !page.outcome.succeeded &&
                  !std::get<bool>(page.outcome.observed.at("retrieval_complete")));
            const auto partial = operators.execute(host, plan, "describe", limited, offline);
            auto partial_document = std::unique_ptr<json_object, decltype(&json_object_put)>(
                json_tokener_parse(partial.response->c_str()), json_object_put);
            check(json_object_get_int(json_object_object_get(json_object_object_get(partial_document.get(), "response"),
                "source_status")) == static_cast<int>(KnowledgeRunStatus::ResourceLimited));
        }
        {
            for (const bool unavailable : {false, true}) {
                Journal journal(directory.path / (unavailable ? "offline.db" : "index-only.db"));
                MissingArchive archive; archive.unavailable = unavailable;
                UnifiedKnowledge knowledge({&archive});
                NativeTaskOperators operators(knowledge, journal, model);
                const auto host = context();
                const auto plan = admit(host, "\"query\",[\"kind\"," + quote(name(ObjectKind::Fact)) + "]", "retrieve", true);
                TaskExecutionState state;
                const auto result = operators.execute(host, plan, "compute", state, offline);
                check(result.status == KnowledgeRunStatus::DependenciesUnavailable && result.results.empty());
                check(result.unavailable == std::vector<ObjectId>{archive.missing});
                check(!result.outcome.succeeded && !std::get<bool>(result.outcome.observed.at("retrieval_complete")));
                const auto explanation = operators.execute(host, plan, "describe", state, offline);
                auto document = std::unique_ptr<json_object, decltype(&json_object_put)>(
                    json_tokener_parse(explanation.response->c_str()), json_object_put);
                auto* view = json_object_object_get(document.get(), "response");
                check(json_object_get_int(json_object_object_get(view, "source_status")) ==
                      static_cast<int>(KnowledgeRunStatus::DependenciesUnavailable));
                check(json_object_array_length(json_object_object_get(view, "unavailable")) == 1 &&
                      json_object_array_length(json_object_object_get(view, "retrieved_objects")) == 0);
            }
        }
        {
            Journal journal(directory.path / "source.db");
            Object source;
            const std::string text = "Robin flies.";
            source.kind = ObjectKind::Evidence; source.format = "source.zim.raw.v1:text/plain";
            source.label = "Original source"; source.payload.assign(text.begin(), text.end());
            source.sources = {{"archive", "1", "A/robin", "libzim-raw-entry-v1", digest(source.payload), 0, text.size()}};
            ProblemContext owner; owner.unit_registry_id = "source"; owner.unit_registry_version = "1";
            owner.views = {make_evidence_view("source", PremiseBasis::Source, text, "archive", "A/robin", "libzim-raw-entry-v1")};
            const auto dsl = "GAMMATHINK_DSL 1\nENTITY \"robin\"\nPREDICATE [\"flies\",1]\n"
                "RULE [0,[0,false,[[\"c\",0]]],[],[[0,0,12,null]]]\nEND\n";
            const auto candidate = *compile_source(source, identify(source), dsl).candidate;
            const auto compiled = materialize_source(source, candidate, owner);
            journal.put(source); journal.put(compiled.context); const auto module = journal.put(compiled.module);
            for (const auto& member : compiled.members) journal.put(member);
            JournalArchive local(journal);
            UnifiedKnowledge knowledge({&local});
            NativeTaskOperators operators(knowledge, journal, model);
            const auto host = context({{"module", {module, ObjectKind::Claim, "gammathink.archive-dsl.v1"}}});
            const auto plan = admit(host, "\"source-query\",[\"module\",[\"atom\",[0,false,[[\"c\",0]]]]]", "source-reason");
            TaskExecutionState state;
            rejects(ErrorCode::Missing, [&] { (void)operators.execute(host, plan, "compute", state, offline); });
            check(state.artifacts.empty());
            const auto result = operators.execute(host, plan, "compute", state, offline, {}, {{module, owner}});
            check(result.status == KnowledgeRunStatus::Checked && result.native_executions == 1);
            check(std::get<bool>(result.outcome.observed.at("native_valid")));
            check(!journal.assessment(module) && !journal.assessment(identify(source)));
        }
        {
            Journal journal(directory.path / "quantity.db");
            Object source;
            const std::string text = "Cart count is exactly 7 units.";
            source.kind = ObjectKind::Evidence; source.format = "source.zim.raw.v1:text/plain";
            source.label = "Original quantity source"; source.payload.assign(text.begin(), text.end());
            source.sources = {{"quantity", "1", "A/count", "libzim-raw-entry-v1", digest(source.payload), 0, text.size()}};
            ProblemContext owner; owner.unit_registry_id = "quantity"; owner.unit_registry_version = "1";
            owner.units = {{"units", "units", {}, Rational(1), {"units"}}};
            owner.views = {make_evidence_view("source", PremiseBasis::Source, text, "quantity", "A/count", "libzim-raw-entry-v1")};
            const auto support = "[[0,0," + std::to_string(text.size()) + ",null]]";
            const auto dsl = "GAMMATHINK_DSL 1\nENTITY \"cart\"\nQUANTITY [\"count\",0,[true,[]],0]\n"
                "NUMBER [[0," + std::to_string(text.find('7')) + ",1,null],[0,"
                + std::to_string(text.find("units")) + ",5,null],0,\"exact\",[0,"
                + std::to_string(text.find("exactly")) + ",7,null]]\nBINDING [0,0," + support + "]\nEND\n";
            const auto candidate = *compile_source(source, identify(source), dsl).candidate;
            const auto compiled = materialize_source(source, candidate, owner);
            journal.put(source); journal.put(compiled.context); const auto module = journal.put(compiled.module);
            for (const auto& member : compiled.members) journal.put(member);
            JournalArchive local(journal);
            UnifiedKnowledge knowledge({&local});
            NativeTaskOperators operators(knowledge, journal, model);
            const auto host = context({{"module", {module, ObjectKind::Claim, "gammathink.archive-dsl.v1"}}},
                                      "Compute cart count from the supplied source.");
            const auto plan = admit(host, "\"source-query\",[\"module\",[\"q\",0,0]]", "source-reason", true);
            TaskExecutionState state;
            const auto result = operators.execute(host, plan, "compute", state, offline, {}, {{module, owner}});
            check(result.status == KnowledgeRunStatus::Checked && result.certificates.size() == 1);
            check(decode_problem_result(journal.get(result.certificates[0])->payload).answer == Rational(7));
            check(!journal.assessment(module) && !journal.assessment(identify(source)));
            const auto explanation = operators.execute(host, plan, "describe", state, offline);
            auto document = std::unique_ptr<json_object, decltype(&json_object_put)>(
                json_tokener_parse(explanation.response->c_str()), json_object_put);
            auto* view = json_object_object_get(document.get(), "response");
            auto* rendered = json_object_array_get_idx(json_object_object_get(view, "native_results"), 0);
            check(std::string_view(json_object_get_string(json_object_object_get(rendered, "rendered_result"))) ==
                  render_binary_result(journal.get(result.certificates[0])->payload));
            check(json_object_array_length(json_object_object_get(rendered, "sources")) > 0);
        }
        check(model.state() == ModelState::Unloaded && model.metrics().file_bytes == 0);
        std::cout << "Native task operator checks passed: " << checks << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
