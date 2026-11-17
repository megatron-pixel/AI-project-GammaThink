#pragma once

#include "retrieval.hpp"

namespace axiom::knowledge {

struct InspectionLimits {
    std::size_t objects = 96, bytes = 8 * 1024 * 1024, queries = 256;
    std::size_t frontier = 512, depth = 8, incoming_page = 16;
};

// A bounded observational export of the same objects used by the core.
// Full ordered derivation arrays survive even at an exploration boundary.
// Does not execute, assess, import, publish, or reinterpret any object.
std::string inspect_knowledge(UnifiedKnowledge& knowledge, const ObjectId& root,
                              InspectionLimits limits = {});
// Keyset navigation over the existing indexed contracts. No relevance ranking,
// guessed semantic joins, or inferred relationships.
std::string inspect_index(UnifiedKnowledge& knowledge, const KnowledgeQuery& query,
                          std::optional<ObjectId> after = {}, std::size_t limit = 16);

} // namespace axiom::knowledge
