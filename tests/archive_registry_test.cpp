#include "archive_registry.hpp"
#include "gks_snapshot.hpp"

#include <iostream>
#include <sqlite3.h>
#include <unistd.h>

namespace {
using namespace axiom::knowledge;
std::size_t assertions = 0;
void check(bool condition) {
    ++assertions;
    if (!condition) throw std::runtime_error("Archive registry assertion " + std::to_string(assertions));
}
template<class Function> void rejects(ErrorCode expected, Function function) {
    try { function(); }
    catch (const Error& error) { check(error.code == expected); return; }
    throw std::runtime_error("Expected archive registry rejection");
}
struct Directory {
    std::filesystem::path path;
    Directory() {
        auto pattern = (std::filesystem::temp_directory_path() / "gammathink-registry-XXXXXX").string();
        std::vector<char> value(pattern.begin(), pattern.end()); value.push_back(0);
        const auto* created = mkdtemp(value.data());
        if (!created) throw std::runtime_error("Cannot create registry test directory");
        path = created;
    }
    ~Directory() { std::filesystem::remove_all(path); }
};
Object object(std::string label) {
    Object result; result.label = std::move(label); result.format = "text/plain; charset=utf-8"; return result;
}
void raw_sql(const std::filesystem::path& path, const std::string& sql) {
    sqlite3* raw = nullptr;
    const auto code = sqlite3_open(path.c_str(), &raw);
    std::unique_ptr<sqlite3, decltype(&sqlite3_close_v2)> database(raw, sqlite3_close_v2);
    if (code != SQLITE_OK || sqlite3_exec(raw, sql.c_str(), nullptr, nullptr, nullptr) != SQLITE_OK)
        throw std::runtime_error(sqlite3_errmsg(raw));
}
}
int main() {
    try {
        Directory directory;
        Journal journal(":memory:");
        const auto first_id = journal.put(object("first revision"));
        const auto v1_path = directory.path / "v1", v2_path = directory.path / "v2";
        const auto v1 = compile_snapshot(journal, v1_path, "science", "r1");
        journal.put(object("second revision"));
        const auto v2 = compile_snapshot(journal, v2_path, "science", "r2");
        const auto conflicting = compile_snapshot(journal, directory.path / "conflicting", "science", "r1");
        const auto third = compile_snapshot(journal, directory.path / "third", "other", "r1");
        const auto path = directory.path / "catalogue.sqlite";
        ArchiveRegistry registry(path), observer(path);
        const auto science1 = registry.register_snapshot(v1_path, v1.root);
        const auto science2 = registry.register_snapshot(v2_path, v2.root);
        check(science1 == ArchiveVersion{"science", "r1", v1.root});
        check(registry.register_snapshot(v1_path, v1.root) == science1);
        check(observer.lookup("science", "r1") == science1);
        check(observer.lookup("science", "r2") == science2);
        check(!observer.lookup("unknown", "r1"));
        rejects(ErrorCode::Integrity, [&] {
            registry.register_snapshot(directory.path / "conflicting", conflicting.root);
        });
        check(registry.lookup("science", "r1") == science1);
        check(registry.resolve("main") == MountState{"main", 0, {}});
        rejects(ErrorCode::Missing, [&] { registry.unmount("main", 0); });
        const auto mounted1 = registry.mount("main", "science", "r1", v1_path, 0);
        check(mounted1.generation == 1 && mounted1.binding->archive == science1);
        check(observer.resolve("main") == mounted1);
        check(observer.mounts() == std::vector<MountState>{mounted1});
        GksSnapshot pinned(mounted1.binding->directory, mounted1.binding->archive.root);
        const auto mounted2 = observer.mount("main", "science", "r2", v2_path, 1);
        check(mounted2.generation == 2 && mounted2.binding->archive == science2);
        check(registry.resolve("main") == mounted2);
        rejects(ErrorCode::Unavailable, [&] { registry.mount("main", "science", "r1", v1_path, 1); });
        check(registry.resolve("main") == mounted2);
        check(pinned.manifest().revision == "r1" && pinned.get(first_id).has_value());
        const auto removed = registry.unmount("main", 2);
        check(removed == MountState{"main", 3, {}});
        check(observer.mounts().empty() && observer.resolve("main") == removed);
        check(registry.lookup("science", "r1") == science1);
        check(std::filesystem::exists(v1_path / (hex(v1.root) + ".gksb")));
        rejects(ErrorCode::Unavailable, [&] { observer.mount("main", "science", "r2", v2_path, 0); });
        rejects(ErrorCode::Missing, [&] { observer.unmount("main", 3); });
        const auto remounted = registry.mount("main", "science", "r1", v1_path, 3);
        check(remounted.generation == 4);
        ArchiveRegistry reopened(path);
        check(reopened.resolve("main") == remounted && reopened.lookup("science", "r2") == science2);
        rejects(ErrorCode::Missing, [&] { registry.mount("main", "science", "absent", v1_path, 4); });
        rejects(ErrorCode::Storage, [&] {
            registry.mount("main", "science", "r2", directory.path / "missing", 4);
        });
        rejects(ErrorCode::Unavailable, [&] { registry.mount("main", "science", "r2", v1_path, 4); });
        check(observer.resolve("main") == remounted);

        raw_sql(path, "CREATE TRIGGER deny_mount AFTER UPDATE ON mounts"
                      " BEGIN SELECT RAISE(ABORT,'synthetic mount failure'); END");
        rejects(ErrorCode::Storage, [&] { registry.mount("main", "science", "r2", v2_path, 4); });
        rejects(ErrorCode::Storage, [&] { registry.unmount("main", 4); });
        check(observer.resolve("main") == remounted);
        raw_sql(path, "DROP TRIGGER deny_mount");

        RegistryLimits limits; limits.versions = 1;
        ArchiveRegistry version_limited(path, limits);
        rejects(ErrorCode::Budget, [&] { version_limited.register_snapshot(directory.path / "third", third.root); });
        check(!registry.lookup("other", "r1"));
        limits = {}; limits.active_mounts = 1;
        ArchiveRegistry mount_limited(path, limits);
        rejects(ErrorCode::Budget, [&] { mount_limited.mount("second", "science", "r2", v2_path, 0); });
        check(registry.resolve("second").generation == 0);
        limits = {}; limits.sql_instructions = 1;
        ArchiveRegistry work_limited(path, limits);
        rejects(ErrorCode::Budget, [&] { work_limited.mount("main", "science", "r2", v2_path, 4); });
        check(observer.resolve("main") == remounted);
        const auto tombstone = registry.unmount("main", 4);
        check(tombstone.generation == 5);
        limits = {}; limits.aliases = 1;
        ArchiveRegistry alias_limited(path, limits);
        rejects(ErrorCode::Budget, [&] { alias_limited.mount("second", "science", "r1", v1_path, 0); });
        check(alias_limited.mount("main", "science", "r1", v1_path, 5).generation == 6);
        limits = {}; limits.active_mounts = 0;
        ArchiveRegistry no_active(path, limits);
        rejects(ErrorCode::Budget, [&] { no_active.mounts(); });
        const auto second_mount = registry.mount("aaa", "science", "r2", v2_path, 0);
        const auto listed = observer.mounts();
        check(listed.size() == 2 && listed.front() == second_mount && listed.back().alias == "main");
        rejects(ErrorCode::Schema, [&] { registry.resolve(""); });
        rejects(ErrorCode::Schema, [&] { registry.lookup(std::string("x\0y", 3), "r1"); });
        const std::string quoted = "' OR 1=1 --";
        check(registry.mount(quoted, "science", "r1", v1_path, 0).alias == quoted);
        check(registry.resolve("main").generation == 6);
        registry.unmount(quoted, 1);
        raw_sql(path, "UPDATE mounts SET generation=9223372036854775807 WHERE alias='main'");
        rejects(ErrorCode::Budget, [&] { registry.unmount("main", INT64_MAX); });
        rejects(ErrorCode::Budget, [&] { registry.mount("main", "science", "r1", v1_path, INT64_MAX); });
        check(registry.resolve("main").generation == INT64_MAX);

        const auto unrelated = directory.path / "unrelated.sqlite";
        raw_sql(unrelated, "CREATE TABLE retained(value INTEGER); INSERT INTO retained VALUES(7)");
        rejects(ErrorCode::Schema, [&] { ArchiveRegistry foreign(unrelated); });
        raw_sql(unrelated, "INSERT INTO retained VALUES(8)");
        raw_sql(path, "PRAGMA ignore_check_constraints=ON; UPDATE versions SET root=zeroblob(31) WHERE revision='r2'");
        rejects(ErrorCode::Integrity, [&] { registry.lookup("science", "r2"); });
        rejects(ErrorCode::Integrity, [&] { registry.mounts(); });
        raw_sql(path, "PRAGMA user_version=99");
        rejects(ErrorCode::Schema, [&] { ArchiveRegistry future(path); });
        std::cout << "archive registry tests passed: " << assertions << " deterministic assertions\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
