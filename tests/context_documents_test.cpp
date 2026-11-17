#include "context_documents.hpp"

#include <filesystem>
#include <iostream>
#include <source_location>
#include <sqlite3.h>
#include <stdlib.h>
#include <unistd.h>

namespace {
using namespace axiom::context;
using namespace axiom::knowledge;
std::size_t checks = 0;
void check(bool value, const std::source_location where = std::source_location::current()) {
    ++checks;
    if (!value) throw std::runtime_error("Context document assertion at " + std::to_string(where.line()));
}
template<class F> void rejects(ErrorCode code, F&& operation) {
    ++checks;
    try { operation(); }
    catch (const Error& error) { if (error.code == code) return; throw; }
    throw std::runtime_error("Expected context document rejection");
}
struct Directory {
    std::filesystem::path path;
    Directory() {
        auto pattern = (std::filesystem::temp_directory_path() / "context-documents-XXXXXX").string();
        const auto made = mkdtemp(pattern.data());
        if (!made) throw std::runtime_error("Cannot create context document directory");
        path = made;
    }
    ~Directory() { std::filesystem::remove_all(path); }
};
ContextDocument source(std::string key, std::string text, std::string session = {}) {
    ContextDocument result;
    result.project = "project";
    result.session = std::move(session);
    result.key = std::move(key);
    result.uri = "file:///project/design.txt";
    result.timestamp = "2026-11-16";
    result.content = std::move(text);
    result.kind = DocumentKind::Project;
    return result;
}
}
int main() {
    try {
        Directory directory;
        const auto path = directory.path / "context.db";
        ContextDocument original;
        {
            ContextDocuments documents(path);
            original = documents.save(source("design", "Use symbolic reasoning, never neural answers."), 0);
            check(original.revision == 1);
            check(documents.get("project", "", "design") == original);
            const auto found = documents.search("project", "session-a", "symbolic reasoning");
            check(found.complete && found.excerpts.size() == 1);
            check(found.excerpts[0].source.content.empty());
            check(found.excerpts[0].source.content_hash == original.content_hash);
            check(found.excerpts[0].source.uri == original.uri);
            check(found.excerpts[0].content == original.content && found.excerpts[0].offset == 0);
            check(documents.search("other-project", "session-a", "symbolic").excerpts.empty());
            const auto private_source = documents.save(source("private", "secret preference", "session-b"), 0);
            check(documents.search("project", "session-a", "secret").excerpts.empty());
            check(documents.search("project", "session-b", "secret").excerpts.size() == 1);
            check(documents.get("project", "session-b", "private") == private_source);
            rejects(ErrorCode::Integrity, [&] { documents.save(source("design", "Stale replacement."), 0); });
            check(documents.get("project", "", "design") == original);
            auto correction = source("design", "The updated architecture preserves the original core.");
            correction.attribution = DocumentAttribution::Derived;
            const auto updated = documents.save(correction, 1);
            check(updated.revision == 2 && updated.attribution == DocumentAttribution::Derived);
            check(documents.search("project", "session-a", "symbolic").excerpts.empty());
            check(documents.search("project", "session-a", "architecture").excerpts.size() == 1);
            check(documents.get("project", "", "design", 1) == original);
            correction.active = false;
            const auto withdrawn = documents.save(correction, 2);
            check(!withdrawn.active && withdrawn.revision == 3);
            check(documents.search("project", "session-a", "architecture").excerpts.empty());
            check(documents.get("project", "", "design", 2) == updated);
            check(documents.get("project", "", "design") == withdrawn);
            check(!documents.get("project", "", "absent"));
            documents.save(source("one", "needle first source"), 0);
            documents.save(source("two", "needle second source"), 0);
            const auto limited = documents.search("project", "session-a", "needle", 1);
            check(!limited.complete && limited.excerpts.size() == 1);
            check(limited.excerpts[0].source.key == "two");
            const auto byte_limited = documents.search("project", "session-a", "needle", 16, 1);
            check(!byte_limited.complete && byte_limited.excerpts.empty());
            // User search syntax is a literal phrase, not an FTS expression.
            check(documents.search("project", "session-a", "needle OR secret").excerpts.empty());
            rejects(ErrorCode::Budget, [&] { documents.search("project", "session-a", "needle", 0); });
            rejects(ErrorCode::Schema, [&] { documents.search("project", "", "needle"); });
            rejects(ErrorCode::Budget, [&] { documents.save(source("large", std::string(1048577, 'x')), 0); });
            auto invalid = source("invalid", "valid text");
            invalid.kind = static_cast<DocumentKind>(9);
            rejects(ErrorCode::Schema, [&] { documents.save(invalid, 0); });
            const auto before = documents.get("project", "", "one");
            ContextDocuments bounded(path, {.database = {.sql_instructions = 1}});
            rejects(ErrorCode::Budget, [&] { bounded.save(source("one", "budget rollback"), 1); });
            check(documents.get("project", "", "one") == before);
        }
        {
            ContextDocuments reopened(path);
            check(reopened.get("project", "", "design", 1) == original);
            check(reopened.search("project", "session-a", "needle").excerpts.size() == 2);
            ConversationMemory conversation(directory.path / "conversation.db");
            rejects(ErrorCode::Schema, [&] { ContextDocuments wrong(directory.path / "conversation.db"); });
            rejects(ErrorCode::Schema, [&] { ConversationMemory wrong(path); });
        }
        {
            ContextDocuments chunks(directory.path / "chunks.db", {.chunk_bytes = 7});
            const auto saved = chunks.save(source("utf8", "aaa \xc3\xa9\xc3\xa9\xc3\xa9 needle"), 0);
            const auto page = chunks.search("project", "session-a", "needle");
            check(page.complete && page.excerpts.size() == 1);
            const auto& item = page.excerpts[0];
            check(saved.content.substr(item.offset, item.content.size()) == item.content);
            validate_text(item.content);
            check(chunks.get("project", "", "utf8") == saved);
        }
        sqlite3* raw = nullptr;
        check(sqlite3_open(path.c_str(), &raw) == SQLITE_OK);
        check(sqlite3_exec(raw, "UPDATE documents SET content='changed'", nullptr, nullptr, nullptr) != SQLITE_OK);
        check(sqlite3_exec(raw, "DELETE FROM chunks", nullptr, nullptr, nullptr) != SQLITE_OK);
        check(sqlite3_close(raw) == SQLITE_OK);
        std::cout << "Context documents: " << checks << " checks passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
