#include "source_ingestion.hpp"
#include "virtual_drive.hpp"

#include <filesystem>
#include <iostream>
#include <source_location>
#include <stdlib.h>
#include <unistd.h>
#include <zim/writer/creator.h>
#include <zim/writer/item.h>

namespace {
using namespace axiom;
using namespace axiom::knowledge;
std::size_t checks = 0;
void check(bool condition, const std::source_location where = std::source_location::current()) {
    ++checks;
    if (!condition) throw std::runtime_error("Source ingestion assertion at " + std::to_string(where.line()));
}
template<class F> void rejects(ErrorCode expected, F&& operation) {
    ++checks;
    try { operation(); }
    catch (const Error& error) { if (error.code == expected) return; throw; }
    throw std::runtime_error("Expected source ingestion rejection");
}
struct Directory {
    std::filesystem::path path;
    Directory() {
        auto pattern = (std::filesystem::temp_directory_path() / "source-ingestion-XXXXXX").string();
        const auto made = mkdtemp(pattern.data());
        if (!made) throw std::runtime_error("Cannot create ingestion directory");
        path = made;
    }
    ~Directory() { std::filesystem::remove_all(path); }
};
}

int main(int argc, char** argv) {
    if (argc != 1 && !(argc == 3 && std::string_view(argv[1]) == "--fixture")) {
        std::cerr << "source-ingestion-test [--fixture NEW_ZIM_PATH]\n";
        return 2;
    }
    try {
        Directory directory;
        const auto path = directory.path / "source.zim";
        const std::string document = "<p>Robin flies.</p><p>Preserve the <math>x</math> markup.</p>";
        zim::writer::Creator creator;
        creator.configNbWorkers(1).configIndexing(false, "en").configClusterSize(4096);
        creator.startZimCreation(path.string());
        creator.addItem(zim::writer::StringItem::create(
            "article", "text/html", "Synthetic source", zim::writer::Hints{}, document));
        creator.setMainPath("article");
        creator.finishZimCreation();
        ZimSourceArchive archive(path);
        const auto raw = archive.read("article");
        const auto& reference = raw.sources.front();
        ProblemContext context;
        context.unit_registry_id = "ingestion";
        context.unit_registry_version = "1";
        context.views.push_back(make_evidence_view("source", PremiseBasis::Source, document,
            reference.archive_id, reference.entry_path, reference.extractor_version));
        const ProblemSourceCatalog catalog(context);
        // Supervisor fixture: schema/source admission, not measured model fidelity.
        const std::string dsl = "GAMMATHINK_DSL 1\nENTITY \"robin\"\nPREDICATE [\"flies\",1]\n"
            "RULE [0,[0,false,[[\"c\",0]]],[],[[0,3,12,null]]]\nEND\n";
        SourceTranslation translated{identify(raw), catalog.snapshot(), dsl};
        Journal staging(directory.path / "staging.db");
        const auto ingested = ingest_zim_entry(archive, "article", staging, context, translated);
        check(ingested.kind == SourceCompilationKind::Candidate && ingested.module && ingested.context);
        check(ingested.new_objects == 4 && ingested.members.size() == 1);
        check(staging.objects().objects.size() == 4);
        check(staging.get(ingested.source)->payload == raw.payload);
        check(staging.get(ingested.source)->format == "source.zim.raw.v1:text/html");
        std::size_t measured_bytes = 0;
        for (const auto& id : staging.objects().objects) {
            const auto object = staging.get(id);
            check(object && object->visibility == Visibility::Private && !staging.assessment(id));
            measured_bytes += encode_object(*object).size();
        }
        check(measured_bytes == ingested.encoded_bytes);
        const auto module = *staging.get(*ingested.module);
        check(std::string(module.payload.begin(), module.payload.end()) == dsl);
        const auto repeated = ingest_zim_entry(archive, "article", staging, context, translated);
        check(repeated.new_objects == 0 && repeated.module == ingested.module);
        check(staging.by_kind(ObjectKind::Goal).objects.empty());
        check(staging.by_kind(ObjectKind::TestResult).objects.empty());

        Journal refused(":memory:");
        auto changed = translated;
        changed.context_snapshot = "old-context";
        rejects(ErrorCode::Integrity, [&] { ingest_zim_entry(archive, "article", refused, context, changed); });
        changed = translated;
        changed.source = ObjectId{digest(Bytes{0})};
        rejects(ErrorCode::Integrity, [&] { ingest_zim_entry(archive, "article", refused, context, changed); });
        auto incomplete = context;
        incomplete.views[0].text.pop_back();
        rejects(ErrorCode::Integrity, [&] { ingest_zim_entry(archive, "article", refused, incomplete, translated); });
        auto low = SourceIngestionLimits{};
        low.storage.objects = 3;
        rejects(ErrorCode::Budget, [&] { ingest_zim_entry(archive, "article", refused, context, translated, low); });
        low = {};
        low.storage.encoded_bytes = encode_object(raw).size() - 1;
        rejects(ErrorCode::Budget, [&] { ingest_zim_entry(archive, "article", refused, context, translated, low); });
        changed = translated;
        changed.archive_dsl = "GAMMATHINK_DSL 1\nENTITY \"robin\"\nPREDICATE [\"flies\",1]\n"
            "GOAL {\"goal\":[\"atom\",[0,false,[[\"c\",0]]]],\"supports\":[[0,3,12,null]]}\nEND\n";
        rejects(ErrorCode::Schema, [&] { ingest_zim_entry(archive, "article", refused, context, changed); });
        rejects(ErrorCode::Missing, [&] { ingest_zim_entry(archive, "absent", refused, context, translated); });
        check(refused.objects().objects.empty());
        changed = translated;
        changed.archive_dsl = "GAMMATHINK_DSL 1\nUNSUPPORTED \"Not represented by this profile.\"\nEND\n";
        const auto unsupported = ingest_zim_entry(archive, "article", refused, context, changed);
        check(unsupported.kind == SourceCompilationKind::Unsupported && !unsupported.module);
        check(unsupported.new_objects == 1 && refused.objects().objects.size() == 1);
        check(refused.by_kind(ObjectKind::Rule).objects.empty());

        const auto snapshot = compile_snapshot(staging, directory.path / "snapshot", "ingested", "1");
        check(snapshot.objects == 4);
        Journal overlay(directory.path / "overlay.db");
        ArchiveRegistry registry(directory.path / "catalogue.db");
        registry.register_snapshot(directory.path / "snapshot", snapshot.root);
        registry.mount("knowledge", "ingested", "1", directory.path / "snapshot", 0);
        VirtualDrive drive(overlay, registry);
        auto view = drive.open_view();
        const auto closure = view->knowledge().resolve(ingested.members[0]);
        check(closure.complete() && closure.objects.size() == 4);
        const auto stored_member = view->knowledge().get(ingested.members[0]);
        check(stored_member.has_value());
        check(source_member_json(module, *staging.get(*ingested.context), stored_member->record.object)
            == "[0,[0,false,[[\"c\",0]]],[],[[0,3,12,null]]]");
        check(overlay.objects().objects.empty());
        check(!stored_member->record.assessment);
        if (argc == 3) std::filesystem::copy_file(path, argv[2]);
        std::cout << "source ingestion integration passed: " << checks << " deterministic assertions\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
