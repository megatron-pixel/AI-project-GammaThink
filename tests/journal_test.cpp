#include "journal.hpp"

#include <algorithm>
#include <stdlib.h>
#include <iostream>
#include <sqlite3.h>
#include <unistd.h>

namespace {
using namespace axiom::knowledge;
std::size_t checks = 0;

void check(bool condition) {
    ++checks;
    if (!condition) {
        throw std::runtime_error("Journal assertion " + std::to_string(checks));
    }
}

template<class F> void rejects(ErrorCode expected, F&& operation) {
    ++checks;
    try {
        operation();
    } catch (const Error& error) {
        if (error.code == expected) {
            return;
        }
        throw;
    }
    throw std::runtime_error("Expected journal rejection " + std::to_string(checks));
}

struct Directory {
    std::filesystem::path path;
    Directory() {
        auto pattern = (std::filesystem::temp_directory_path() / "gammathink-journal-XXXXXX").string();
        std::vector<char> name(pattern.begin(), pattern.end());
        name.push_back(0);
        const auto* created = mkdtemp(name.data());
        if (!created) {
            throw std::runtime_error("Cannot create journal test directory");
        }
        path = created;
    }
    ~Directory() { std::filesystem::remove_all(path); }
};

ObjectId external(std::string_view name) {
    return {digest({reinterpret_cast<const std::uint8_t*>(name.data()), name.size()})};
}

Object object(std::string label) {
    Object result;
    result.label = label;
    result.format = "text/plain; charset=utf-8";
    result.payload.assign(label.begin(), label.end());
    result.concepts = {"shared", "shared"};
    result.sources = {{"archive-a", "v1", "A/example", "synthetic-v1",
                       external(label).digest, 0, label.size()}};
    return result;
}

Assessment checked(std::vector<ObjectId> dependencies = {}) {
    Assessment result;
    result.state = KnowledgeState::Verified;
    result.logical = LogicalValidity::Valid;
    result.evidence = EvidenceAssessment::Unassessed;
    result.checker = "synthetic-checker-v1";
    result.checked_dependencies = std::move(dependencies);
    return result;
}

void raw_sql(const std::filesystem::path& path, const std::string& sql) {
    sqlite3* raw = nullptr;
    if (sqlite3_open(path.c_str(), &raw) != SQLITE_OK) {
        if (raw) {
            sqlite3_close(raw);
        }
        throw std::runtime_error("Cannot open test corruption connection");
    }
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> connection(raw, sqlite3_close);
    if (sqlite3_exec(raw, sql.c_str(), nullptr, nullptr, nullptr) != SQLITE_OK) {
        throw std::runtime_error(sqlite3_errmsg(raw));
    }
}

void persistence_and_queries() {
    Directory directory;
    const auto path = directory.path / "journal.sqlite";
    auto first = object("alpha beta");
    const auto first_id = identify(first);
    auto second = object("alpha something beta");
    second.concepts.push_back("' OR 1=1 --");
    second.sources.push_back({"archive-b", "v2", "B/example", "synthetic-v1",
                              external("other source").digest, 0, 12});
    const auto second_id = identify(second);
    {
        Journal journal(path);
        check(!journal.contains(first_id));
        check(!journal.get(first_id));
        check(!journal.assessment(first_id));
        check(journal.put(first) == first_id);
        check(journal.put(first) == first_id);
        check(journal.put(second) == second_id);
        check(journal.get(first_id) == first);
        check(journal.get(second_id) == second);
        check(journal.contains(first_id));
        check(journal.objects().objects.size() == 2);
        check(journal.by_concept("shared").objects.size() == 2);
        check(journal.by_concept("' OR 1=1 --").objects == std::vector<ObjectId>{second_id});
        check(journal.by_concept("unknown").objects.empty());
        check(journal.by_source_archive("archive-a").objects.size() == 2);
        check(journal.by_source_archive("archive-b", "v2").objects == std::vector<ObjectId>{second_id});
        check(journal.by_source_archive("archive-b", "v1").objects.empty());
        check(journal.by_kind(ObjectKind::Claim).objects.size() == 2);
        check(journal.by_kind(ObjectKind::Hypothesis).objects.empty());
        check(journal.search("alpha beta").objects == std::vector<ObjectId>{first_id});
        check(journal.search("alpha OR beta").objects.empty());
        check(journal.search("x\" OR shared").objects.empty());
        check(journal.record_assessment(first_id, checked()).version == 1);
        auto next = checked();
        next.evidence = EvidenceAssessment::Conflicting;
        next.state = KnowledgeState::Disputed;
        check(journal.record_assessment(first_id, next).version == 2);
        check(journal.assessment(first_id)->logical == LogicalValidity::Valid);
        check(journal.assessment(first_id)->evidence == EvidenceAssessment::Conflicting);
        const auto history = journal.assessment_history(first_id, 0, 1);
        check(!history.complete && history.next == 1);
        check(history.records.front().evidence == EvidenceAssessment::Unassessed);
        const auto final = journal.assessment_history(first_id, *history.next, 1);
        check(final.complete && !final.next && final.records.front().version == 2);
        // Verification does not invent local availability for a remote object.
        const auto remote = external("remote object");
        check(journal.record_assessment(remote, checked({first_id})).version == 1);
        check(!journal.contains(remote));
        check(journal.required_by(first_id).objects == std::vector<ObjectId>{remote});
        check(journal.get(first_id) == first);
    }
    {
        Journal reopened(path);
        check(reopened.get(first_id) == first);
        check(reopened.get(second_id) == second);
        check(reopened.assessment(first_id)->version == 2);
        check(reopened.assessment_history(first_id).records.size() == 2);
        check(reopened.by_source_archive("archive-b").objects == std::vector<ObjectId>{second_id});
        check(reopened.search("alpha beta").objects == std::vector<ObjectId>{first_id});
    }
}

void pagination() {
    Directory directory;
    Journal journal(directory.path / "pages.sqlite");
    std::vector<ObjectId> expected;
    for (unsigned i = 0; i < 140; ++i) {
        expected.push_back(journal.put(object("page node " + std::to_string(i))));
    }
    std::sort(expected.begin(), expected.end());
    std::optional<ObjectId> cursor;
    std::vector<ObjectId> actual;
    std::size_t pages = 0;
    do {
        const auto result = journal.objects(cursor, 7);
        check(result.objects.size() == 7);
        check(result.complete == !result.next.has_value());
        actual.insert(actual.end(), result.objects.begin(), result.objects.end());
        cursor = result.next;
        ++pages;
    } while (cursor);
    check(pages == 20 && actual == expected);
    actual.clear();
    do {
        const auto result = journal.by_concept("shared", cursor, 9);
        actual.insert(actual.end(), result.objects.begin(), result.objects.end());
        cursor = result.next;
    } while (cursor);
    check(actual == expected);
    check(journal.objects(expected.back()).objects.empty());
    rejects(ErrorCode::Budget, [&] { journal.objects({}, 0); });
    rejects(ErrorCode::Budget, [&] { journal.objects({}, maximum_items + 1); });
    rejects(ErrorCode::Schema, [&] { journal.by_kind(static_cast<ObjectKind>(99)); });
    rejects(ErrorCode::Schema, [&] { journal.related(expected[0], static_cast<Relationship>(99)); });
    rejects(ErrorCode::Schema, [&] { journal.search(""); });
}

void hypergraph_and_invalidation() {
    Directory directory;
    Journal journal(directory.path / "graph.sqlite");
    const auto a = journal.put(object("premise a")), b = journal.put(object("premise b"));
    auto rule = object("explicit rule");
    rule.kind = ObjectKind::Rule;
    const auto rule_id = journal.put(rule);
    const auto conclusion = journal.put(object("conclusion"));
    auto derivation = object("derivation");
    derivation.kind = ObjectKind::Derivation;
    derivation.derivation = Derivation{conclusion, rule_id, {a, b}, {}, {}};
    const auto proof = journal.put(derivation);
    check(journal.required_by(a).objects == std::vector<ObjectId>{proof});
    check(journal.required_by(b).objects == std::vector<ObjectId>{proof});
    check(journal.required_by(conclusion).objects.empty());
    check(journal.related(conclusion, Relationship::Produces).objects == std::vector<ObjectId>{proof});
    check(journal.related(rule_id, Relationship::DependsOn).objects == std::vector<ObjectId>{proof});
    check(journal.related(a, Relationship::Requires).objects == std::vector<ObjectId>{proof});
    check(journal.related(b, Relationship::Requires).objects == std::vector<ObjectId>{proof});
    journal.record_assessment(proof, checked({a, b, rule_id}));
    const auto original = journal.get(proof);
    rejects(ErrorCode::Budget, [&] { journal.invalidate(a, "synthetic withdrawal", 1); });
    check(!journal.assessment(a));
    check(journal.assessment(proof)->version == 1);
    const auto changed = journal.invalidate(a, "synthetic withdrawal", 2);
    check(changed.size() == 2);
    check(journal.assessment(a)->state == KnowledgeState::Invalidated);
    const auto invalidated = journal.assessment(proof);
    check(invalidated->state == KnowledgeState::Invalidated);
    check(invalidated->logical == LogicalValidity::Valid);
    check(invalidated->evidence == EvidenceAssessment::Unassessed);
    check(invalidated->version == 2);
    check(journal.get(proof) == original);
    check(identify(*original) == proof);
    check(journal.assessment_history(proof).records.front().state == KnowledgeState::Verified);

    auto opposed = object("opposed conclusion");
    opposed.kind = ObjectKind::Contradiction;
    opposed.links = {{Relationship::Contradicts, a}, {Relationship::Contradicts, b}};
    const auto conflict = journal.put(opposed);
    check(journal.related(a, Relationship::Contradicts).objects == std::vector<ObjectId>{conflict});
    check(journal.related(b, Relationship::Contradicts).objects == std::vector<ObjectId>{conflict});
    check(journal.get(a).has_value() && journal.get(b).has_value());

    // A new certificate selects B instead of A. Historical supports are not AND premises.
    auto alternative = object("alternative conclusion");
    alternative.links = {{Relationship::Supports, a}, {Relationship::Supports, b}};
    const auto goal = journal.put(alternative);
    journal.record_assessment(goal, checked({a}));
    journal.record_assessment(goal, checked({b}));
    check(journal.related(a, Relationship::Supports).objects == std::vector<ObjectId>{goal});
    const auto old_path = journal.required_by(a).objects;
    check(std::find(old_path.begin(), old_path.end(), goal) == old_path.end());
    journal.invalidate(a, "old support withdrawn");
    check(journal.assessment(goal)->version == 2);
    journal.invalidate(b, "current support withdrawn");
    check(journal.assessment(goal)->version == 3);
    check(journal.assessment(goal)->state == KnowledgeState::Invalidated);
}

void external_dependencies_and_cycles() {
    Directory directory;
    Journal journal(directory.path / "cycles.sqlite");
    const auto missing = external("not mounted");
    const auto a = journal.put(object("cycle a")), b = journal.put(object("cycle b"));
    auto first = checked({missing, b});
    first.state = KnowledgeState::Provisional;
    first.logical = LogicalValidity::Unchecked;
    auto second = first;
    second.checked_dependencies = {a};
    journal.record_assessment(a, first);
    journal.record_assessment(b, second);
    const auto affected = journal.invalidate(missing, "dependency recheck", 2);
    check(affected.size() == 2);
    check(journal.assessment(a)->state == KnowledgeState::Invalidated);
    check(journal.assessment(b)->state == KnowledgeState::Invalidated);
    check(!journal.assessment(missing) && !journal.contains(missing));
    check(journal.assessment(a)->logical == LogicalValidity::Unchecked);
}

void guarded_revalidation() {
    Directory directory;
    const auto path = directory.path / "guarded.sqlite";
    Journal reader(path), writer(path);
    const auto dependency = reader.put(object("guarded dependency"));
    const auto target = reader.put(object("guarded result"));
    const std::vector<ObservedAssessment> absent{{dependency, 0}};
    writer.record_assessment(dependency, checked());
    rejects(ErrorCode::Unavailable, [&] {
        reader.record_revalidated_assessment(target, checked({dependency}), absent);
    });
    check(!reader.assessment(target));
    const std::vector<ObservedAssessment> initial{{dependency, 1}};
    check(reader.record_revalidated_assessment(target, checked({dependency}), initial).version == 1);
    check(reader.assessment(target)->evidence == EvidenceAssessment::Unassessed);
    const auto invalidated = writer.invalidate(dependency, "changed while checking");
    check(std::find(invalidated.begin(), invalidated.end(), target) != invalidated.end());
    const auto invalidated_version = reader.assessment(target)->version;
    rejects(ErrorCode::Unavailable, [&] {
        reader.record_revalidated_assessment(target, checked({dependency}), initial);
    });
    check(reader.assessment(target)->version == invalidated_version);
    check(reader.assessment(target)->state == KnowledgeState::Invalidated);
    rejects(ErrorCode::Schema, [&] {
        reader.record_revalidated_assessment(target, checked({dependency}), {});
    });
    const std::vector<ObservedAssessment> duplicate{{dependency, 2}, {dependency, 2}};
    rejects(ErrorCode::Schema, [&] {
        reader.record_revalidated_assessment(target, checked({dependency}), duplicate);
    });
    const std::vector<ObservedAssessment> oversized_version{{dependency, UINT64_MAX}};
    rejects(ErrorCode::Schema, [&] {
        reader.record_revalidated_assessment(target, checked({dependency}), oversized_version);
    });
    const auto remote = external("remote checked elsewhere");
    const auto remote_result = reader.put(object("remote dependent"));
    const std::vector<ObservedAssessment> remote_unassessed{{remote, 0}};
    check(reader.record_revalidated_assessment(remote_result, checked({remote}),
                                               remote_unassessed).version == 1);
    check(!reader.contains(remote));
    writer.record_assessment(remote, checked());
    rejects(ErrorCode::Unavailable, [&] {
        reader.record_revalidated_assessment(remote_result, checked({remote}), remote_unassessed);
    });
    const auto before_budget = reader.assessment(target);
    Journal limited(path, {64, 1});
    const std::vector<ObservedAssessment> current{{dependency, reader.assessment(dependency)->version}};
    rejects(ErrorCode::Budget, [&] {
        limited.record_revalidated_assessment(target, checked({dependency}), current);
    });
    check(reader.assessment(target) == before_budget);
    const auto certificate = reader.put(object("independent certificate"));
    auto with_certificate = checked();
    with_certificate.certificate = certificate;
    rejects(ErrorCode::Schema, [&] {
        reader.record_revalidated_assessment(target, with_certificate, {});
    });
    const std::vector<ObservedAssessment> certificate_version{{certificate, 0}};
    reader.record_revalidated_assessment(target, with_certificate, certificate_version);
    check(reader.required_by(certificate).objects == std::vector<ObjectId>{target});
    writer.invalidate(certificate, "certificate withdrawn");
    check(reader.assessment(target)->state == KnowledgeState::Invalidated);
    with_certificate.certificate.reset();
    reader.record_assessment(target, with_certificate);
    check(reader.required_by(certificate).objects.empty());
}

void consistent_snapshots() {
    Directory directory;
    const auto path = directory.path / "snapshot.sqlite";
    Journal reader(path), writer(path);
    const auto first = reader.put(object("snapshot before"));
    const auto original = reader.record_assessment(first, checked());
    const auto later = object("snapshot after");
    reader.read_snapshot([&] {
        // The view is pinned before this callback's first read.
        writer.put(later);
        writer.record_assessment(first, checked());
        check(reader.objects().objects == std::vector<ObjectId>{first});
        check(reader.assessment(first) == original);
        check(!reader.get(identify(later)));
        rejects(ErrorCode::Permission, [&] { reader.read_snapshot([] {}); });
        rejects(ErrorCode::Storage, [&] { reader.put(object("forbidden write")); });
        check(reader.assessment(first) == original);
    });
    check(reader.contains(identify(later)));
    check(reader.assessment(first)->version == 2);
    check(reader.put(object("write restored")) == identify(object("write restored")));
    rejects(ErrorCode::Budget, [&] {
        reader.read_snapshot([&] { throw Error(ErrorCode::Budget, "Synthetic reader failure"); });
    });
    check(reader.put(object("write after failure")) == identify(object("write after failure")));
    Journal limited(path, {64, 1});
    rejects(ErrorCode::Budget, [&] { limited.read_snapshot([] {}); });
    // Failed pinning cannot strand the other connection in a read transaction.
    check(writer.put(object("after limited reader")) == identify(object("after limited reader")));
    rejects(ErrorCode::Schema, [&] { reader.read_snapshot({}); });
}

void budgets_and_integrity() {
    Directory directory;
    const auto path = directory.path / "limits.sqlite";
    auto value = object("retained object");
    const auto id = identify(value);
    {
        Journal journal(path);
        journal.put(value);
        auto bad = checked();
        bad.version = 1;
        rejects(ErrorCode::Schema, [&] { journal.record_assessment(id, bad); });
        bad = checked();
        bad.checker.clear();
        rejects(ErrorCode::Schema, [&] { journal.record_assessment(id, bad); });
        bad = checked();
        bad.logical = static_cast<LogicalValidity>(99);
        rejects(ErrorCode::Schema, [&] { journal.record_assessment(id, bad); });
        bad = checked();
        bad.reason = std::string("x\0y", 3);
        rejects(ErrorCode::Schema, [&] { journal.record_assessment(id, bad); });
        rejects(ErrorCode::Schema, [&] { journal.assessment_history(id, UINT64_MAX); });
        auto invalid_text = value;
        invalid_text.payload = {0};
        const auto invalid_id = identify(invalid_text);
        rejects(ErrorCode::Schema, [&] { journal.put(invalid_text); });
        check(!journal.contains(invalid_id));
        auto empty = value;
        empty.payload.clear();
        journal.put(empty);
        check(journal.get(identify(empty)) == empty);
    }
    {
        Journal constrained(path, {64, 1});
        rejects(ErrorCode::Budget, [&] { constrained.get(id); });
        rejects(ErrorCode::Budget, [&] { constrained.record_assessment(id, checked()); });
        rejects(ErrorCode::Budget, [&] { constrained.put(object("must roll back")); });
    }
    {
        Journal journal(path);
        check(journal.get(id) == value);
        check(!journal.assessment(id));
        check(!journal.contains(identify(object("must roll back"))));
        check(journal.record_assessment(id, checked()).version == 1);
    }
    raw_sql(path, "UPDATE objects SET wire=x'00' WHERE id=x'" + hex(id) + "'");
    {
        Journal journal(path);
        rejects(ErrorCode::Integrity, [&] { journal.get(id); });
        rejects(ErrorCode::Integrity, [&] { journal.put(value); });
    }
    const auto unrelated = directory.path / "unrelated.sqlite";
    raw_sql(unrelated, "CREATE TABLE unrelated(value INTEGER); INSERT INTO unrelated VALUES(7)");
    rejects(ErrorCode::Schema, [&] { Journal journal(unrelated); });
    raw_sql(unrelated, "INSERT INTO unrelated VALUES(8)");
    raw_sql(path, "PRAGMA user_version=99");
    rejects(ErrorCode::Schema, [&] { Journal journal(path); });
    rejects(ErrorCode::Budget, [&] { Journal journal(directory.path / "zero.sqlite", {0, 1}); });
}
void compact_cold_copy() {
    Directory directory;
    Journal journal(directory.path / "original.db");
    const auto premise = journal.put(object("cold archive premise"));
    auto dependent = object("cold archive dependent");
    dependent.links = {{Relationship::Requires, premise}, {Relationship::Supports, premise}};
    const auto conclusion = journal.put(dependent);
    journal.record_assessment(premise, checked());
    journal.record_assessment(conclusion, checked({premise, external("unavailable archive")}));
    auto disputed = checked({premise});
    disputed.state = KnowledgeState::Disputed; disputed.evidence = EvidenceAssessment::Conflicting;
    journal.record_assessment(conclusion, disputed);
    journal.invalidate(premise, "synthetic invalidation");
    const auto original_history = journal.assessment_history(conclusion).records;
    const auto copied = journal.compact_copy(directory.path / "cold");
    check(copied.objects == 2 && copied.assessments == 5);
    check(copied.source_bytes > 0 && copied.copy_bytes > 0);
    check(copied.database == directory.path / "cold/journal.db");
    {
        Journal cold(copied.database);
        check(cold.get(conclusion) == dependent && cold.get(premise) == journal.get(premise));
        check(cold.assessment_history(conclusion).records == original_history);
        check(cold.assessment_history(premise).records == journal.assessment_history(premise).records);
        check(cold.assessment(conclusion) == journal.assessment(conclusion));
        check(cold.required_by(premise).objects == journal.required_by(premise).objects);
        check(cold.search("cold archive").objects == journal.search("cold archive").objects);
        check(cold.by_source_archive("archive-a").objects == journal.by_source_archive("archive-a").objects);
        check(cold.by_concept("shared").objects == journal.by_concept("shared").objects);
        const auto later = journal.put(object("after compact snapshot"));
        check(!cold.contains(later) && journal.contains(later));
    }
    rejects(ErrorCode::Permission, [&] { journal.compact_copy(directory.path / "cold"); });
    rejects(ErrorCode::Budget, [&] { journal.compact_copy(directory.path / "too-small", 1); });
    check(!std::filesystem::exists(directory.path / "too-small"));
    rejects(ErrorCode::Permission, [&] {
        journal.read_snapshot([&] { journal.compact_copy(directory.path / "nested"); });
    });
    check(!std::filesystem::exists(directory.path / "nested"));
    check(journal.get(conclusion) == dependent && journal.assessment_history(conclusion).records == original_history);
    raw_sql(directory.path / "original.db", "UPDATE objects SET wire=x'00' WHERE id=x'" + hex(premise) + "'");
    rejects(ErrorCode::Integrity, [&] { journal.compact_copy(directory.path / "corrupt"); });
    check(!std::filesystem::exists(directory.path / "corrupt"));
}
} // namespace

int main() {
    try {
        persistence_and_queries();
        pagination();
        hypergraph_and_invalidation();
        external_dependencies_and_cycles();
        guarded_revalidation();
        consistent_snapshots();
        budgets_and_integrity();
        compact_cold_copy();
        std::cout << "journal tests passed: " << checks << " deterministic assertions\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
