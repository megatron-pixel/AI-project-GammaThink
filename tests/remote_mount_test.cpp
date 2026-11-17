#include "virtual_drive.hpp"

#include <functional>
#include <iostream>
#include <map>
#include <sqlite3.h>
#include <unistd.h>

namespace {
using namespace axiom::knowledge;
std::size_t assertions = 0;
void check(bool condition) {
    ++assertions; if (!condition) throw std::runtime_error("Remote mount assertion " + std::to_string(assertions));
}
template<class Function> void rejects(ErrorCode expected, Function function) {
    try { function(); } catch (const Error& error) { check(error.code == expected); return; }
    throw std::runtime_error("Expected remote mount rejection");
}
struct Directory {
    std::filesystem::path path;
    Directory() {
        auto pattern = (std::filesystem::temp_directory_path() / "gammathink-mount-XXXXXX").string();
        std::vector<char> value(pattern.begin(), pattern.end()); value.push_back(0);
        const auto* made = mkdtemp(value.data());
        if (!made) throw std::runtime_error("Cannot create mount fixture");
        path = made;
    }
    ~Directory() { std::filesystem::remove_all(path); }
};
struct Blocks final : BlockSource {
    std::map<BlockId, Bytes> values;
    std::function<void()> on_read;
    Bytes read(const BlockId& id, std::size_t maximum) override {
        if (on_read) { auto action = std::move(on_read); on_read = {}; action(); }
        const auto found = values.find(id);
        if (found == values.end()) throw Error(ErrorCode::Unavailable, "Missing fixture block");
        if (found->second.size() > maximum) throw Error(ErrorCode::Budget, "Fixture read cap");
        return found->second;
    }
};
void raw_sql(const std::filesystem::path& path, const std::string& sql) {
    sqlite3* raw = nullptr;
    const auto code = sqlite3_open(path.c_str(), &raw);
    std::unique_ptr<sqlite3, decltype(&sqlite3_close_v2)> database(raw, sqlite3_close_v2);
    if (code != SQLITE_OK || sqlite3_exec(raw, sql.c_str(), nullptr, nullptr, nullptr) != SQLITE_OK)
        throw std::runtime_error(sqlite3_errmsg(raw));
}
void legacy(const std::filesystem::path& path, const ArchiveVersion& archive, const std::filesystem::path& directory) {
    raw_sql(path, "PRAGMA application_id=0x474b5231;PRAGMA user_version=1;"
        "CREATE TABLE versions(name TEXT NOT NULL,revision TEXT NOT NULL,root BLOB NOT NULL CHECK(length(root)=32),"
        "PRIMARY KEY(name,revision)) STRICT, WITHOUT ROWID;"
        "CREATE TABLE mounts(alias TEXT PRIMARY KEY,generation INTEGER NOT NULL CHECK(generation>0),"
        "archive TEXT,revision TEXT,directory TEXT,FOREIGN KEY(archive,revision) REFERENCES versions(name,revision),"
        "CHECK((archive IS NULL AND revision IS NULL AND directory IS NULL) OR"
        "(archive IS NOT NULL AND revision IS NOT NULL AND directory IS NOT NULL))) STRICT, WITHOUT ROWID;"
        "INSERT INTO versions VALUES('science','r1',X'" + hex(archive.root) + "');"
        "INSERT INTO mounts VALUES('legacy',7,'science','r1','" + directory.string() + "');"
        "INSERT INTO mounts VALUES('removed',9,NULL,NULL,NULL);"
        "CREATE INDEX retained_index ON mounts(generation);"
        "CREATE TRIGGER retained_trigger AFTER UPDATE ON mounts BEGIN SELECT RAISE(ABORT,'retained');END;");
}
}
int main() {
    try {
        Directory directory; Journal data(":memory:"), overlay(":memory:");
        Object object; object.label = "first"; object.format = "text/plain";
        const auto first_id = data.put(object);
        const auto first_path = directory.path / "first";
        const auto first = compile_snapshot(data, first_path, "science", "r1");
        object.label = "second"; const auto second_id = data.put(object);
        const auto second_path = directory.path / "second";
        const auto second = compile_snapshot(data, second_path, "science", "r2");
        auto blocks = std::make_shared<Blocks>();
        for (const auto& [path,root] : std::vector<std::pair<std::filesystem::path,BlockId>>{{first_path,first.root},{second_path,second.root}}) {
            GksSnapshot snapshot(path, root);
            for (const auto& file : std::filesystem::directory_iterator(path)) if (file.path().extension() == ".gksb") {
                const auto id = parse_block_id(file.path().stem().string()); blocks->values.emplace(id, snapshot.block(id));
            }
        }
        const auto db_path = directory.path / "registry.sqlite";
        ArchiveRegistry registry(db_path), observer(db_path);
        const auto v1 = registry.register_snapshot(blocks, first.root);
        const auto v2 = registry.register_snapshot(blocks, second.root);
        check(v1 == ArchiveVersion{"science","r1",first.root});
        check(registry.register_snapshot(first_path, first.root) == v1);
        auto empty = std::make_shared<Blocks>();
        rejects(ErrorCode::Unavailable, [&] { registry.mount_remote("main","science","r1",MountBackend::RemoteBlocks,"allowed",empty,0); });
        check(registry.resolve("main").generation == 0);
        const auto mounted = registry.mount_remote("main","science","r1",MountBackend::RemoteBlocks,"allowed",blocks,0);
        check(mounted.binding->directory.empty() && mounted.binding->source == "allowed"
            && mounted.binding->backend == MountBackend::RemoteBlocks);
        check(observer.resolve("main") == mounted);
        DriveAccess access;
        std::size_t factory_calls = 0;
        access.blocks = [blocks,&factory_calls](std::string_view key) -> std::shared_ptr<BlockSource> {
            ++factory_calls; return key == "allowed" ? blocks : nullptr;
        };
        VirtualDrive denied(overlay, registry);
        rejects(ErrorCode::Permission, [&] { denied.open_view(); });
        check(factory_calls == 0);
        VirtualDrive drive(overlay, registry, {}, access);
        auto old = drive.open_view(); auto simultaneous = drive.open_view();
        check(old->knowledge().get(first_id).has_value() && simultaneous->knowledge().get(first_id).has_value());
        check(!old->knowledge().get(second_id));
        const auto swapped = observer.mount_remote("main","science","r2",MountBackend::RemoteBlocks,"allowed",blocks,1);
        check(swapped.generation == 2 && swapped.binding->archive == v2);
        auto current = drive.open_view();
        check(current->knowledge().get(second_id).has_value() && !old->knowledge().get(second_id));
        rejects(ErrorCode::Unavailable, [&] { registry.mount_remote("main","science","r1",MountBackend::RemoteBlocks,"allowed",blocks,1); });
        ArchiveRegistry reopened(db_path);
        check(reopened.resolve("main") == swapped);
        VirtualDrive again(overlay, reopened, {}, access);
        check(again.open_view()->knowledge().get(second_id).has_value());
        check(registry.unmount("main",2).generation == 3);
        check(!drive.open_view()->knowledge().get(first_id));
        check(old->knowledge().get(first_id).has_value() && current->knowledge().get(second_id).has_value());
        check(registry.lookup("science","r1") == v1);
        rejects(ErrorCode::Schema, [&] { registry.mount_remote("main","science","r1",MountBackend::Local,"allowed",blocks,3); });
        rejects(ErrorCode::Schema, [&] { registry.mount_remote("main","science","r1",MountBackend::RemoteBlocks,"",blocks,3); });
        const auto unknown = registry.mount_remote("main","science","r1",MountBackend::RemoteBlocks,"unapproved",blocks,3);
        rejects(ErrorCode::Permission, [&] { drive.open_view(); });
        check(registry.resolve("main") == unknown);
        registry.mount("main","science","r1",first_path,4);
        check(denied.open_view()->knowledge().get(first_id).has_value()); // Local path needs no remote authority.
        raw_sql(db_path, "CREATE TRIGGER deny_change AFTER UPDATE ON mounts BEGIN SELECT RAISE(ABORT,'fixture');END");
        rejects(ErrorCode::Storage, [&] { registry.mount_remote("main","science","r2",MountBackend::RemoteBlocks,"allowed",blocks,5); });
        check(registry.resolve("main").generation == 5 && registry.resolve("main").binding->backend == MountBackend::Local);
        raw_sql(db_path, "DROP TRIGGER deny_change");
        // A writer commits while admission is reading the root. This proves
        // admission holds no writer lock and bind rechecks the generation.
        blocks->on_read = [&] { observer.mount("race","science","r1",first_path,0); };
        rejects(ErrorCode::Unavailable, [&] {
            registry.mount_remote("race","science","r2",MountBackend::RemoteBlocks,"allowed",blocks,0);
        });
        check(observer.resolve("race").generation == 1 && observer.resolve("race").binding->backend == MountBackend::Local);
        RegistryLimits limited; limited.active_mounts = 2;
        ArchiveRegistry capped(db_path, limited);
        rejects(ErrorCode::Budget, [&] { capped.mount_remote("third","science","r1",MountBackend::RemoteBlocks,"allowed",blocks,0); });
        check(registry.resolve("third").generation == 0);
        const auto legacy_path = directory.path / "legacy.sqlite";
        legacy(legacy_path, v1, std::filesystem::canonical(first_path));
        {
            RegistryLimits budget; budget.sql_instructions = 1;
            rejects(ErrorCode::Budget, [&] { ArchiveRegistry interrupted(legacy_path, budget); });
        }
        ArchiveRegistry migrated(legacy_path);
        check(migrated.resolve("legacy").generation == 7 && migrated.resolve("legacy").binding->archive == v1);
        check(migrated.resolve("legacy").binding->backend == MountBackend::Local);
        check(migrated.resolve("removed") == MountState{"removed",9,{}});
        rejects(ErrorCode::Storage, [&] { migrated.unmount("legacy",7); }); // Existing trigger preserved.
        raw_sql(legacy_path, "DROP TRIGGER retained_trigger");
        check(migrated.mount_remote("legacy","science","r1",MountBackend::RemoteBlocks,"allowed",blocks,7).generation == 8);
        ArchiveRegistry migrated_again(legacy_path);
        check(migrated_again.resolve("legacy").binding->source == "allowed");
        raw_sql(legacy_path, "DROP INDEX retained_index"); // Existing index preserved.
        raw_sql(legacy_path, "PRAGMA ignore_check_constraints=ON;UPDATE mounts SET backend=99 WHERE alias='legacy'");
        rejects(ErrorCode::Integrity, [&] { migrated.resolve("legacy"); });
        raw_sql(legacy_path, "UPDATE mounts SET backend=2 WHERE alias='legacy'");
        check(migrated.resolve("legacy").generation == 8);
        std::cout << "remote mount tests passed: " << assertions << " deterministic assertions\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
