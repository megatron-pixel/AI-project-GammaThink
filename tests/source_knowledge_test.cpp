#include "source_archive.hpp"
#include "source_compiler.hpp"
#include "journal.hpp"
#include "retrieval.hpp"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <stdlib.h>
#include <unistd.h>
#include <zim/writer/creator.h>
#include <zim/writer/item.h>

namespace {
struct TemporaryDirectory {
    std::filesystem::path path;
    TemporaryDirectory() {
        auto pattern = (std::filesystem::temp_directory_path() / "source-knowledge-XXXXXX").string();
        const auto created = mkdtemp(pattern.data());
        if (!created) throw std::runtime_error("Cannot create knowledge integration directory");
        path = created;
    }
    ~TemporaryDirectory() { std::filesystem::remove_all(path); }
};
}

int main() {
    using namespace axiom;
    using namespace axiom::knowledge;
    try {
        TemporaryDirectory temporary;
        const auto archive_path = temporary.path / "source.zim";
        const std::string source_text = "Robin flies.";
        zim::writer::Creator creator;
        creator.configNbWorkers(1).configIndexing(false, "en").configClusterSize(4096);
        creator.startZimCreation(archive_path.string());
        creator.addItem(zim::writer::StringItem::create(
            "article", "text/plain", "Synthetic source", zim::writer::Hints{}, source_text));
        creator.setMainPath("article");
        creator.finishZimCreation();
        ZimSourceArchive archive(archive_path);
        const auto source = archive.read("article");
        const auto& reference = source.sources.front();
        ProblemContext context;
        context.unit_registry_id = "integration";
        context.unit_registry_version = "1";
        context.views.push_back(make_evidence_view("source", PremiseBasis::Source, source_text,
            reference.archive_id, reference.entry_path, reference.extractor_version));
        // Supervisor-supplied translation fixture, not a measured model output.
        const std::string dsl = "GAMMATHINK_DSL 1\nENTITY \"robin\"\nPREDICATE [\"flies\",1]\n"
            "RULE [0,[0,false,[[\"c\",0]]],[],[[0,0,12,null]]]\nEND\n";
        const auto translation = compile_source(source, identify(source), dsl);
        const auto compiled = materialize_source(source, *translation.candidate, context);
        ObjectId member_id;
        {
            Journal journal(temporary.path / "journal.db");
            Journal source_store(temporary.path / "source-store.db");
            source_store.put(source);
            source_store.put(compiled.context);
            journal.put(compiled.module);
            if (compiled.members.size() != 1) throw std::runtime_error("Missing source rule");
            member_id = journal.put(compiled.members.front());
        }
        Journal reopened(temporary.path / "journal.db");
        Journal source_store(temporary.path / "source-store.db");
        JournalArchive local_reader(reopened), source_reader(source_store);
        UnifiedKnowledge knowledge({&local_reader, &source_reader});
        const auto member = reopened.get(member_id);
        const auto module = reopened.get(identify(compiled.module));
        const auto stored_context = source_store.get(identify(compiled.context));
        KnowledgeQuery query;
        query.kind = QueryKind::Concept;
        query.value = "robin";
        KnowledgeStream concept_stream(knowledge, query, {1});
        std::vector<ObjectId> concepts;
        while (const auto item = concept_stream.next()) {
            if (!item->object) throw std::runtime_error("Stored source concept became unavailable");
            concepts.push_back(item->id);
        }
        query.kind = QueryKind::Source;
        query.value = reference.archive_id;
        query.source_version = reference.archive_version;
        const auto closure = knowledge.resolve(member_id);
        if (!member || !module || !stored_context
            || member->kind != ObjectKind::Rule || reopened.assessment(member_id)
            || member->visibility != Visibility::Private
            || member->sources != source.sources
            || std::find(concepts.begin(), concepts.end(), member_id) == concepts.end()
            || knowledge.query(query).objects.size() != 4
            || !closure.complete() || closure.objects.size() != 4
            || source_member_json(*module, *stored_context, *member)
               != "[0,[0,false,[[\"c\",0]]],[],[[0,0,12,null]]]") {
            throw std::runtime_error("Persistent ZIM source knowledge integration failed");
        }
        const auto query_request = source_query_request(source, *module, *stored_context, context,
            Atom{0, false, {{TermKind::Constant, 0}}});
        const auto queried = execute_problem(query_request);
        if (queried.status != ProblemStatus::Checked || !verify_problem_result(query_request, queried)
            || reopened.assessment(member_id) || source_store.assessment(identify(source))) {
            throw std::runtime_error("Source-bound native query or authority separation failed");
        }
        std::cout << "source knowledge integration passed: generated ZIM, scoped query, native verification, cross-store journal restart\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
