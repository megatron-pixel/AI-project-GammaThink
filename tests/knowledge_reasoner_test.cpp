#include "knowledge_reasoner.hpp"
#include "virtual_drive.hpp"
#include "core.hpp"

#include <filesystem>
#include <iostream>
#include <source_location>
#include <stdlib.h>
#include <unistd.h>

namespace {
using namespace axiom;
using namespace axiom::knowledge;
std::size_t checks = 0;
void check(bool value, const std::source_location where = std::source_location::current()) {
    ++checks;
    if (!value) throw std::runtime_error("Knowledge reasoner assertion at line " + std::to_string(where.line()));
}
struct Directory {
    std::filesystem::path path;
    Directory() {
        auto pattern = (std::filesystem::temp_directory_path() / "knowledge-reasoner-XXXXXX").string();
        const auto made = mkdtemp(pattern.data());
        if (!made) throw std::runtime_error("Cannot create knowledge reasoner directory");
        path = made;
    }
    ~Directory() { std::filesystem::remove_all(path); }
};
Object goal(const ObjectId& dependency, std::string_view module) {
    Object value;
    value.kind = ObjectKind::Goal;
    value.format = native_request_format;
    value.payload = encode_request(compile_proof(parse_module(std::string(module)), "b"));
    value.links.push_back({Relationship::DependsOn, dependency});
    return value;
}
}

int main() {
    try {
        Directory directory;
        Journal source(":memory:");
        Object evidence;
        evidence.kind = ObjectKind::Evidence;
        evidence.format = "text/plain";
        evidence.payload = {'r', 'u', 'l', 'e'};
        evidence.sources = {{"synthetic", "1", "entry", "literal-v1", digest(evidence.payload), 0, 4}};
        const auto dependency = source.put(evidence);
        auto input = goal(dependency, "axioms 1 archive 1\nfact a | source-a\nrule a -> b | implication\n");
        input.sources = evidence.sources;
        const auto id = source.put(input);
        Assessment untrusted;
        untrusted.checker = "publisher-self-report";
        untrusted.state = KnowledgeState::Verified;
        untrusted.logical = LogicalValidity::Valid;
        untrusted.evidence = EvidenceAssessment::Supported;
        source.record_assessment(id, untrusted);
        const auto snapshot = compile_snapshot(source, directory.path / "snapshot", "facts", "1");
        ObjectId certificate;
        {
            Journal local(directory.path / "journal.db");
            ArchiveRegistry registry(directory.path / "catalogue.db");
            registry.register_snapshot(directory.path / "snapshot", snapshot.root);
            registry.mount("facts", "facts", "1", directory.path / "snapshot", 0);
            VirtualDrive drive(local, registry);
            auto view = drive.open_view();
            const auto result = run_knowledge_goal(view->knowledge(), local, id);
            check(result.status == KnowledgeRunStatus::Checked);
            check(result.object_lookups == 2 && result.imported_objects == 2);
            check(result.execution.check.valid && !result.execution.reused);
            check(!result.conclusions.empty() && !result.derivations.empty());
            check(!local.assessment(id));
            certificate = *result.execution.certificate;
            const auto checked = local.assessment(certificate);
            check(checked && checked->logical == LogicalValidity::Valid);
            check(checked->evidence == EvidenceAssessment::Unassessed);
            for (const auto& derived : result.derivations) {
                check(local.get(derived)->sources == input.sources);
                check(!local.assessment(derived));
            }
            const auto repeated = run_knowledge_goal(view->knowledge(), local, id);
            check(repeated.execution.reused && repeated.imported_objects == 0);
        }
        Journal reopened(directory.path / "journal.db");
        JournalArchive own(reopened);
        UnifiedKnowledge offline({&own});
        const auto cached = run_knowledge_goal(offline, reopened, id);
        check(cached.status == KnowledgeRunStatus::Checked && cached.execution.reused);
        check(cached.execution.certificate == certificate);
        reopened.invalidate(dependency, "source changed");
        const auto invalidated = run_knowledge_goal(offline, reopened, id);
        check(invalidated.status == KnowledgeRunStatus::RevalidationRequired);
        check(!invalidated.revalidation_required.empty() && !invalidated.execution.certificate);

        Journal local(":memory:");
        JournalArchive original(source);
        UnifiedKnowledge knowledge({&original});
        KnowledgeRunLimits tight;
        tight.retrieval.objects = 1;
        const auto limited = run_knowledge_goal(knowledge, local, id, tight);
        check(limited.status == KnowledgeRunStatus::ResourceLimited);
        check(!limited.execution.certificate && local.objects().objects.empty());
        const auto missing = run_knowledge_goal(knowledge, local, ObjectId{digest(Bytes{1, 2, 3})});
        check(missing.status == KnowledgeRunStatus::DependenciesUnavailable);
        check(missing.unavailable.size() == 1 && !missing.execution.certificate);

        Journal incomplete(":memory:");
        const auto broken = incomplete.put(goal(ObjectId{digest(Bytes{9})},
            "axioms 1 missing 1\nfact a | source-a\nrule a -> b | implication\n"));
        JournalArchive partial(incomplete);
        UnifiedKnowledge partial_knowledge({&partial});
        const auto absent = run_knowledge_goal(partial_knowledge, local, broken);
        check(absent.status == KnowledgeRunStatus::DependenciesUnavailable);
        check(absent.object_lookups == 2 && absent.imported_objects == 0);
        check(local.objects().objects.empty());

        auto conflicting = goal(dependency,
            "axioms 1 conflict 1\nfact b | first\nfact !b | second\n");
        const auto conflicting_id = source.put(conflicting);
        const auto conflict = run_knowledge_goal(knowledge, local, conflicting_id);
        check(conflict.status == KnowledgeRunStatus::Conflict && conflict.execution.check.conflict);
        check(!local.by_kind(ObjectKind::Contradiction).objects.empty());
        const auto unknown_id = source.put(goal(dependency, "axioms 1 unknown 1\nfact a | first\n"));
        const auto unknown = run_knowledge_goal(knowledge, local, unknown_id);
        check(unknown.status == KnowledgeRunStatus::Unresolved && !unknown.execution.check.conclusive);
        check(unknown.conclusions.empty());
        check(!source.contains(*conflict.execution.certificate));
        std::cout << "knowledge reasoning integration passed: " << checks << " deterministic assertions\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
