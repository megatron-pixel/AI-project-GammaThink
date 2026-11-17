#include "knowledge_reasoner.hpp"
#include "core.hpp"
#include "problem_wire.hpp"
#include "relational.hpp"
#include <algorithm>

namespace axiom::knowledge {
namespace {
void validate_native_goal(const Object& goal) {
    const auto& bytes = goal.payload;
    if (goal.kind != ObjectKind::Goal || goal.format != native_request_format ||
        bytes.size() < 12 || bytes[0] != 'A' || bytes[1] != 'X' || bytes[2] != 'R' || bytes[3] != 'M') {
        throw Error(ErrorCode::Schema, "Knowledge reasoning requires an admitted native formal goal");
    }
    const auto version = static_cast<unsigned>(bytes[4] | (static_cast<unsigned>(bytes[5]) << 8));
    bool gpu = false;
    switch (version) {
    case 1: gpu = decode_request(bytes).gpu; break;
    case 2: gpu = decode_relational_request(bytes).gpu; break;
    case 3: (void)decode_problem_request(bytes); break;
    default: throw Error(ErrorCode::Schema, "Unsupported stored native goal version");
    }
    if (gpu) throw Error(ErrorCode::Schema, "Knowledge reasoning baseline requires CPU requests");
}
}

KnowledgeRun run_knowledge_goal(UnifiedKnowledge& knowledge, Journal& journal,
    const ObjectId& goal, KnowledgeRunLimits limits, const std::set<ObjectId>& accepted_assumptions) {
    KnowledgeRun result;
    try {
        const auto closure = knowledge.resolve(goal, limits.retrieval);
        result.object_lookups = closure.objects.size() + closure.unavailable.size();
        result.unavailable = closure.unavailable;
        if (!closure.complete()) {
            result.status = KnowledgeRunStatus::DependenciesUnavailable;
            result.reason = "Required knowledge objects are unavailable; no native execution occurred";
            return result;
        }
        const Object* root = nullptr;
        for (const auto& item : closure.objects) {
            if (item.id == goal) root = &item.record.object;
            const auto& assessment = item.record.assessment;
            if (assessment && (assessment->state == KnowledgeState::Invalidated ||
                               assessment->logical == LogicalValidity::Invalid)) {
                result.revalidation_required.push_back(item.id);
            }
        }
        if (!root) throw Error(ErrorCode::Integrity, "Complete knowledge closure lost its root");
        validate_native_goal(*root);
        for (const auto& link : root->links) {
            if (link.relationship != Relationship::Requires) continue;
            const auto target = std::find_if(closure.objects.begin(), closure.objects.end(),
                [&](const auto& item) { return item.id == link.target; });
            if (target != closure.objects.end() && target->record.object.kind == ObjectKind::Assumption &&
                !accepted_assumptions.contains(link.target)) {
                result.status = KnowledgeRunStatus::RevalidationRequired;
                result.revalidation_required.push_back(link.target);
                result.reason = "A required assumption has not been explicitly accepted for this run";
                return result;
            }
        }
        if (!result.revalidation_required.empty()) {
            result.status = KnowledgeRunStatus::RevalidationRequired;
            result.reason = "Observed invalidation requires revalidation, not an inference of falsity";
            return result;
        }
        for (const auto& item : closure.objects) {
            if (!journal.contains(item.id)) {
                if (journal.put(item.record.object) != item.id) {
                    throw Error(ErrorCode::Integrity, "Imported knowledge identity changed");
                }
                ++result.imported_objects;
            }
            // Never copy remote/snapshot assessments into the trusted journal.
        }
        result.execution = run_native(journal, goal, limits.reasoning);
        if (!result.execution.unavailable.empty()) {
            result.status = KnowledgeRunStatus::DependenciesUnavailable;
            result.unavailable = result.execution.unavailable;
            result.reason = "Current local dependency validation prevents reuse or execution";
            return result;
        }
        if (!result.execution.certificate || !result.execution.check.valid) {
            throw Error(ErrorCode::Integrity, "Native knowledge execution lacks a checked certificate");
        }
        const auto stored_goal = journal.get(goal);
        const auto certificate = journal.get(*result.execution.certificate);
        if (!stored_goal || !certificate) throw Error(ErrorCode::Unavailable, "Checked knowledge became unavailable");
        const auto graph = materialize_native(*stored_goal, *certificate, limits.materialization);
        for (const auto& object : graph.objects) journal.put(object);
        result.conclusions = graph.conclusions;
        result.derivations = graph.derivations;
        result.status = result.execution.check.conflict ? KnowledgeRunStatus::Conflict
            : result.execution.check.conclusive ? KnowledgeRunStatus::Checked : KnowledgeRunStatus::Unresolved;
        return result;
    } catch (const Error& error) {
        if (error.code == ErrorCode::Budget) result.status = KnowledgeRunStatus::ResourceLimited;
        else if (error.code == ErrorCode::Unavailable) result.status = KnowledgeRunStatus::DependenciesUnavailable;
        else throw;
        result.reason = error.what();
        return result;
    }
}

} // namespace axiom::knowledge
