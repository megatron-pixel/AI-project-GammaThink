#include "contribution.hpp"

#include <algorithm>
#include <deque>
#include <set>

namespace axiom::knowledge {
namespace {
void require(bool value, ErrorCode code, const char* message) {
    if (!value) throw Error(code, message);
}

std::vector<ObjectId> dependencies(const Object& object, const std::optional<Assessment>& assessment) {
    auto result = references(object);
    if (assessment) {
        result.insert(result.end(), assessment->checked_dependencies.begin(), assessment->checked_dependencies.end());
        if (assessment->certificate) result.push_back(*assessment->certificate);
    }
    return result;
}
}

PreparedContribution prepare_contribution(Journal& journal, std::span<const ObjectId> roots,
                                          const std::filesystem::path& destination,
                                          std::string name, std::string revision,
                                          ContributionLimits limits) {
    require(limits.objects > 0 && limits.objects <= maximum_items && limits.edges > 0
        && limits.edges <= 65536 && limits.input_bytes > 0 && limits.input_bytes <= 64 * 1024 * 1024,
        ErrorCode::Budget, "Invalid contribution limits");
    require(!roots.empty() && roots.size() <= limits.objects, ErrorCode::Schema, "Invalid contribution roots");
    std::set<ObjectId> selected;
    std::deque<ObjectId> pending;
    std::size_t edges = 0;
    const auto enqueue = [&](const ObjectId& id) {
        if (selected.contains(id)) return;
        require(selected.size() < limits.objects, ErrorCode::Budget, "Contribution object limit exceeded");
        selected.insert(id);
        pending.push_back(id);
    };
    for (const auto& root : roots) enqueue(root);
    require(selected.size() == roots.size(), ErrorCode::Schema, "Duplicate contribution root");
    journal.read_snapshot([&] {
        std::uint64_t bytes = 0;
        while (!pending.empty()) {
            const auto id = pending.front();
            pending.pop_front();
            auto object = journal.get(id);
            require(object.has_value(), ErrorCode::Missing, "Contribution dependency is unavailable");
            require(object->visibility == Visibility::Public, ErrorCode::Permission,
                    "Contribution contains a private object");
            KnowledgeRecord record{std::move(*object), journal.assessment(id)};
            const auto size = snapshot_record_size(record);
            require(size <= limits.input_bytes - bytes, ErrorCode::Budget,
                    "Contribution input byte limit exceeded");
            bytes += size;
            for (const auto& dependency : dependencies(record.object, record.assessment)) {
                require(edges < limits.edges, ErrorCode::Budget, "Contribution dependency edge limit exceeded");
                ++edges;
                enqueue(dependency);
            }
        }
    });
    PreparedContribution result;
    result.directory = destination;
    result.roots.assign(roots.begin(), roots.end());
    result.objects.assign(selected.begin(), selected.end());
    limits.snapshot.objects = std::min<std::uint64_t>(limits.snapshot.objects, limits.objects);
    limits.snapshot.input_bytes = std::min(limits.snapshot.input_bytes, limits.input_bytes);
    result.snapshot = compile_selected_snapshot(journal, result.objects, destination,
        std::move(name), std::move(revision), limits.snapshot);

    // Compilation owns a second coherent journal view. Validate its actual
    // pinned bytes, not old observations: a changed assessment can add a new
    // dependency between selection and compilation. Such a packet is not ready
    // for approval, and no implicit retry or publication occurs.
    GksSnapshot snapshot(destination, result.snapshot.root);
    require(snapshot.manifest().object_count == selected.size(), ErrorCode::Integrity,
            "Contribution snapshot selection differs");
    edges = 0;
    for (const auto& id : result.objects) {
        const auto record = snapshot.get(id);
        require(record.has_value(), ErrorCode::Integrity, "Contribution snapshot object is missing");
        require(record->object.visibility == Visibility::Public, ErrorCode::Permission,
                "Contribution snapshot contains private data");
        for (const auto& dependency : dependencies(record->object, record->assessment)) {
            require(edges < limits.edges, ErrorCode::Budget, "Contribution snapshot edge limit exceeded");
            ++edges;
            require(selected.contains(dependency), ErrorCode::Unavailable,
                    "Contribution dependencies changed during preparation");
        }
        result.observed.push_back({id, record->assessment ? record->assessment->version : 0});
    }
    return result;
}

PreparedContribution review_contribution(const std::filesystem::path& directory, const BlockId& root,
                                         std::span<const ObjectId> roots, ContributionLimits limits) {
    require(limits.objects > 0 && limits.objects <= maximum_items && limits.edges > 0 && limits.edges <= 65536
        && limits.input_bytes > 0 && limits.input_bytes <= 64 * 1024 * 1024, ErrorCode::Budget, "Invalid contribution limits");
    require(!roots.empty() && roots.size() <= limits.objects, ErrorCode::Schema, "Invalid contribution roots");
    std::set<ObjectId> selected(roots.begin(), roots.end());
    require(selected.size() == roots.size(), ErrorCode::Schema, "Duplicate contribution root");
    std::deque<ObjectId> pending(roots.begin(), roots.end());
    PreparedContribution result;
    result.directory = directory; result.snapshot.root = root;
    result.roots.assign(roots.begin(), roots.end());
    GksSnapshot snapshot(directory, root);
    require(snapshot.manifest().object_count <= limits.objects, ErrorCode::Budget, "Contribution object limit exceeded");
    std::size_t edges = 0;
    while (!pending.empty()) {
        const auto id = pending.front(); pending.pop_front();
        const auto record = snapshot.get(id);
        require(record.has_value(), ErrorCode::Missing, "Contribution dependency is unavailable");
        require(record->object.visibility == Visibility::Public, ErrorCode::Permission, "Contribution contains a private object");
        const auto size = snapshot_record_size(*record);
        require(size <= limits.input_bytes - result.snapshot.input_bytes, ErrorCode::Budget, "Contribution input byte limit exceeded");
        result.snapshot.input_bytes += size;
        result.observed.push_back({id, record->assessment ? record->assessment->version : 0});
        for (const auto& dependency : dependencies(record->object, record->assessment)) {
            require(edges < limits.edges, ErrorCode::Budget, "Contribution dependency edge limit exceeded");
            ++edges;
            if (!selected.contains(dependency)) {
                require(selected.size() < limits.objects, ErrorCode::Budget, "Contribution object limit exceeded");
                selected.insert(dependency); pending.push_back(dependency);
            }
        }
    }
    require(snapshot.manifest().object_count == selected.size(), ErrorCode::Integrity, "Contribution contains unrelated objects");
    result.objects.assign(selected.begin(), selected.end());
    result.snapshot.objects = result.objects.size();
    std::sort(result.observed.begin(), result.observed.end(),
        [](const auto& a, const auto& b) { return a.object < b.object; });
    return result;
}

} // namespace axiom::knowledge
