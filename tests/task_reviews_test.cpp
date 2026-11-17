#include "task_session.hpp"

#include <algorithm>
#include <iostream>
#include <json-c/json.h>
#include <unistd.h>

namespace {
using namespace axiom;
using namespace axiom::context;
using namespace axiom::knowledge;
using namespace axiom::runtime;
using Json = std::unique_ptr<json_object, decltype(&json_object_put)>;
std::size_t assertions = 0;
void check(bool value) {
    ++assertions;
    if (!value) throw std::runtime_error("Task review assertion " + std::to_string(assertions));
}
template<class Function> void rejects(ErrorCode code, Function function) {
    try { function(); } catch (const Error& error) { check(error.code == code); return; }
    throw std::runtime_error("Expected review rejection");
}
struct Directory {
    std::filesystem::path path;
    Directory() {
        auto pattern = (std::filesystem::temp_directory_path() / "task-reviews-XXXXXX").string();
        const auto* created = mkdtemp(pattern.data());
        if (!created) throw std::runtime_error("Cannot create review test directory");
        path = created;
    }
    ~Directory() { std::filesystem::remove_all(path); }
};
std::string quote(std::string_view value) {
    Json object(json_object_new_string_len(value.data(), static_cast<int>(value.size())), json_object_put);
    return json_object_to_json_string_ext(object.get(), JSON_C_TO_STRING_PLAIN);
}
std::string plan(const TaskPlanContext& context, bool publication, bool require_published = false) {
    return "GAMMATHINK_TASK_IR 1\nCONTEXT " + quote(context.snapshot()) + "\nROOT \"report\"\n"
        "ENTITY [\"input\",\"object\",\"first\",[\"s0\"]]\n"
        "ENTITY [\"second\",\"object\",\"second\",[\"s0\"]]\n"
        "ENTITY [\"result\",\"artifact\",null,[\"s0\"]]\n"
        "ENTITY [\"reply\",\"artifact\",null,[\"s0\"]]\n"
        "GOAL [\"work\",[],[]," + std::string(require_published ? "[[\"published\",\"eq\",true]]" : "[]") + ",[],[\"s0\"]]\n"
        "GOAL [\"report\",[[\"work\"]],[],[],[],[\"s0\"]]\n"
        "ACTION [\"review\",\"work\"," + quote(publication ? "request-publication" : "visualize") +
        (publication ? ",[\"input\",\"second\",\"input\"]" : ",[\"input\"]") + ",[\"result\"],[\"s0\"]]\n"
        "ACTION [\"describe\",\"report\",\"explain\",[\"result\"],[\"reply\"],[\"s0\"]]\nEND\n";
}
Json document(const Object& value) {
    return Json(json_tokener_parse(std::string(value.payload.begin(), value.payload.end()).c_str()), json_object_put);
}
void encode(Object& value, json_object* fields) {
    const std::string text = json_object_to_json_string_ext(fields, JSON_C_TO_STRING_PLAIN);
    value.payload.assign(text.begin(), text.end());
}
}

int main() {
    try {
        Directory directory;
        ConversationMemory memory(directory.path / "conversation.db");
        SessionTasks tasks(directory.path / "tasks.db");
        Journal journal(directory.path / "journal.db");
        neural::NativeModelManager model;
        JournalArchive local(journal); UnifiedKnowledge knowledge({&local});
        Object a; a.kind = ObjectKind::Claim; a.label = "First"; a.format = "synthetic.review.v1";
        a.payload = {0, 255, 1};
        const auto first = journal.put(a);
        auto b = a; b.label = "Second"; const auto second = journal.put(b);
        auto rule = a; rule.kind = ObjectKind::Rule; rule.label = "Joint rule";
        const auto rule_id = journal.put(rule);
        auto conclusion = a; conclusion.kind = ObjectKind::Fact; conclusion.label = "Conclusion";
        const auto conclusion_id = journal.put(conclusion);
        Object proof; proof.kind = ObjectKind::Derivation; proof.label = "Ordered joint premises";
        proof.format = a.format; proof.concepts = {"joint"};
        proof.derivation = Derivation{conclusion_id, rule_id, {second, first, second}, {}, {}};
        proof.sources = {{"synthetic-zim", "1", "article", "verbatim", digest(a.payload), 9007199254740993ULL, 1}};
        const auto proof_id = journal.put(proof);
        const auto capture = [&](std::string_view session) {
            ProblemContext host; host.unit_registry_id = "review-test"; host.unit_registry_version = "1";
            return prepare_task_prompt(retrieve_context(memory, session), host,
                [](std::string_view text) { return text.size(); }, {1000000, 262144, 256},
                {{"first", {proof_id, proof.kind, proof.format}}, {"second", {second, b.kind, b.format}}});
        };
        for (const bool publication : {false, true}) {
            const std::string name = publication ? "publication" : "inspection";
            memory.open_session(name, "test");
            const auto message = memory.append(name, Role::User, "1", "Inspect selected knowledge and report.");
            const auto prepared = capture(name);
            std::uint64_t revision;
            ObjectId view_id, marker_id;
            {
                TaskSession session(memory, tasks, journal, model);
                const auto task = std::get<SessionTask>(session.accept_plan(name, message.id, prepared.context,
                    prepared.context.snapshot(), plan(prepared.context, publication), 0));
                TaskExecutionState state;
                if (!publication) {
                    rejects(ErrorCode::Budget, [&] { (void)session.review_action(name, task.revision, prepared.context,
                        "review", state, knowledge, {}, {}, {.bytes = 1}); });
                    check(tasks.history(name).events.size() == 1 && state.steps.empty());
                }
                const auto step = session.review_action(name, task.revision, prepared.context, "review", state, knowledge, {});
                revision = tasks.active(name)->revision; view_id = step.artifact; marker_id = step.receipt;
                check(step.status == KnowledgeRunStatus::Checked && step.outcome.succeeded);
                check(step.certificates.empty() && step.results.empty() && step.native_executions == 0);
                check(!std::get<bool>(step.outcome.observed.at("published")));
                check(!std::get<bool>(step.outcome.observed.at("publication_approved")));
                check(!std::get<bool>(step.outcome.observed.at("empirical_truth_verified")));
                check(!std::get<bool>(step.outcome.observed.at("native_checked")));
                check(journal.get(proof_id) == proof && !journal.assessment(proof_id));
                const auto view = *journal.get(view_id);
                check(view.visibility == Visibility::Private && view.kind == ObjectKind::Evidence);
                check(!journal.assessment(view_id) && !journal.assessment(marker_id));
                const auto fields = document(view);
                if (publication) {
                    auto* roots = json_object_object_get(fields.get(), "selected_roots");
                    check(json_object_array_length(roots) == 3);
                    check(std::string(json_object_get_string(json_object_array_get_idx(roots, 0))) == hex(proof_id));
                    check(std::string(json_object_get_string(json_object_array_get_idx(roots, 1))) == hex(second));
                    check(std::string(json_object_get_string(json_object_array_get_idx(roots, 2))) == hex(proof_id));
                    check(!json_object_get_boolean(json_object_object_get(fields.get(), "approved")));
                    check(std::string(json_object_get_string(json_object_object_get(fields.get(), "status"))) == "pending-host-review");
                } else {
                    auto* nodes = json_object_object_get(fields.get(), "nodes");
                    check(json_object_array_length(nodes) >= 5);
                    auto* root = json_object_array_get_idx(nodes, 0);
                    auto* premises = json_object_object_get(json_object_object_get(root, "derivation"), "premises");
                    check(json_object_array_length(premises) == 3);
                    check(std::string(json_object_get_string(json_object_array_get_idx(premises, 0))) == hex(second));
                    check(std::string(json_object_get_string(json_object_array_get_idx(premises, 2))) == hex(second));
                    auto* source = json_object_array_get_idx(json_object_object_get(root, "sources"), 0);
                    check(std::string(json_object_get_string(json_object_object_get(source, "offset"))) == "9007199254740993");
                }
            }
            {
                // New incoming edges must not change the historical captured graph.
                auto later = a; later.label = name; later.links = {{Relationship::DependsOn, proof_id}};
                journal.put(later);
                const auto original = *journal.get(view_id);
                TaskSession reopened(memory, tasks, journal, model);
                const auto restored = reopened.restore_native_execution(name, revision, prepared.context, knowledge, {});
                check(restored.steps.at("review").artifact == view_id && restored.steps.at("review").receipt == marker_id);
                check(std::get<bool>(restored.steps.at("review").outcome.observed.at("review_record_restored")));
                check(journal.get(view_id) == original && restored.certificates.empty());
                UnifiedKnowledge offline({});
                const auto unavailable = reopened.restore_native_execution(name, revision, prepared.context, offline, {});
                check(unavailable.steps.at("review").artifact == view_id);
                check(!std::get<bool>(unavailable.steps.at("review").outcome.observed.at("review_focus_available")));
                check(!unavailable.steps.at("review").unavailable.empty() && unavailable.certificates.empty());
                auto state = restored;
                const auto explained = reopened.execute_native_action(name, revision, prepared.context, "describe", state, knowledge, {});
                check(explained.response.has_value() && model.state() == neural::ModelState::Unloaded);
            }
        }
        // Recording a request cannot satisfy actual publication.
        memory.open_session("unpublished", "test");
        const auto message = memory.append("unpublished", Role::User, "1", "Publish the selected knowledge.");
        const auto prepared = capture("unpublished");
        {
            TaskSession session(memory, tasks, journal, model);
            const auto task = std::get<SessionTask>(session.accept_plan("unpublished", message.id, prepared.context,
                prepared.context.snapshot(), plan(prepared.context, true, true), 0));
            TaskExecutionState state;
            (void)session.review_action("unpublished", task.revision, prepared.context, "review", state, knowledge, {});
            check(tasks.active("unpublished")->phase == TaskPhase::Unresolved);
            Assessment withdrawn; withdrawn.state = KnowledgeState::Invalidated;
            withdrawn.checker = "synthetic-review-checker"; withdrawn.reason = "Requires revalidation";
            journal.record_assessment(proof_id, withdrawn);
            const auto current = session.restore_native_execution("unpublished", tasks.active("unpublished")->revision,
                prepared.context, knowledge, {});
            const auto& pending = current.steps.at("review");
            check(std::find(pending.revalidation_required.begin(), pending.revalidation_required.end(), proof_id) !=
                pending.revalidation_required.end());
            check(!std::get<bool>(pending.outcome.observed.at("published")) && current.certificates.empty());
            state = current;
            rejects(ErrorCode::Permission, [&] { (void)session.execute_native_action("unpublished",
                tasks.active("unpublished")->revision, prepared.context, "describe", state, knowledge, {}); });
        }
        // Even a self-consistent rewritten receipt cannot authorize publication.
        memory.open_session("forged", "test");
        const auto forged_message = memory.append("forged", Role::User, "1", "Request publication.");
        const auto forged_context = capture("forged");
        {
            TaskSession session(memory, tasks, journal, model);
            auto task = std::get<SessionTask>(session.accept_plan("forged", forged_message.id, forged_context.context,
                forged_context.context.snapshot(), plan(forged_context.context, true), 0));
            TaskExecutionState state;
            const auto step = session.review_action("forged", task.revision, forged_context.context, "review", state, knowledge, {});
            auto view = *journal.get(step.artifact); auto fields = document(view);
            json_object_object_add(fields.get(), "approved", json_object_new_boolean(true)); encode(view, fields.get());
            const auto changed_id = journal.put(view);
            auto marker = *journal.get(step.receipt); fields = document(marker);
            json_object_object_add(fields.get(), "artifact", json_object_new_string(hex(changed_id).c_str()));
            encode(marker, fields.get()); marker.links.back().target = changed_id;
            // A separate active task with the same manifest avoids duplicate action markers.
            memory.open_session("forged-replay", "test");
            const auto next_message = memory.append("forged-replay", Role::User, "1", "Request publication.");
            const auto next_context = capture("forged-replay");
            task = std::get<SessionTask>(session.accept_plan("forged-replay", next_message.id, next_context.context,
                next_context.context.snapshot(), plan(next_context.context, true), 0));
            fields = document(view);
            json_object_object_add(fields.get(), "manifest", json_object_new_string(hex(task.ordered_task).c_str()));
            encode(view, fields.get()); view.links.front().target = task.ordered_task;
            const auto forged_view_id = journal.put(view);
            fields = document(marker);
            json_object_object_add(fields.get(), "manifest", json_object_new_string(hex(task.ordered_task).c_str()));
            json_object_object_add(fields.get(), "artifact", json_object_new_string(hex(forged_view_id).c_str()));
            encode(marker, fields.get()); marker.links.front().target = task.ordered_task; marker.links.back().target = forged_view_id;
            task = tasks.transition("forged-replay", task.revision, TaskPhase::Reasoning);
            task = tasks.transition("forged-replay", task.revision, TaskPhase::Unresolved, {},
                CompletedTaskAction{task.revision + 1, "review", journal.put(marker)});
            rejects(ErrorCode::Integrity, [&] { (void)session.restore_native_execution("forged-replay", task.revision,
                next_context.context, knowledge, {}); });
        }
        std::cout << "Task review checks passed: " << assertions << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
