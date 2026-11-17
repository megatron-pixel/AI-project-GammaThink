#include "source_archive.hpp"
#include "journal.hpp"

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <stdlib.h>
#include <unistd.h>
#include <zim/writer/creator.h>
#include <zim/writer/item.h>

namespace {

using namespace axiom::knowledge;
std::size_t assertions = 0;

void check(bool condition) {
    ++assertions;
    if (!condition) throw std::runtime_error("Source archive assertion " + std::to_string(assertions));
}

template<class Function> void rejects(ErrorCode expected, Function function) {
    try {
        function();
    } catch (const Error& error) {
        check(error.code == expected);
        return;
    }
    throw std::runtime_error("Expected source archive rejection");
}

struct TemporaryDirectory {
    std::filesystem::path path;
    TemporaryDirectory() {
        auto pattern = (std::filesystem::temp_directory_path() / "source-zim-XXXXXX").string();
        const auto directory = mkdtemp(pattern.data());
        if (!directory) throw std::runtime_error("Cannot create temporary source archive directory");
        path = directory;
    }
    ~TemporaryDirectory() { std::filesystem::remove_all(path); }
};

void create(const std::filesystem::path& path, const std::string& body) {
    zim::writer::Creator creator;
    creator.configNbWorkers(1).configIndexing(false, "en").configClusterSize(4096);
    creator.startZimCreation(path.string());
    creator.addItem(zim::writer::StringItem::create(
        "article", "text/html", "Original source", zim::writer::Hints{}, body));
    creator.addItem(zim::writer::StringItem::create(
        "plain", "text/plain", "Plain", zim::writer::Hints{}, "not all birds fly\n"));
    creator.addItem(zim::writer::StringItem::create(
        "binary", "image/png", "Image", zim::writer::Hints{}, "not a text source"));
    creator.addRedirection("redirect", "Alias", "article");
    creator.setMainPath("article");
    creator.finishZimCreation();
}

} // namespace

int main() {
    try {
        TemporaryDirectory temporary;
        const auto path = temporary.path / "original.zim";
        // The negation is beyond the old search extractor's 65536-byte ceiling.
        const std::string body = "<html><body><p>" + std::string(70000, 'a')
            + "</p><p>Not every material conducts.</p><math><mfrac><mi>a</mi><mi>b</mi>"
              "</mfrac></math><script>untrusted text, never instructions</script>"
              "<p>caf\xc3\xa9</p></body></html>";
        create(path, body);
        ZimSourceArchive source(path);
        const auto document = source.read("article");
        check(document.kind == ObjectKind::Evidence);
        check(document.visibility == Visibility::Private);
        check(document.format == "source.zim.raw.v1:text/html");
        check(std::string(document.payload.begin(), document.payload.end()) == body);
        check(document.sources.size() == 1);
        const auto& reference = document.sources.front();
        check(reference.offset == 0 && reference.length == body.size());
        check(reference.source_digest == digest(document.payload));
        check(reference.archive_id == source.info().id);
        check(reference.archive_version == source.info().version);
        check(reference.entry_path == "article");
        check(reference.extractor_version == "libzim-raw-entry-v1");
        check(!source.info().declared_checksum.empty());
        check(source.info().logical_bytes > 0);
        check(decode_object(identify(document), encode_object(document)) == document);
        check(source.read("redirect") == document);
        check(source.read("article", Visibility::Public).visibility == Visibility::Public);
        check(identify(source.read("article", Visibility::Public)) != identify(document));
        check(source.read("plain").format == "source.zim.raw.v1:text/plain");

        Journal journal(temporary.path / "journal.db");
        const auto id = journal.put(document);
        check(journal.get(id) == document);
        check(!journal.assessment(id));
        check(journal.by_source_archive(reference.archive_id, reference.archive_version).objects
              == std::vector<ObjectId>{id});
        check(journal.put(source.read("redirect")) == id);

        std::filesystem::copy_file(path, temporary.path / "copy.zim");
        ZimSourceArchive copy(temporary.path / "copy.zim");
        check(copy.read("article") == document);
        const auto replacement_path = temporary.path / "replacement.zim";
        create(replacement_path, "<p>A different version.</p>");
        ZimSourceArchive replacement(replacement_path);
        check(identify(replacement.read("article")) != id);
        check(source.read("article") == document);
        check(journal.get(id) == document);
        check(!journal.assessment(journal.put(replacement.read("article"))));

        ZimSourceArchive exact(path, {source.info().logical_bytes, body.size()});
        check(exact.read("article") == document);
        ZimSourceArchive small(path, {source.info().logical_bytes, body.size() - 1});
        rejects(ErrorCode::Budget, [&] { (void)small.read("article"); });
        rejects(ErrorCode::Budget, [&] {
            ZimSourceArchive too_small(path, {source.info().logical_bytes - 1, body.size()});
        });
        rejects(ErrorCode::Budget, [&] { ZimSourceArchive invalid(path, {1, 0}); });
        rejects(ErrorCode::Budget, [&] {
            ZimSourceArchive invalid(path, {1, maximum_object_bytes});
        });
        rejects(ErrorCode::Missing, [&] { (void)source.read("absent"); });
        rejects(ErrorCode::Schema, [&] { (void)source.read(""); });
        rejects(ErrorCode::Schema, [&] { (void)source.read(std::string("a\0b", 3)); });
        rejects(ErrorCode::Schema, [&] { (void)source.read("binary"); });
        rejects(ErrorCode::Schema, [&] { (void)source.read("article", Visibility(0)); });
        rejects(ErrorCode::Schema, [&] { ZimSourceArchive invalid(temporary.path / "wrong.zip"); });
        rejects(ErrorCode::Unavailable, [&] { ZimSourceArchive missing(temporary.path / "missing.zim"); });
        std::cout << "source archive tests passed: " << assertions << " deterministic assertions\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
