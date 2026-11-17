#include "gks_snapshot.hpp"

#include <json-c/json.h>
#include <iostream>

namespace {
using namespace axiom::knowledge;
Object item(ObjectKind kind, std::string label) {
    Object object;
    object.kind = kind;
    object.label = std::move(label);
    object.format = "synthetic.viewer-fixture.v1";
    object.payload = {1};
    return object;
}
}

int main(int argc, char** argv) {
    using namespace axiom::knowledge;
    try {
        if (argc != 2) throw Error(ErrorCode::Schema, "Expected a NEW viewer-fixture directory");
        const auto directory = std::filesystem::absolute(argv[1]);
        if (!std::filesystem::create_directory(directory))
            throw Error(ErrorCode::Storage, "Fixture directory already exists");
        std::filesystem::permissions(directory, std::filesystem::perms::owner_all);
        Journal journal(directory / "journal.db");
        std::vector<ObjectId> premises;
        for (unsigned i = 0; i < 128; ++i)
            premises.push_back(journal.put(item(ObjectKind::Claim, "Synthetic premise " + std::to_string(i))));
        premises.push_back(premises[0]);
        premises.push_back(premises[1]);
        const auto conclusion = journal.put(item(ObjectKind::Fact, "Synthetic conclusion"));
        const auto rule = journal.put(item(ObjectKind::Rule, "Synthetic rule"));
        const auto assumption = journal.put(item(ObjectKind::Assumption, "Synthetic conditional assumption"));
        const auto unavailable = ObjectId{digest(Bytes{42})};
        auto evidence = item(ObjectKind::Evidence, "Synthetic <img src=x onerror=\"window.__injected=true\">");
        evidence.format = "text/plain; charset=utf-8";
        std::string raw(4095, 'a');
        raw += "\xF0\x9F\xA7\xAA"; // Four-byte UTF-8 at the text-chunk boundary.
        raw += "\n<script>window.__injected=true</script>\n";
        raw += "\xE7\x9F\xA5\xE8\xAD\x98\xE6\x9D\xA5\xE6\xBA\x90\n";
        raw += std::string(5000, 'b');
        evidence.payload = Bytes(raw.begin(), raw.end());
        evidence.sources.push_back({"synthetic-only", "1", "/" + std::string(512, 'p'),
            "fixture-not-certified", digest(evidence.payload), 9007199254740993ULL, 12});
        const auto source = journal.put(evidence);
        auto derivation = item(ObjectKind::Derivation, "Synthetic joint derivation - UI fixture only");
        derivation.derivation = Derivation{conclusion, rule, premises, {assumption, unavailable}, {source}};
        const auto root = journal.put(derivation);
        auto alternate = item(ObjectKind::Derivation, "Synthetic alternative - not a joint merge");
        alternate.derivation = Derivation{conclusion, rule, {premises[2]}, {}, {source}};
        alternate.links.push_back({Relationship::AlternativeTo, root});
        const auto alternative = journal.put(alternate);
        auto conflict = item(ObjectKind::Contradiction, "Synthetic conflicting evidence");
        conflict.links = {{Relationship::Contradicts, root}, {Relationship::Contradicts, conclusion}};
        const auto contradiction = journal.put(conflict);
        Assessment observed;
        observed.state = KnowledgeState::Disputed;
        observed.logical = LogicalValidity::Valid;
        observed.evidence = EvidenceAssessment::Conflicting;
        observed.checker = "synthetic-ui-fixture-not-a-core-checker";
        observed.reason = "Synthetic metadata for testing independent display fields; not a proof or empirical certification";
        observed.checked_dependencies = {assumption, source, unavailable};
        journal.record_assessment(root, observed);
        const auto snapshot = compile_snapshot(journal, directory / "snapshot", "synthetic-viewer-fixture", "1");
        auto manifest = std::unique_ptr<json_object, decltype(&json_object_put)>(json_object_new_object(), json_object_put);
        const auto text = [&](const char* key, std::string_view value) {
            json_object_object_add(manifest.get(), key, json_object_new_string_len(value.data(), static_cast<int>(value.size())));
        };
        text("journal", (directory / "journal.db").string());
        text("snapshot_directory", (directory / "snapshot").string());
        text("snapshot_root", hex(snapshot.root));
        text("root", hex(root));
        text("source", hex(source));
        text("unavailable", hex(unavailable));
        text("alternative", hex(alternative));
        text("contradiction", hex(contradiction));
        json_object_object_add(manifest.get(), "premise_occurrences", json_object_new_int(static_cast<int>(premises.size())));
        json_object_object_add(manifest.get(), "synthetic_only", json_object_new_boolean(true));
        json_object_object_add(manifest.get(), "core_executions", json_object_new_int(0));
        std::cout << json_object_to_json_string_ext(manifest.get(), JSON_C_TO_STRING_PLAIN) << '\n';
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
