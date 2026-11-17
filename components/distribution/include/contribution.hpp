#pragma once

#include "gks_snapshot.hpp"

namespace axiom::knowledge {

struct ContributionLimits {
    std::size_t objects = 4096, edges = 16384;
    std::uint64_t input_bytes = 16 * 1024 * 1024;
    SnapshotLimits snapshot{};
};

struct PreparedContribution {
    std::filesystem::path directory;
    SnapshotCompilation snapshot;
    // Original selected roots, distinct from the sorted dependency closure.
    std::vector<ObjectId> roots, objects;
    std::vector<ObservedAssessment> observed;
};

// A private local review artifact, NOT permission to publish it. Every selected
// object and referenced/checked dependency must be available and explicitly
// Public. Unrelated private journal contents are never compiled into this root.
// No source object, authoritative assessment, or original snapshot is changed.
PreparedContribution prepare_contribution(Journal& journal, std::span<const ObjectId> roots,
                                          const std::filesystem::path& destination,
                                          std::string name, std::string revision,
                                          ContributionLimits limits = {});
// Reopen an existing packet by an explicit expected root, without trusting a
// serialized review or authorizing publication. Reconstructs its exact public
// dependency closure; unrelated snapshot records prevent approval.
PreparedContribution review_contribution(const std::filesystem::path& directory, const BlockId& snapshot,
                                         std::span<const ObjectId> roots, ContributionLimits limits = {});

} // namespace axiom::knowledge
