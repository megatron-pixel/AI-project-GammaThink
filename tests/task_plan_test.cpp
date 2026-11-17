#include "task_plan.hpp"
#include "problem_semantic.hpp"

#include <json-c/json.h>
#include <sqlite3.h>

#include <algorithm>
#include <iostream>
#include <fstream>
#include <unistd.h>

namespace {
using namespace axiom;
using namespace axiom::knowledge;
std::size_t assertions = 0;
void check(bool value) {
    ++assertions;
    if (!value) throw std::runtime_error("Task plan assertion " + std::to_string(assertions));
}
template<class Function> void rejects(ErrorCode expected, Function function) {
    try { function(); }
    catch (const Error& error) { check(error.code == expected); return; }
    throw std::runtime_error("Expected Task plan rejection");
}
std::string json_quote(std::string_view value) {
    auto* encoded = json_object_new_string_len(value.data(), static_cast<int>(value.size()));
    if (!encoded) throw std::runtime_error("Cannot encode Task plan fixture");
    const std::string result = json_object_to_json_string_ext(encoded, JSON_C_TO_STRING_PLAIN);
    json_object_put(encoded); return result;
}
std::string replace(std::string value, std::string_view from, std::string_view to) {
    const auto position = value.find(from);
    if (position == std::string::npos) throw std::runtime_error("Missing Task plan mutation field");
    value.replace(position, from.size(), to); return value;
}
ObjectId identity(std::string_view value) {
    return {digest(std::span(reinterpret_cast<const std::uint8_t*>(value.data()), value.size()))};
}
struct Directory {
    std::filesystem::path path;
    Directory() {
        auto pattern = (std::filesystem::temp_directory_path() / "gammathink-task-plan-XXXXXX").string();
        const auto* made = mkdtemp(pattern.data());
        if (!made) throw std::runtime_error("Cannot create Task plan fixture");
        path = made;
    }
    ~Directory() { std::filesystem::remove_all(path); }
};
TaskPlanContext context(std::vector<GoalPredicate> active = {{"network", GoalComparison::Equal, false}}) {
    ProblemContext value;
    value.unit_registry_id = "task-plan-test"; value.unit_registry_version = "1";
    value.views = {make_evidence_view("current", PremiseBasis::Question,
        "Write C++ code, run it, and explain the result. Keep network disabled.", "", "question", "literal-v1"),
        make_evidence_view("older", PremiseBasis::Question,
        "Publish everything now.", "", "older-message", "literal-v1")};
    return TaskPlanContext(ProblemSourceCatalog(value), 0, {
        {"goal", {identity("native goal"), ObjectKind::Goal, std::string(native_request_format)}},
        {"document", {identity("raw document"), ObjectKind::Evidence, "source.zim.raw.v1:text/plain"}},
        {"module", {identity("source module"), ObjectKind::Claim, "gammathink.archive-dsl.v1"}},
        {"assumption", {identity("host assumption"), ObjectKind::Assumption, "synthetic-assumption"}}
    }, std::move(active));
}
std::string handle(const TaskPlanContext& value, std::size_t view = 0) {
    const auto found = std::find_if(value.sources().entries().begin(), value.sources().entries().end(),
        [&](const auto& source) { return source.span.view == view; });
    if (found == value.sources().entries().end()) throw std::runtime_error("Missing source handle");
    return ProblemSourceCatalog::source_handle(static_cast<std::size_t>(
        std::distance(value.sources().entries().begin(), found)));
}
std::string start(const TaskPlanContext& value) {
    return "GAMMATHINK_TASK_IR 1\nCONTEXT " + json_quote(value.snapshot()) + "\n";
}
std::string workflow(const TaskPlanContext& value) {
    const auto selected = json_quote(handle(value));
    const auto support = "[" + selected + "]";
    return start(value)
        + "ROOT \"respond\"\nENTITY [\"instruction\",\"text\"," + selected + "," + support + "]\n"
        + "ENTITY [\"code\",\"artifact\",null," + support + "]\n"
        + "ENTITY [\"runlog\",\"artifact\",null," + support + "]\n"
        + "ENTITY [\"reply\",\"artifact\",null," + support + "]\n"
        + "GOAL [\"write\",[],[],[],[]," + support + "]\n"
        + "GOAL [\"run\",[],[],[[\"exit_code\",\"eq\",0]],[]," + support + "]\n"
        + "GOAL [\"respond\",[],[],[],[]," + support + "]\n"
        + "ACTION [\"generate\",\"write\",\"generate-code\",[\"instruction\"],[\"code\"]," + support + "]\n"
        + "ACTION [\"execute\",\"run\",\"execute-code\",[\"code\"],[\"runlog\"]," + support + "]\n"
        + "ACTION [\"describe\",\"respond\",\"explain\",[\"runlog\"],[\"reply\"]," + support + "]\nEND\n";
}
std::string single(const TaskPlanContext& value, std::string entity, std::string kind) {
    const auto support = "[" + json_quote(handle(value)) + "]";
    return start(value) + "ROOT \"root\"\nENTITY [\"input\"," + entity + "," + support + "]\n"
        + "ENTITY [\"output\",\"artifact\",null," + support + "]\n"
        + "GOAL [\"root\",[],[],[],[]," + support + "]\n"
        + "ACTION [\"action\",\"root\"," + json_quote(kind) + ",[\"input\"],[\"output\"]," + support + "]\nEND\n";
}
GoalOutcome completed(GoalConditions observed = {}) { return {true, true, std::move(observed), {}}; }
void raw_sql(const std::filesystem::path& path, const std::string& sql) {
    sqlite3* raw = nullptr;
    if (sqlite3_open(path.c_str(), &raw) != SQLITE_OK) throw std::runtime_error("Cannot open Task plan corruption connection");
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> database(raw, sqlite3_close);
    if (sqlite3_exec(raw, sql.c_str(), nullptr, nullptr, nullptr) != SQLITE_OK)
        throw std::runtime_error(sqlite3_errmsg(raw));
}
}
int main(int argc, char** argv) {
    try {
        const auto host = context();
        if (argc == 2) {
            const auto directory = std::filesystem::path(argv[1]);
            if (!std::filesystem::create_directory(directory))
                throw Error(ErrorCode::Schema, "Expected a new synthetic Task IR fixture directory");
            std::filesystem::permissions(directory, std::filesystem::perms::owner_all);
            for (const auto& [name, value] : std::vector<std::pair<std::string, std::string>>{
                {"context.json", host.wire()}, {"task.dsl", workflow(host)},
                {"environment.json", "[[\"network\",false]]"},
                {"problem-context.json", serialize_problem_context(host.sources().context())},
                {"clarify.dsl", start(host) + "CLARIFY [\"Missing subject\",[" + json_quote(handle(host)) + "]]\nEND\n"}}) {
                std::ofstream file(directory / name, std::ios::binary);
                file << value; file.close();
                if (!file) throw Error(ErrorCode::Storage, "Cannot write synthetic Task IR input");
            }
            std::cout << "{\"snapshot\":" << json_quote(host.snapshot()) << "}\n"; return 0;
        }
        if (argc != 1) throw Error(ErrorCode::Schema, "Expected no arguments or a new Task IR fixture directory");
        check(restore_task_plan_context(host.wire()).snapshot() == host.snapshot());
        const auto dsl = workflow(host);
        const auto interpretation = admit_task_plan(host, host.snapshot(), dsl);
        check(std::holds_alternative<AdmittedTaskPlan>(interpretation));
        const auto& plan = std::get<AdmittedTaskPlan>(interpretation);
        check(plan.actions().size() == 3 && plan.entities().size() == 4 && plan.definitions().size() == 5);
        check(plan.artifact_goals().size() == 2);
        const auto& text = std::get<TaskText>(plan.entities().front().value);
        check(text.value == host.sources().context().views[text.span.view].text.substr(text.span.offset, text.span.length));
        const GoalConditions offline{{"network", false}};
        const auto initial = evaluate_task_plan(host, plan, {}, offline);
        check(initial.ready_actions == std::vector<std::string>{"generate"} && !initial.goals.root_satisfied);
        check(initial.missing_artifacts == std::vector<std::string>({"code", "runlog"}));
        check(evaluate_task_plan(host, plan, {}, {}).ready_actions.empty());
        check(evaluate_task_plan(host, plan, {}, {{"network", true}}).ready_actions.empty());
        auto outputs = std::map<std::string, ObjectId>{{"code", identity("generated code")}};
        auto outcomes = std::map<std::string, GoalOutcome>{{"write", completed()}};
        const auto missing = evaluate_task_plan(host, plan, outcomes, offline);
        check(missing.ready_actions.empty() && !missing.goals.root_satisfied);
        outcomes["write"].observed["__task_outputs_present"] = true;
        outcomes[plan.artifact_goals().at("code")] = completed({{"artifact_present", true}});
        check(evaluate_task_plan(host, plan, outcomes, offline).ready_actions.empty()); // Cannot forge the dataflow guard.
        const auto runnable = evaluate_task_plan(host, plan, outcomes, offline, outputs);
        check(runnable.ready_actions == std::vector<std::string>{"execute"});
        outcomes["run"] = {false, true, {{"exit_code", std::int64_t{0}}}, {}};
        outputs["runlog"] = identity("actual execution record");
        check(evaluate_task_plan(host, plan, outcomes, offline, outputs).ready_actions.empty());
        auto failed = outcomes;
        failed["run"] = {true, false, {{"exit_code", std::int64_t{1}}}, identity("verified failure receipt")};
        const auto failed_run = evaluate_task_plan(host, plan, failed, offline, outputs);
        auto successful = failed;
        successful["run"] = {true, true, {{"exit_code", std::int64_t{0}}}, failed["run"].evidence};
        const auto successful_run = evaluate_task_plan(host, plan, successful, offline, outputs);
        check(successful_run.ready_actions == std::vector<std::string>{"describe"});
        check(failed_run.ready_actions == std::vector<std::string>{"describe"});
        check(!failed_run.goals.root_satisfied);
        const auto failed_goal = std::find_if(plan.graph().nodes.begin(), plan.graph().nodes.end(),
            [](const auto& node) { return node.id == "run"; });
        check(failed_run.goals.states.at(static_cast<std::size_t>(
            std::distance(plan.graph().nodes.begin(), failed_goal))) == GoalState::Failed);
        const auto availability = std::find_if(plan.graph().nodes.begin(), plan.graph().nodes.end(),
            [&](const auto& node) { return node.id == plan.artifact_goals().at("runlog"); });
        check(availability->methods.size() == 1 && availability->definition->methods.size() == 1);
        failed["run"].verified = false;
        check(evaluate_task_plan(host, plan, failed, offline, outputs).ready_actions.empty());
        outcomes["run"] = completed({{"exit_code", std::int64_t{0}}});
        check(evaluate_task_plan(host, plan, outcomes, offline, outputs).ready_actions == std::vector<std::string>{"describe"});
        outcomes["respond"] = completed();
        check(!evaluate_task_plan(host, plan, outcomes, offline, outputs).goals.root_satisfied);
        outputs["reply"] = identity("delivered response record");
        const auto finished = evaluate_task_plan(host, plan, outcomes, offline, outputs);
        check(finished.goals.root_satisfied && finished.ready_actions.empty());
        rejects(ErrorCode::Schema, [&] {
            auto invalid = outputs; invalid["not-declared"] = identity("unknown");
            evaluate_task_plan(host, plan, outcomes, offline, invalid);
        });
        const auto changed = context({{"network", GoalComparison::Equal, true}});
        rejects(ErrorCode::Integrity, [&] { evaluate_task_plan(changed, plan, outcomes, offline, outputs); });
        rejects(ErrorCode::Integrity, [&] { admit_task_plan(changed, host.snapshot(), dsl); });
        rejects(ErrorCode::Integrity, [&] { admit_task_plan(host, host.snapshot(), replace(dsl, host.snapshot(), "wrong")); });
        {
            TaskPlanLimits limits; limits.entities = 3;
            const TaskPlanContext small(host.sources(), 0, host.objects(), host.active_constraints(), limits);
            rejects(ErrorCode::Budget, [&] { admit_task_plan(small, small.snapshot(), workflow(small)); });
            limits = {}; limits.planner.nodes = 4;
            const TaskPlanContext graph_bound(host.sources(), 0, host.objects(), host.active_constraints(), limits);
            rejects(ErrorCode::Budget, [&] { admit_task_plan(graph_bound, graph_bound.snapshot(), workflow(graph_bound)); });
        }
        check(parse_task_environment("[[\"number\",-9223372036854775808],[\"text\",\"x\"],[\"flag\",false]]").size() == 3);
        rejects(ErrorCode::Schema, [&] { parse_task_environment("[[\"number\",1],[\"number\",2]]"); });
        rejects(ErrorCode::Schema, [&] { parse_task_environment("[[\"number\",1.0]]"); });
        rejects(ErrorCode::Schema, [&] { parse_task_environment("[[\"number\",-9223372036854775809]]"); });
        const auto support = "[" + json_quote(handle(host)) + "]";
        {
            ProblemContext input;
            input.unit_registry_id = "inline-problem"; input.unit_registry_version = "1";
            input.views = {make_evidence_view("question", PremiseBasis::Question,
                "Robin flies. Is Robin flying?", "", "question", "literal-v1")};
            const TaskPlanContext owner(ProblemSourceCatalog(input), 0);
            const auto citation = "[" + json_quote(handle(owner)) + "]";
            const auto problem = "GAMMATHINK_DSL 1\nENTITY \"robin\"\nPREDICATE [\"flies\",1]\n"
                "RULE [0,[0,false,[[\"c\",0]]],[]," + citation + "]\n"
                "GOAL {\"goal\":[\"atom\",[0,false,[[\"c\",0]]]],\"supports\":" + citation + "}\nEND\n";
            const auto admitted = admit_task_plan(owner, owner.snapshot(),
                single(owner, "\"problem\"," + json_quote(problem), "reason"));
            check(std::holds_alternative<AdmittedTaskPlan>(admitted));
            const auto& task = std::get<AdmittedTaskPlan>(admitted);
            check(std::holds_alternative<AdmittedTask>(task.entities().front().value));
            check(evaluate_task_plan(owner, task, {}, {}).ready_actions == std::vector<std::string>{"action"});
        }
        {
            const auto bounded = context({{"ram_bytes", GoalComparison::Less, std::int64_t{3000000000}}});
            auto bounded_dsl = single(bounded, "\"object\",\"goal\"", "reason");
            bounded_dsl = replace(bounded_dsl, "GOAL [\"root\",[],[],[]",
                "GOAL [\"root\",[],[[\"ram_bytes\",\"lt\",4000000000]],[]");
            const auto admitted = admit_task_plan(bounded, bounded.snapshot(), bounded_dsl);
            const auto& task = std::get<AdmittedTaskPlan>(admitted);
            check(evaluate_task_plan(bounded, task, {}, {{"ram_bytes", std::int64_t{3500000000}}}).ready_actions.empty());
            check(evaluate_task_plan(bounded, task, {}, {{"ram_bytes", std::int64_t{2999999999}}}).ready_actions.size() == 1);
        }
        {
            const auto assumed = replace(single(host, "\"object\",\"goal\"", "reason"),
                "GOAL [\"root\",[],[],[],[],", "GOAL [\"root\",[],[],[],[\"assumption\"],");
            const auto admitted = admit_task_plan(host, host.snapshot(), assumed);
            const auto& task = std::get<AdmittedTaskPlan>(admitted);
            check(evaluate_task_plan(host, task, {}, offline).ready_actions.empty());
            check(evaluate_task_plan(host, task, {}, offline, {}, {host.objects().at("assumption").id}).ready_actions.size() == 1);
        }
        {
            const auto alternatives = replace(dsl, "GOAL [\"respond\",[],", "GOAL [\"respond\",[[\"run\"],[\"write\"]],");
            const auto admitted = admit_task_plan(host, host.snapshot(), alternatives);
            const auto& task = std::get<AdmittedTaskPlan>(admitted);
            check(task.definitions()[2].methods.size() == 2);
            check(std::all_of(task.definitions()[2].methods.begin(), task.definitions()[2].methods.end(),
                [&](const auto& method) { return method.back() == task.artifact_goals().at("runlog"); }));
            check(evaluate_task_plan(host, task, {{"write", completed()}}, offline,
                {{"code", identity("ready code")}}).ready_actions == std::vector<std::string>{"execute"});
        }
        {
            const auto explicit_repair = replace(dsl, "\"execute-code\",[\"code\"]",
                "\"execute-code\",[\"code\",\"instruction\"]");
            const auto accepted = admit_task_plan(host, host.snapshot(), explicit_repair);
            check(std::get<AdmittedTaskPlan>(accepted).actions()[1].inputs ==
                  std::vector<std::string>({"code", "instruction"}));
            rejects(ErrorCode::Schema, [&] {
                auto invalid = replace(dsl, "GOAL [\"write\"",
                    "ENTITY [\"not_text\",\"query\",[\"kind\",\"FACT\"]," + support + "]\nGOAL [\"write\"");
                invalid = replace(invalid, "\"execute-code\",[\"code\"]",
                    "\"execute-code\",[\"code\",\"not_text\"]");
                admit_task_plan(host, host.snapshot(), invalid);
            });
            rejects(ErrorCode::Schema, [&] {
                auto invalid = replace(dsl, "GOAL [\"write\"",
                    "ENTITY [\"extra\",\"text\"," + json_quote(handle(host)) + "," + support + "]\nGOAL [\"write\"");
                invalid = replace(invalid, "\"execute-code\",[\"code\"]",
                    "\"execute-code\",[\"code\",\"instruction\",\"extra\"]");
                admit_task_plan(host, host.snapshot(), invalid);
            });
        }
        for (const auto& [entity, action] : std::vector<std::pair<std::string, std::string>>{
            {"\"object\",\"goal\"", "reason"},
            {"\"source-query\",[\"module\",[\"atom\",[0,false,[[\"c\",0]]]]]", "source-reason"},
            {"\"source-query\",[\"module\",[\"q\",1,0]]", "source-reason"},
            {"\"query\",[\"source\",\"wiki\",\"1\"]", "retrieve"},
            {"\"query\",[\"related\",\"goal\"," + json_quote(name(Relationship::DependsOn)) + "]", "retrieve"},
            {"\"query\",[\"required-by\",\"goal\"]", "retrieve"},
            {"\"object\",\"document\"", "compile-source"},
            {"\"text\"," + json_quote(handle(host)), "generate-code"},
            {"\"text\"," + json_quote(handle(host)), "execute-code"},
            {"\"object\",\"goal\"", "explain"},
            {"\"object\",\"goal\"", "visualize"},
            {"\"object\",\"goal\"", "request-publication"}}) {
            const auto admitted = admit_task_plan(host, host.snapshot(), single(host, entity, action));
            check(std::holds_alternative<AdmittedTaskPlan>(admitted));
            const auto& task = std::get<AdmittedTaskPlan>(admitted);
            check(evaluate_task_plan(host, task, {}, offline).ready_actions == std::vector<std::string>{"action"});
        }
        for (const auto& [from, to, code] : std::vector<std::tuple<std::string, std::string, ErrorCode>>{
            {"GAMMATHINK_TASK_IR 1", "GAMMATHINK_DSL 1", ErrorCode::Schema},
            {"\"generate-code\"", "\"publish\"", ErrorCode::Schema},
            {"[\"code\"]," + support, "[\"instruction\"]," + support, ErrorCode::Schema},
            {"\"execute-code\",[\"code\"]", "\"reason\",[\"code\"]", ErrorCode::Schema},
            {"\"explain\",[\"runlog\"]", "\"explain\",[\"missing\"]", ErrorCode::Schema},
            {"GOAL [\"write\"", "GOAL [\"__task_injected\"", ErrorCode::Schema},
            {"\"artifact\",null", "\"artifact\",42", ErrorCode::Schema},
            {"[\"exit_code\",\"eq\",0]", "[\"exit_code\",\"eq\",0.0]", ErrorCode::Schema},
            {"[\"exit_code\",\"eq\",0]", "[\"exit_code\",\"eq\",9223372036854775808]", ErrorCode::Schema},
            {"[\"exit_code\",\"eq\",0]", "[\"exit_code\",\"eq\",-9223372036854775809]", ErrorCode::Schema},
            {"[\"exit_code\",\"eq\",0]", "[\"exit_code\",\"lt\",true]", ErrorCode::Schema},
            {"[\"exit_code\",\"eq\",0]", "[\"exit_code\",\"eq\",\"\\ud800\"]", ErrorCode::Schema},
            {"END\n", "OUTCOME [\"respond\",true]\nEND\n", ErrorCode::Schema},
            {"END\n", "END\nACTION []\n", ErrorCode::Schema}}) {
            rejects(code, [&] { admit_task_plan(host, host.snapshot(), replace(dsl, from, to)); });
        }
        rejects(ErrorCode::Schema, [&] {
            admit_task_plan(host, host.snapshot(), single(host, "\"object\",\"guessed-address\"", "reason"));
        });
        const auto older = "[" + json_quote(handle(host, 1)) + "]";
        rejects(ErrorCode::Schema, [&] {
            admit_task_plan(host, host.snapshot(), replace(dsl, "ROOT \"respond\"", "ROOT \"respond\"\nROOT \"write\""));
        });
        rejects(ErrorCode::Schema, [&] {
            admit_task_plan(host, host.snapshot(), replace(dsl, "GOAL [\"respond\",[],[],[],[]," + support,
                "GOAL [\"respond\",[],[],[],[]," + older));
        });
        rejects(ErrorCode::Schema, [&] {
            admit_task_plan(host, host.snapshot(), replace(dsl, "[\"runlog\"],[\"reply\"]," + support,
                "[\"runlog\"],[\"reply\"]," + older));
        });
        const auto clarification = start(host) + "CLARIFY [\"Missing requested subject\"," + support + "]\nEND\n";
        const auto declined = admit_task_plan(host, host.snapshot(), clarification);
        check(std::holds_alternative<TaskPlanDecline>(declined));
        check(std::get<TaskPlanDecline>(declined).kind == TaskDeclineKind::Clarify
            && std::get<TaskPlanDecline>(declined).snapshot == host.snapshot());
        rejects(ErrorCode::Schema, [&] {
            admit_task_plan(host, host.snapshot(), replace(clarification, "END\n", "ROOT \"run\"\nEND\n"));
        });
        {
            Directory directory;
            const auto path = directory.path / "journal.db";
            PersistedTaskPlan persisted;
            {
                Journal journal(path);
                check(journal.objects().objects.empty());
                persisted = persist_task_plan(journal, plan);
                check(persisted.actions.size() == 3 && persisted.entities.size() == 4 && persisted.goals.size() == 5);
                check(persist_task_plan(journal, plan).manifest == persisted.manifest);
                for (const auto& id : journal.objects().objects) {
                    check(journal.get(id)->visibility == Visibility::Private);
                    check(!journal.assessment(id));
                }
            }
            {
                Journal journal(path);
                const auto restored = restore_task_plan(journal, persisted.manifest);
                check(restored.dsl() == dsl && restored.snapshot() == host.snapshot());
                check(evaluate_task_plan(host, restored, {}, offline).ready_actions == std::vector<std::string>{"generate"});
            }
            raw_sql(path, "DELETE FROM objects WHERE id=x'" + hex(persisted.actions.front()) + "'");
            {
                Journal journal(path);
                const auto before = journal.objects().objects;
                rejects(ErrorCode::Integrity, [&] { restore_task_plan(journal, persisted.manifest); });
                check(journal.objects().objects == before && !journal.contains(persisted.actions.front()));
            }
        }
        std::cout << "Task plan passed: " << assertions << " deterministic assertions\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
