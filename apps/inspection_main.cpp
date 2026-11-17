#include "inspection.hpp"
#include "virtual_drive.hpp"

#include <iostream>

int main(int argc, char** argv) {
    using namespace axiom::knowledge;
    try {
        std::unique_ptr<GksArchive> archive;
        if (argc >= 6 && std::string_view(argv[argc - 3]) == "--snapshot") {
            archive = std::make_unique<GksArchive>(argv[argc - 2], BlockId{parse_id(argv[argc - 1]).digest});
            argc -= 3;
        }
        if (argc != 3 && argc != 5 && argc != 6)
            throw Error(ErrorCode::Schema,
                "Usage: knowledge-inspect JOURNAL_DB OBJECT_ID | --search TEXT AFTER_OR_DASH | "
                "--kind KIND_OR_ALL AFTER_OR_DASH | --related OBJECT_ID RELATIONSHIP AFTER_OR_DASH "
                "[--snapshot DIRECTORY ROOT_HASH]");
        if (!std::filesystem::is_regular_file(argv[1]))
            throw Error(ErrorCode::Missing, "Inspection requires an existing journal");
        Journal journal(argv[1]);
        JournalArchive local(journal);
        std::vector<KnowledgeArchive*> sources{&local};
        if (archive) sources.push_back(archive.get());
        UnifiedKnowledge knowledge(std::move(sources));
        std::string output;
        journal.read_snapshot([&] {
        if (argc == 3) output = inspect_knowledge(knowledge, parse_id(argv[2]));
        else {
            KnowledgeQuery query;
            if (argc == 5 && std::string_view(argv[2]) == "--search") {
                query.kind = QueryKind::Text;
                query.value = argv[3];
            } else if (argc == 5 && std::string_view(argv[2]) == "--kind") {
                query.kind = QueryKind::All;
                if (std::string_view(argv[3]) != "ALL") {
                    bool matched = false;
                    for (unsigned i = 1; i <= 12; ++i) {
                        const auto kind = static_cast<ObjectKind>(i);
                        if (name(kind) == argv[3]) { query.kind = QueryKind::Kind; query.object_kind = kind; matched = true; break; }
                    }
                    if (!matched) throw Error(ErrorCode::Schema, "Unknown inspection object kind");
                }
            } else if (argc == 6 && std::string_view(argv[2]) == "--related") {
                query.kind = QueryKind::Related;
                query.target = parse_id(argv[3]);
                bool matched = false;
                for (unsigned i = 1; i <= 8; ++i) {
                    const auto relationship = static_cast<Relationship>(i);
                    if (name(relationship) == argv[4]) { query.relationship = relationship; matched = true; break; }
                }
                if (!matched) throw Error(ErrorCode::Schema, "Unknown inspection relationship");
            } else throw Error(ErrorCode::Schema, "Unknown inspection command");
            const auto cursor = std::string_view(argv[argc - 1]);
            output = inspect_index(knowledge, query, cursor == "-" ? std::optional<ObjectId>{} : parse_id(cursor));
        }
        });
        std::cout << output << '\n';
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
