#include "gks_snapshot.hpp"

#include <algorithm>
#include <fcntl.h>
#include <iostream>
#include <sys/stat.h>
#include <unistd.h>

namespace {
using namespace axiom::knowledge;
std::size_t assertions = 0;
void check(bool condition) {
    ++assertions;
    if (!condition) throw std::runtime_error("GKS snapshot assertion " + std::to_string(assertions));
}
template<class Function> void rejects(ErrorCode expected, Function function) {
    try { function(); }
    catch (const Error& error) { check(error.code == expected); return; }
    throw std::runtime_error("Expected GKS snapshot rejection");
}
struct Directory {
    std::filesystem::path path;
    Directory() {
        auto pattern = (std::filesystem::temp_directory_path() / "gammathink-snapshot-XXXXXX").string();
        std::vector<char> value(pattern.begin(), pattern.end()); value.push_back(0);
        const auto* created = mkdtemp(value.data());
        if (!created) throw std::runtime_error("Cannot create snapshot test directory");
        path = created;
    }
    ~Directory() { std::filesystem::remove_all(path); }
};
Object object(std::size_t number) {
    Object value;
    value.label = "document " + std::to_string(number);
    value.format = "text/plain; charset=utf-8";
    const std::string text = "alpha beta alpha";
    value.payload.assign(text.begin(), text.end());
    value.concepts = {"thermal", "thermal"};
    value.sources = {{"test-zim", "r1", "A/Example", "synthetic-v1", {}, 0, text.size()}};
    return value;
}
std::vector<IndexEntry> entries(const GksSnapshot& snapshot, const BlockId& id, std::uint32_t level,
                              std::string_view format) {
    const auto page = decode_index_chunk(id, snapshot.block(id));
    check(page.level == level && page.key_format == format);
    if (!level) return page.entries;
    std::vector<IndexEntry> result;
    for (const auto& child : page.children) {
        auto values = entries(snapshot, child.block, level - 1, format);
        check(!values.empty() && values.front().position == child.first && values.back().position == child.last);
        result.insert(result.end(), values.begin(), values.end());
    }
    return result;
}
void corruption(const std::filesystem::path& path) {
    check(chmod(path.c_str(), 0600) == 0);
    const auto fd = open(path.c_str(), O_WRONLY);
    check(fd >= 0);
    const char byte = 0;
    const auto count = write(fd, &byte, 1);
    close(fd);
    check(count == 1);
}
}
int main() {
    try {
        Directory directory;
        Journal journal(directory.path / "journal.sqlite");
        std::vector<ObjectId> ids;
        for (std::size_t i = 0; i < 19; ++i) {
            auto value = object(i);
            if (i) value.links.push_back({Relationship::Requires, ids.front()});
            ids.push_back(journal.put(value));
        }
        Assessment assessment;
        assessment.checker = "synthetic-checker";
        assessment.state = KnowledgeState::Verified;
        assessment.logical = LogicalValidity::Valid;
        assessment.checked_dependencies = {ids.back()};
        const auto observed = journal.record_assessment(ids.front(), assessment);
        SnapshotLimits limits;
        limits.chunk_bytes = 1024; limits.chunk_records = 2;
        const auto destination = directory.path / "snapshot";
        const auto compiled = compile_snapshot(journal, destination, "test", "r1", limits);
        check(compiled.objects == ids.size() && compiled.input_bytes > 0 && compiled.output_bytes > 0);
        check(!std::filesystem::exists(destination / "index-work.sqlite"));
        GksSnapshot snapshot(destination, compiled.root);
        check(snapshot.manifest().name == "test" && snapshot.manifest().object_count == ids.size());
        check(snapshot.manifest().indexes.size() == 7 && snapshot.manifest().indexes.front().level > 0);
        for (const auto& id : ids) {
            const auto got = snapshot.get(id);
            check(got && got->object == *journal.get(id) && got->assessment == journal.assessment(id));
        }
        journal.record_assessment(ids.front(), assessment);
        journal.put(object(100));
        check(snapshot.get(ids.front())->assessment == observed);
        check(!snapshot.get(identify(object(100))));
        GksSnapshot reopened(destination, compiled.root);
        check(reopened.get(ids.front())->assessment == observed);
        std::sort(ids.begin(), ids.end());
        BlockId first_objects;
        for (const auto& root : snapshot.manifest().indexes) {
            const auto all = entries(snapshot, root.block, root.level, root.key_format);
            check(!all.empty());
            for (std::size_t i = 1; i < all.size(); ++i) check(all[i - 1].position < all[i].position);
            for (const auto& entry : all) {
                const auto chunk = decode_object_chunk(entry.block, snapshot.block(entry.block));
                check(entry.ordinal < chunk.records.size()
                    && identify(chunk.records[entry.ordinal].object) == entry.position.object);
            }
            if (root.kind == IndexKind::Objects) {
                check(all.size() == ids.size());
                for (std::size_t i = 0; i < ids.size(); ++i) check(all[i].position.object == ids[i]);
                first_objects = all.front().block;
            }
            if (root.kind == IndexKind::Concept) check(all.size() == ids.size()); // Duplicate tag is indexed once.
            if (root.kind == IndexKind::Text) {
                const Bytes token{'a','l','p','h','a',0};
                std::vector<std::uint32_t> offsets;
                for (const auto& entry : all) {
                    if (entry.position.object == ids.front() && entry.position.key.size() == 10
                        && std::equal(token.begin(), token.end(), entry.position.key.begin())) {
                        std::uint32_t offset = 0;
                        for (unsigned i = 0; i < 4; ++i) offset |= std::uint32_t(entry.position.key[6 + i]) << (8 * i);
                        offsets.push_back(offset);
                    }
                }
                std::sort(offsets.begin(), offsets.end());
                check(offsets.size() == 2 && offsets.back() - offsets.front() == 2);
            }
        }
        rejects(ErrorCode::Permission, [&] { compile_snapshot(journal, destination, "test", "r2"); });
        check(reopened.get(ids.front()).has_value());
        const auto repeat = compile_snapshot(journal, directory.path / "repeat", "test", "r2", limits);
        const auto repeat_again = compile_snapshot(journal, directory.path / "repeat-again", "test", "r2", limits);
        check(repeat.root == repeat_again.root);
        const std::array selected{identify(object(100)), identify(object(0))};
        const auto selected_path = directory.path / "selected";
        const auto subset = compile_selected_snapshot(journal, selected, selected_path, "selected", "r1", limits);
        GksSnapshot selected_snapshot(selected_path, subset.root);
        check(subset.objects == 2 && selected_snapshot.manifest().object_count == 2);
        for (const auto& id : selected) {
            const auto record = selected_snapshot.get(id);
            check(record && record->object == *journal.get(id) && record->assessment == journal.assessment(id));
        }
        check(selected_snapshot.get(identify(object(0)))->assessment->version == 2);
        const auto excluded = *std::find_if(ids.begin(), ids.end(), [&](const auto& id) {
            return id != selected[0] && id != selected[1];
        });
        check(!selected_snapshot.get(excluded));
        const auto indexed = entries(selected_snapshot, selected_snapshot.manifest().indexes.front().block,
            selected_snapshot.manifest().indexes.front().level, gks_index_formats[0]);
        check(indexed.size() == selected.size());
        const std::array reversed{selected[1], selected[0]};
        check(compile_selected_snapshot(journal, reversed, directory.path / "selected-reversed",
            "selected", "r1", limits).root == subset.root);
        const std::array duplicate{selected[0], selected[0]};
        rejects(ErrorCode::Schema, [&] {
            compile_selected_snapshot(journal, duplicate, directory.path / "selected-duplicate", "selected", "r1", limits);
        });
        check(!std::filesystem::exists(directory.path / "selected-duplicate"));
        auto selected_budget = limits;
        selected_budget.objects = 1;
        rejects(ErrorCode::Budget, [&] {
            compile_selected_snapshot(journal, selected, directory.path / "selected-budget", "selected", "r1", selected_budget);
        });
        const std::array absent{ObjectId{}};
        rejects(ErrorCode::Missing, [&] {
            compile_selected_snapshot(journal, absent, directory.path / "selected-missing", "selected", "r1", limits);
        });
        check(journal.put(object(102)) == identify(object(102)));
        const auto empty_subset = compile_selected_snapshot(journal, {}, directory.path / "selected-empty", "selected", "r1", limits);
        check(empty_subset.objects == 0 && GksSnapshot(directory.path / "selected-empty", empty_subset.root).manifest().indexes.empty());
        Journal empty(":memory:");
        const auto empty_result = compile_snapshot(empty, directory.path / "empty", "empty", "r1");
        GksSnapshot empty_snapshot(directory.path / "empty", empty_result.root);
        check(empty_snapshot.manifest().object_count == 0 && !empty_snapshot.get(ids.front()));
        std::size_t attempt = 0;
        const auto budget_failure = [&](SnapshotLimits budget) {
            rejects(ErrorCode::Budget, [&] {
                compile_snapshot(journal, directory.path / ("limited-" + std::to_string(attempt++)), "test", "r1", budget);
            });
            check(journal.put(object(101)) == identify(object(101))); // Reader rollback restores writes.
        };
        auto budget = limits; budget.objects = 0; budget_failure(budget);
        budget = limits; budget.input_bytes = 0; budget_failure(budget);
        budget = limits; budget.output_bytes = 0; budget_failure(budget);
        budget = limits; budget.index_entries = 0; budget_failure(budget);
        budget = limits; budget.sql_instructions = 1; budget_failure(budget);
        budget = limits; budget.workspace_bytes = 1; budget_failure(budget);
        Journal workspace_source(":memory:");
        auto wide = object(200);
        wide.concepts = {std::string(maximum_text_bytes, 'x')};
        const auto wide_id = workspace_source.put(wide);
        budget = limits; budget.workspace_bytes = 65536;
        rejects(ErrorCode::Budget, [&] {
            compile_snapshot(workspace_source, directory.path / "workspace-full", "test", "r1", budget);
        });
        check(workspace_source.get(wide_id) == wide);
        const auto missing_path = directory.path / "missing";
        std::filesystem::create_directory(missing_path);
        rejects(ErrorCode::Unavailable, [&] { GksSnapshot absent(missing_path, compiled.root); });
        const auto block_path = destination / (hex(first_objects) + ".gksb");
        corruption(block_path);
        rejects(ErrorCode::Integrity, [&] { reopened.get(ids.front()); });
        std::filesystem::remove(block_path);
        rejects(ErrorCode::Unavailable, [&] { reopened.get(ids.front()); });
        std::filesystem::create_symlink(directory.path / "journal.sqlite", block_path);
        rejects(ErrorCode::Storage, [&] { reopened.get(ids.front()); });
        std::cout << "GKS snapshot tests passed: " << assertions << " deterministic assertions\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
