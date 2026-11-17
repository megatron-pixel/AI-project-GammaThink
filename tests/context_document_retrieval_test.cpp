#include "task_frontend.hpp"

#include <filesystem>
#include <iostream>
#include <source_location>
#include <stdlib.h>
#include <unistd.h>

namespace {
using namespace axiom;
using namespace axiom::context;
using namespace axiom::knowledge;
std::size_t checks = 0;
void check(bool value, const std::source_location where = std::source_location::current()) {
    ++checks;
    if (!value) throw std::runtime_error("Document retrieval assertion at " + std::to_string(where.line()));
}
template<class F> void rejects(ErrorCode code, F&& operation) {
    ++checks;
    try { operation(); }
    catch (const Error& error) {
        if (error.code == code) return;
        throw std::runtime_error("Unexpected error after check " + std::to_string(checks) + ": " + error.what());
    }
    throw std::runtime_error("Expected document retrieval rejection");
}
struct Directory {
    std::filesystem::path path;
    Directory() {
        auto pattern = (std::filesystem::temp_directory_path() / "document-retrieval-XXXXXX").string();
        const auto made = mkdtemp(pattern.data());
        if (!made) throw std::runtime_error("Cannot create document retrieval directory");
        path = made;
    }
    ~Directory() { std::filesystem::remove_all(path); }
};
}
int main() {
    try {
        Directory directory;
        ConversationMemory memory(directory.path / "conversation.db");
        ContextDocuments documents(directory.path / "documents.db");
        memory.open_session("session", "project");
        const auto original = memory.append("session", Role::User, "1", "Review the archive requirements.");
        check(memory.project("session") == "project");
        ContextDocument source;
        source.project = "project";
        source.uri = "file:///private/design.txt";
        source.timestamp = "2026-11-16";
        source.content = "The archive preserves original source provenance.";
        for (const auto kind : {DocumentKind::External, DocumentKind::Preference,
                                DocumentKind::Project, DocumentKind::Summary}) {
            source.key = "source-" + std::to_string(static_cast<unsigned>(kind));
            source.kind = kind;
            source.attribution = kind == DocumentKind::Summary ? DocumentAttribution::Derived : DocumentAttribution::Explicit;
            documents.save(source, 0);
        }
        auto sources = retrieve_document_context(memory, documents, "session", {}, {}, "archive");
        check(sources.documents.size() == 4 && sources.documents_complete);
        const auto count = [](std::string_view text) { return text.size(); };
        const auto package = assemble_context(sources, count, {65536, 65536, 256});
        check(package.entries.size() == 5 && package.input_tokens == package.json.size());
        check(package.entries[0].layer == ContextLayer::Current);
        check(package.entries[1].layer == ContextLayer::History);
        check(package.entries[2].layer == ContextLayer::Project);
        check(package.entries[3].layer == ContextLayer::Preference);
        check(package.entries[4].layer == ContextLayer::Document);
        for (std::size_t i = 1; i < package.entries.size(); ++i) {
            const auto& entry = package.entries[i];
            check(entry.document && entry.message == 0);
            check(entry.document->source.uri == source.uri);
            check(entry.document->source.content.empty());
            check(entry.document->source_bytes == source.content.size());
        }
        const auto legacy = assemble_context(retrieve_context(memory, "session"), count);
        check(legacy.json.starts_with("{\"schema\":1,"));
        check(package.json.starts_with("{\"schema\":2,"));
        ProblemContext host;
        host.unit_registry_id = "document-test";
        host.unit_registry_version = "1";
        const auto prompt = runtime::prepare_task_prompt(sources, host, count, {262144, 262144, 256});
        check(prompt.conversation.input_tokens == prompt.input.size());
        check(prompt.context.current_question() == 0);
        const auto& views = prompt.context.sources().context().views;
        check(views.size() == 5 && views[0].basis == PremiseBasis::Question);
        for (std::size_t i = 1; i < views.size(); ++i) {
            check(views[i].basis == PremiseBasis::Source);
            check(views[i].extractor_version == "context-document-v1");
            check(views[i].archive_id.starts_with("private-context:"));
        }
        auto changed = sources;
        changed.documents[0].source.uri = "file:///different-origin";
        check(runtime::prepare_task_prompt(changed, host, count, {262144, 262144, 256}).context.snapshot() !=
            prompt.context.snapshot());
        changed = sources;
        changed.documents[0].source.session = "another-session";
        rejects(ErrorCode::Schema, [&] { assemble_context(changed, count); });
        changed = sources;
        changed.documents[0].source.project = "another-project";
        rejects(ErrorCode::Schema, [&] { assemble_context(changed, count); });
        changed = sources;
        changed.documents[0].content[0] = 'X';
        rejects(ErrorCode::Integrity, [&] { assemble_context(changed, count); });
        const auto small = assemble_context(sources, count, {600, 65536, 256});
        check(small.entries.size() == 1);
        check(small.omitted[6] == 1 && small.omitted[7] == 1 &&
              small.omitted[8] == 1 && small.omitted[9] == 1);
        check(small.input_tokens <= 600);
        auto limited = retrieve_document_context(memory, documents, "session", {}, {}, "archive",
            {.historical_messages = 1});
        check(!limited.documents_complete && limited.documents.size() == 1);
        check(!assemble_context(limited, count).documents_complete);
        source.key = "source-1";
        source.kind = DocumentKind::Project;
        source.active = false;
        documents.save(source, 1);
        const auto updated = retrieve_document_context(memory, documents, "session", {}, {}, "archive");
        check(updated.documents.size() == 3);
        check(memory.get("session", original.id) == original);
        std::cout << "Document retrieval: " << checks << " checks passed\n";
    } catch (const std::exception& error) {
        std::cerr << "After " << checks << " checks: " << error.what() << '\n';
        return 1;
    }
}
