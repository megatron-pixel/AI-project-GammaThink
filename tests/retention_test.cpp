#include "retention.hpp"
#include "retained_source.hpp"
#include "virtual_drive.hpp"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <unistd.h>

namespace {
using namespace axiom::knowledge;
std::size_t assertions = 0;
void check(bool value) {
    ++assertions;
    if (!value) throw std::runtime_error("Retention plan assertion " + std::to_string(assertions));
}
template<class Function> void rejects(ErrorCode expected, Function function) {
    try { function(); }
    catch (const Error& error) { check(error.code == expected); return; }
    throw std::runtime_error("Expected retention plan rejection");
}
struct Directory {
    std::filesystem::path path;
    Directory() {
        auto pattern = (std::filesystem::temp_directory_path() / "gammathink-working-set-XXXXXX").string();
        std::vector<char> value(pattern.begin(), pattern.end()); value.push_back(0);
        const auto* made = mkdtemp(value.data());
        if (!made) throw std::runtime_error("Cannot create retention plan fixture");
        path = made;
    }
    ~Directory() { std::filesystem::remove_all(path); }
};
Object object(ObjectKind kind, std::string label) {
    Object value; value.kind = kind; value.label = std::move(label);
    value.format = "synthetic.working-set-retention.v1"; value.payload = {0, 1, 2};
    return value;
}
class StoredBlocks final : public BlockSource {
public:
    StoredBlocks(const std::filesystem::path& path, const BlockId& root) : snapshot_(path, root) {}
    Bytes read(const BlockId& id, std::size_t maximum) override {
        auto bytes = snapshot_.block(id);
        if (bytes.size() > maximum) throw Error(ErrorCode::Budget, "Stored fixture byte budget");
        return bytes;
    }
private:
    GksSnapshot snapshot_;
};
}
int main() {
    try {
        Directory directory;
        Journal library(directory.path / "library.db"), overlay(directory.path / "overlay.db");
        const auto first = library.put(object(ObjectKind::Claim, "first joint premise"));
        const auto second = library.put(object(ObjectKind::Claim, "second joint premise"));
        const auto assumption = library.put(object(ObjectKind::Assumption, "assumption"));
        const auto evidence = library.put(object(ObjectKind::Evidence, "evidence"));
        const auto rule = library.put(object(ObjectKind::Rule, "rule"));
        const auto conclusion = library.put(object(ObjectKind::Fact, "conclusion"));
        auto joint = object(ObjectKind::Derivation, "joint derivation");
        joint.derivation = Derivation{conclusion, rule, {first, second, first}, {assumption}, {evidence}};
        joint.sources = {{"source", "r1", "A/item", "synthetic", {}, 9007199254740993ULL, 8}};
        const auto root = library.put(joint);
        auto alternative = object(ObjectKind::Derivation, "separate alternative");
        alternative.derivation = Derivation{conclusion, rule, {second}, {}, {evidence}};
        alternative.links = {{Relationship::AlternativeTo, root}};
        const auto alternative_id = library.put(alternative);
        auto contradiction = object(ObjectKind::Contradiction, "contradictory source");
        contradiction.links = {{Relationship::Contradicts, root}, {Relationship::Supports, evidence}};
        const auto contradiction_id = library.put(contradiction);
        auto certificate = object(ObjectKind::TestResult, "observed checker record");
        certificate.links = {{Relationship::DependsOn, root}}; // Deliberate cycle.
        const auto certificate_id = library.put(certificate);
        const auto checked = library.put(object(ObjectKind::Claim, "observed checked dependency"));
        Assessment observed;
        observed.checker = "synthetic-not-a-proof"; observed.certificate = certificate_id;
        observed.checked_dependencies = {checked};
        library.record_assessment(root, observed);
        const auto revision = library.record_assessment(root, observed);
        const auto snapshot_path = directory.path / "snapshot";
        const auto compiled = compile_snapshot(library, snapshot_path, "retention-test", "r1");
        GksArchive archive(snapshot_path, compiled.root);
        JournalArchive local(overlay);
        UnifiedKnowledge knowledge({&local, &archive});
        const auto optional = overlay.put(object(ObjectKind::Claim, "optional past context"));
        const std::array roots{alternative_id, contradiction_id};
        const std::array candidates{first, second, assumption, certificate_id, checked, optional};
        const auto plan = plan_retention(knowledge, overlay, roots, candidates);
        check(plan.status == RetentionStatus::Complete && plan.unavailable.empty());
        check(plan.roots == std::vector<ObjectId>(roots.begin(), roots.end()));
        check(plan.retained.size() == 11 && plan.lookups == 11);
        check(plan.optional_omissions == std::vector<ObjectId>{optional});
        check(plan.archives == std::vector<std::size_t>{1});
        std::uint64_t bytes = 0;
        for (const auto& record : plan.retained) {
            check(record.archive == 1 && record.record.object == *library.get(record.id));
            bytes += snapshot_record_size(record.record);
        }
        check(plan.record_bytes == bytes);
        const auto found = std::find_if(plan.retained.begin(), plan.retained.end(),
            [&](const auto& value) { return value.id == root; });
        check(found != plan.retained.end() && found->record.object.derivation->premises ==
            std::vector<ObjectId>({first, second, first}));
        check(found->record.object.sources.front().offset == 9007199254740993ULL);
        check(found->record.assessment == revision && !overlay.assessment(root));
        check(overlay.get(optional).has_value() && library.get(root) == joint);
        {
            auto source = std::make_shared<StoredBlocks>(snapshot_path, compiled.root);
            BlockCacheLimits cache_limits;
            cache_limits.ram_bytes = 65536; cache_limits.ssd_bytes = 1024 * 1024; cache_limits.ssd_entries = 128;
            auto cache = std::make_shared<BlockCache>(directory.path / "active-view-cache", source, cache_limits);
            std::size_t held = 0;
            {
                auto retained_source = std::make_shared<RetainedBlockSource>(cache);
                GksArchive retained_archive(retained_source, compiled.root);
                UnifiedKnowledge retained_knowledge({&local, &retained_archive});
                const auto retained_plan = plan_retention(retained_knowledge, overlay, roots, candidates);
                check(retained_plan.status == RetentionStatus::Complete && retained_plan.retained.size() == 11);
                check(retained_plan.optional_omissions == plan.optional_omissions);
                held = cache->stats().leased_blocks;
                check(held > 0 && cache->stats().ssd_bytes < compiled.output_bytes); // No full archive prefetch.
                check(cache->stats().source_reads == held && cache->pinned().empty());
                GksSnapshot snapshot(snapshot_path, compiled.root);
                const auto unused_index = snapshot.manifest().indexes.at(1).block;
                cache->read(unused_index, 16 + gks_max_block_bytes + 65536);
                CachePruneLimits apply; apply.apply = true;
                check(cache->prune({}, apply).evicted == 1);
                check(cache->stats().leased_blocks == held);
                retained_source.reset(); // Archive's shared owner still protects it.
                check(cache->stats().leased_blocks == held && retained_archive.get(root).has_value());
            }
            check(cache->stats().leased_blocks == 0);
            CachePruneLimits apply; apply.apply = true;
            check(cache->prune({}, apply).evicted == held);
            check(library.get(root) == joint && overlay.get(optional).has_value());
        }
        for (unsigned budget = 0; budget < 4; ++budget) {
            RetentionLimits limits;
            if (budget == 0) limits.objects = 3;
            if (budget == 1) limits.edges = 1;
            if (budget == 2) limits.depth = 0;
            if (budget == 3) limits.record_bytes = 1;
            const auto limited = plan_retention(knowledge, overlay, roots, candidates, limits);
            check(limited.status == RetentionStatus::ResourceLimited && limited.optional_omissions.empty());
            check(limited.lookups <= limits.objects);
        }
        rejects(ErrorCode::Schema, [&] {
            plan_retention(knowledge, overlay, std::array{root, root}, candidates);
        });
        rejects(ErrorCode::Schema, [&] {
            plan_retention(knowledge, overlay, roots, std::array{optional, optional});
        });
        rejects(ErrorCode::Budget, [&] { plan_retention(knowledge, overlay, {}, candidates); });
        rejects(ErrorCode::Permission, [&] {
            overlay.read_snapshot([&] { plan_retention(knowledge, overlay, roots, candidates); });
        });
        auto missing = object(ObjectKind::Claim, "unavailable source");
        missing.links = {{Relationship::Requires, ObjectId{}}};
        const auto missing_id = overlay.put(missing);
        const auto unavailable = plan_retention(knowledge, overlay, std::array{missing_id}, candidates);
        check(unavailable.status == RetentionStatus::DependenciesUnavailable
            && unavailable.unavailable == std::vector<ObjectId>{ObjectId{}});
        check(unavailable.optional_omissions.empty() && unavailable.retained.size() == 1);
        check(unavailable.archives == std::vector<std::size_t>{0});
        check(!overlay.assessment(missing_id) && overlay.get(optional).has_value());
        // Corruption must escape as Integrity, not authorize forgetting.
        GksSnapshot snapshot(snapshot_path, compiled.root);
        const auto& index = snapshot.manifest().indexes.front();
        const auto leaf = decode_index_chunk(index.block, snapshot.block(index.block));
        check(leaf.level == 0 && !leaf.entries.empty());
        const auto path = snapshot_path / (hex(leaf.entries.front().block) + ".gksb");
        std::filesystem::permissions(path, std::filesystem::perms::owner_write, std::filesystem::perm_options::add);
        {
            std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
            char first_byte = 0; file.read(&first_byte, 1); first_byte ^= 1;
            file.seekp(0); file.write(&first_byte, 1); check(bool(file));
        }
        rejects(ErrorCode::Integrity, [&] { plan_retention(knowledge, overlay, roots, candidates); });
        check(overlay.put(object(ObjectKind::Claim, "after failed snapshot")) != optional);
        std::cout << "Working-set retention passed: " << assertions << " deterministic assertions\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
