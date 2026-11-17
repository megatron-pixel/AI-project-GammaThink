#include "retention.hpp"

#include <deque>
#include <set>

namespace axiom::knowledge {
namespace {
void require(bool value, ErrorCode code, const char* message) {
    if (!value) throw Error(code, message);
}
}
RetentionPlan plan_retention(UnifiedKnowledge& knowledge, Journal& journal,
                             std::span<const ObjectId> roots, std::span<const ObjectId> candidates,
                             RetentionLimits limits) {
    require(limits.objects > 0 && limits.objects <= maximum_items && limits.edges > 0
        && limits.edges <= 65536 && limits.depth <= maximum_items && limits.record_bytes > 0
        && limits.record_bytes <= 64 * 1024 * 1024, ErrorCode::Budget, "Invalid retention limits");
    require(!roots.empty() && roots.size() <= limits.objects && candidates.size() <= maximum_items,
            ErrorCode::Budget, "Retention selection exceeds bounds");
    std::set<ObjectId> discovered(roots.begin(), roots.end());
    require(discovered.size() == roots.size(), ErrorCode::Schema, "Duplicate retention root");
    require(std::set<ObjectId>(candidates.begin(), candidates.end()).size() == candidates.size(),
            ErrorCode::Schema, "Duplicate retention candidate");
    RetentionPlan plan;
    plan.roots.assign(roots.begin(), roots.end());
    std::deque<std::pair<ObjectId, std::size_t>> pending;
    for (const auto& id : roots) pending.emplace_back(id, 0);
    std::set<std::size_t> archives;
    try {
        journal.read_snapshot([&] {
            while (!pending.empty()) {
                const auto [id, depth] = pending.front(); pending.pop_front();
                ++plan.lookups;
                std::optional<RetrievedObject> object;
                try { object = knowledge.get(id); }
                catch (const Error& error) {
                    if (error.code != ErrorCode::Missing && error.code != ErrorCode::Unavailable) throw;
                }
                if (!object) { plan.unavailable.push_back(id); continue; }
                const auto size = snapshot_record_size(object->record);
                require(size <= limits.record_bytes - plan.record_bytes, ErrorCode::Budget,
                        "Retention record budget exceeded");
                plan.record_bytes += size;
                auto dependencies = references(object->record.object);
                if (const auto& assessment = object->record.assessment) {
                    dependencies.insert(dependencies.end(), assessment->checked_dependencies.begin(),
                                        assessment->checked_dependencies.end());
                    if (assessment->certificate) dependencies.push_back(*assessment->certificate);
                }
                archives.insert(object->archive);
                plan.retained.push_back(std::move(*object));
                for (const auto& dependency : dependencies) {
                    require(plan.edges < limits.edges, ErrorCode::Budget, "Retention edge budget exceeded");
                    ++plan.edges;
                    if (discovered.contains(dependency)) continue;
                    require(discovered.size() < limits.objects && depth < limits.depth,
                            ErrorCode::Budget, "Retention dependency closure exceeds bounds");
                    discovered.insert(dependency);
                    pending.emplace_back(dependency, depth + 1);
                }
            }
        });
    } catch (const Error& error) {
        if (error.code != ErrorCode::Budget) throw;
        plan.status = RetentionStatus::ResourceLimited;
    }
    plan.archives.assign(archives.begin(), archives.end());
    if (plan.status != RetentionStatus::ResourceLimited) {
        if (!plan.unavailable.empty()) plan.status = RetentionStatus::DependenciesUnavailable;
        else for (const auto& id : candidates)
            if (!discovered.contains(id)) plan.optional_omissions.push_back(id);
    }
    return plan;
}

} // namespace axiom::knowledge
