#include "contribution.hpp"

#include <iostream>
#include <unistd.h>

namespace {
using namespace axiom::knowledge;
std::size_t assertions = 0;
void check(bool value) {
    ++assertions;
    if (!value) throw std::runtime_error("Contribution assertion " + std::to_string(assertions));
}
template<class Function> void rejects(ErrorCode expected, Function function) {
    try { function(); }
    catch (const Error& error) { check(error.code == expected); return; }
    throw std::runtime_error("Expected contribution rejection");
}
struct Directory {
    std::filesystem::path path;
    Directory() {
        auto pattern = (std::filesystem::temp_directory_path() / "gammathink-contribution-XXXXXX").string();
        std::vector<char> value(pattern.begin(), pattern.end()); value.push_back(0);
        const auto* made = mkdtemp(value.data());
        if (!made) throw std::runtime_error("Cannot create contribution test directory");
        path = made;
    }
    ~Directory() { std::filesystem::remove_all(path); }
};
Object object(ObjectKind kind, std::string label) {
    Object value;
    value.kind = kind;
    value.label = std::move(label);
    value.visibility = Visibility::Public;
    value.format = "synthetic.contribution-test.v1";
    value.payload = {1, 2, 3};
    return value;
}
}
int main() {
    try {
        Directory directory;
        Journal journal(directory.path / "journal.db");
        const auto premise = journal.put(object(ObjectKind::Claim, "source premise"));
        const auto evidence = journal.put(object(ObjectKind::Evidence, "source evidence"));
        const auto conclusion = journal.put(object(ObjectKind::Fact, "conclusion"));
        const auto rule = journal.put(object(ObjectKind::Rule, "rule"));
        auto proof = object(ObjectKind::Derivation, "joint derivation");
        proof.derivation = Derivation{conclusion, rule, {premise, premise}, {}, {evidence}};
        proof.sources = {{"synthetic", "v1", "A/source", "fixture", {}, 9007199254740993ULL, 8}};
        const auto root = journal.put(proof);
        auto alternate = object(ObjectKind::Derivation, "separate alternative");
        alternate.derivation = Derivation{conclusion, rule, {evidence}, {}, {}};
        alternate.links = {{Relationship::AlternativeTo, root}};
        const auto alternative = journal.put(alternate);
        auto conflict = object(ObjectKind::Contradiction, "contradiction");
        conflict.links = {{Relationship::Contradicts, root}};
        const auto contradiction = journal.put(conflict);
        auto secret = object(ObjectKind::Evidence, "unrelated private evidence");
        secret.visibility = Visibility::Private;
        const auto private_id = journal.put(secret);
        const auto checked = journal.put(object(ObjectKind::TestResult, "checked dependency"));
        Assessment observed;
        observed.checker = "synthetic-observation-not-a-proof";
        observed.logical = LogicalValidity::Valid;
        observed.evidence = EvidenceAssessment::Conflicting;
        observed.state = KnowledgeState::Disputed;
        observed.checked_dependencies = {checked};
        journal.record_assessment(root, observed);
        const auto revision = journal.record_assessment(root, observed);
        const std::array roots{alternative, contradiction};
        const auto prepared = prepare_contribution(journal, roots, directory.path / "prepared", "selected", "r1");
        check(prepared.roots == std::vector<ObjectId>(roots.begin(), roots.end()));
        check(prepared.objects.size() == 8 && prepared.snapshot.objects == 8);
        check(prepared.observed.size() == prepared.objects.size());
        GksSnapshot snapshot(prepared.directory, prepared.snapshot.root);
        check(!snapshot.get(private_id));
        const auto got = snapshot.get(root);
        check(got && got->object == proof && got->assessment == revision);
        check(got->object.derivation->premises == std::vector<ObjectId>({premise, premise}));
        check(got->object.sources.front().offset == 9007199254740993ULL);
        check(snapshot.get(alternative)->object == alternate);
        check(snapshot.get(contradiction)->object == conflict);
        check(snapshot.get(checked).has_value());
        check(journal.get(private_id) == secret && journal.assessment(root) == revision);
        rejects(ErrorCode::Permission, [&] {
            prepare_contribution(journal, std::array{private_id}, directory.path / "private", "selected", "r1");
        });
        check(!std::filesystem::exists(directory.path / "private"));
        auto leaking = object(ObjectKind::Claim, "public with private dependency");
        leaking.links = {{Relationship::Requires, private_id}};
        const auto leaking_id = journal.put(leaking);
        rejects(ErrorCode::Permission, [&] {
            prepare_contribution(journal, std::array{leaking_id}, directory.path / "leaking", "selected", "r1");
        });
        auto checked_secret = observed;
        checked_secret.checked_dependencies = {private_id};
        journal.record_assessment(leaking_id, checked_secret);
        auto indirect = object(ObjectKind::Claim, "public with checked private dependency");
        const auto indirect_id = journal.put(indirect);
        journal.record_assessment(indirect_id, checked_secret);
        rejects(ErrorCode::Permission, [&] {
            prepare_contribution(journal, std::array{indirect_id}, directory.path / "checked-private", "selected", "r1");
        });
        auto certificate_secret = observed;
        certificate_secret.checked_dependencies.clear();
        certificate_secret.certificate = private_id;
        const auto certified_id = journal.put(object(ObjectKind::Claim, "public with private certificate"));
        journal.record_assessment(certified_id, certificate_secret);
        rejects(ErrorCode::Permission, [&] {
            prepare_contribution(journal, std::array{certified_id}, directory.path / "certificate-private", "selected", "r1");
        });
        auto missing = object(ObjectKind::Claim, "missing dependency");
        missing.links = {{Relationship::Requires, ObjectId{}}};
        const auto missing_id = journal.put(missing);
        rejects(ErrorCode::Missing, [&] {
            prepare_contribution(journal, std::array{missing_id}, directory.path / "missing", "selected", "r1");
        });
        rejects(ErrorCode::Schema, [&] {
            prepare_contribution(journal, std::array{root, root}, directory.path / "duplicate", "selected", "r1");
        });
        rejects(ErrorCode::Schema, [&] {
            prepare_contribution(journal, {}, directory.path / "empty", "selected", "r1");
        });
        ContributionLimits limits;
        limits.objects = 2;
        rejects(ErrorCode::Budget, [&] {
            prepare_contribution(journal, roots, directory.path / "object-budget", "selected", "r1", limits);
        });
        limits = {}; limits.edges = 1;
        rejects(ErrorCode::Budget, [&] {
            prepare_contribution(journal, roots, directory.path / "edge-budget", "selected", "r1", limits);
        });
        limits = {}; limits.input_bytes = 1;
        rejects(ErrorCode::Budget, [&] {
            prepare_contribution(journal, roots, directory.path / "byte-budget", "selected", "r1", limits);
        });
        const auto repeat = prepare_contribution(journal, roots, directory.path / "repeat", "selected", "r1");
        check(repeat.snapshot.root == prepared.snapshot.root);
        check(journal.assessment(root) == revision && journal.contains(private_id));
        const auto reviewed = review_contribution(prepared.directory, prepared.snapshot.root, roots);
        check(reviewed.objects == prepared.objects && reviewed.roots == prepared.roots);
        check(reviewed.snapshot.input_bytes == prepared.snapshot.input_bytes);
        check(reviewed.observed.size() == prepared.observed.size());
        for (std::size_t i = 0; i < reviewed.observed.size(); ++i)
            check(reviewed.observed[i].object == prepared.observed[i].object
                && reviewed.observed[i].version == prepared.observed[i].version);
        rejects(ErrorCode::Schema, [&] { review_contribution(prepared.directory, prepared.snapshot.root, {}); });
        rejects(ErrorCode::Schema, [&] {
            review_contribution(prepared.directory, prepared.snapshot.root, std::array{root, root});
        });
        rejects(ErrorCode::Integrity, [&] {
            review_contribution(prepared.directory, prepared.snapshot.root, std::array{root});
        });
        limits = {}; limits.input_bytes = 1;
        rejects(ErrorCode::Budget, [&] { review_contribution(prepared.directory, prepared.snapshot.root, roots, limits); });
        const auto private_snapshot = compile_selected_snapshot(journal, std::array{private_id},
            directory.path / "private-review", "private", "1");
        rejects(ErrorCode::Permission, [&] {
            review_contribution(directory.path / "private-review", private_snapshot.root, std::array{private_id});
        });
        std::cout << "Contribution preparation passed: " << assertions << " deterministic assertions\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
