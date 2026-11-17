#include "retrieval.hpp"

#include <set>

namespace axiom::knowledge {
namespace {
std::size_t record_bytes(const KnowledgeRecord& record) {
    auto bytes = encode_object(record.object).size();
    if (const auto& assessment = record.assessment) {
        bytes += assessment->checker.size() + assessment->reason.size()
            + assessment->checked_dependencies.size() * Digest{}.size()
            + (assessment->certificate ? Digest{}.size() : 0) + sizeof(assessment->version);
    }
    return bytes;
}

} // namespace

void validate_knowledge_query(const KnowledgeQuery& query, std::size_t limit) {
    if (!limit || limit > 256) throw Error(ErrorCode::Budget, "Invalid retrieval page limit");
    switch (query.kind) {
    case QueryKind::All: case QueryKind::RequiredBy: break;
    case QueryKind::Kind:
        if (query.object_kind < ObjectKind::Claim || query.object_kind > ObjectKind::Contradiction) {
            throw Error(ErrorCode::Schema, "Unknown retrieval object kind");
        }
        break;
    case QueryKind::Concept: case QueryKind::Text: case QueryKind::Source:
        validate_text(query.value);
        if (query.kind == QueryKind::Text && query.value.empty()) {
            throw Error(ErrorCode::Schema, "Empty retrieval text query");
        }
        if (query.kind == QueryKind::Source && query.source_version) validate_text(*query.source_version);
        break;
    case QueryKind::Related:
        if (query.relationship < Relationship::Requires || query.relationship > Relationship::AlternativeTo) {
            throw Error(ErrorCode::Schema, "Unknown retrieval relationship");
        }
        break;
    default: throw Error(ErrorCode::Schema, "Unknown retrieval query");
    }
}

std::optional<KnowledgeRecord> JournalArchive::get(const ObjectId& id) {
    auto object = journal_.get(id);
    if (!object) return std::nullopt;
    return KnowledgeRecord{std::move(*object), journal_.assessment(id)};
}

ObjectPage JournalArchive::query(const KnowledgeQuery& query, std::optional<ObjectId> after,
                                  std::size_t limit) {
    validate_knowledge_query(query, limit);
    switch (query.kind) {
    case QueryKind::All: return journal_.objects(after, limit);
    case QueryKind::Kind: return journal_.by_kind(query.object_kind, after, limit);
    case QueryKind::Concept: return journal_.by_concept(query.value, after, limit);
    case QueryKind::Text: return journal_.search(query.value, after, limit);
    case QueryKind::Source:
        return journal_.by_source_archive(query.value,
            query.source_version ? std::optional<std::string_view>(*query.source_version) : std::nullopt,
            after, limit);
    case QueryKind::Related: return journal_.related(query.target, query.relationship, after, limit);
    case QueryKind::RequiredBy: return journal_.required_by(query.target, after, limit);
    }
    throw Error(ErrorCode::Schema, "Unknown retrieval query");
}

UnifiedKnowledge::UnifiedKnowledge(std::vector<KnowledgeArchive*> archives)
    : archives_(std::move(archives)) {
    if (archives_.size() > 16) throw Error(ErrorCode::Budget, "Too many retrieval providers");
    std::set<KnowledgeArchive*> unique;
    for (const auto archive : archives_) {
        if (!archive || !unique.insert(archive).second) {
            throw Error(ErrorCode::Schema, "Null or repeated retrieval provider");
        }
    }
}

std::optional<RetrievedObject> UnifiedKnowledge::get(const ObjectId& id) {
    for (std::size_t i = 0; i < archives_.size(); ++i) {
        auto record = archives_[i]->get(id);
        if (!record) continue;
        if (identify(record->object) != id) {
            throw Error(ErrorCode::Integrity, "Retrieved object differs from requested identity");
        }
        if (const auto& assessment = record->assessment) {
            if (assessment->checked_dependencies.size() > maximum_assessment_dependencies) {
                throw Error(ErrorCode::Budget, "Retrieved verification dependency limit exceeded");
            }
            validate_text(assessment->checker);
            validate_text(assessment->reason);
            if (!assessment->version || assessment->checker.empty()
                || assessment->state < KnowledgeState::Provisional || assessment->state > KnowledgeState::Archived
                || assessment->logical < LogicalValidity::Unchecked || assessment->logical > LogicalValidity::Invalid
                || assessment->evidence < EvidenceAssessment::Unassessed
                || assessment->evidence > EvidenceAssessment::Conflicting) {
                throw Error(ErrorCode::Schema, "Invalid retrieved assessment metadata");
            }
        }
        return RetrievedObject{id, std::move(*record), i};
    }
    return std::nullopt;
}

ObjectPage UnifiedKnowledge::query(const KnowledgeQuery& query, std::optional<ObjectId> after,
                                    std::size_t limit) {
    validate_knowledge_query(query, limit);
    std::set<ObjectId> identities;
    bool complete = true;
    std::optional<ObjectId> safe_through;
    for (auto* archive : archives_) {
        const auto page = archive->query(query, after, limit);
        if (page.objects.size() > limit
            || (!page.complete && (page.objects.empty() || page.next != page.objects.back()))
            || (page.complete && page.next)) {
            throw Error(ErrorCode::Integrity, "Invalid retrieval provider page");
        }
        auto previous = after;
        for (const auto& id : page.objects) {
            if (previous && id <= *previous) {
                throw Error(ErrorCode::Integrity, "Retrieval page is not strictly increasing");
            }
            previous = id;
            identities.insert(id);
        }
        complete &= page.complete;
        if (!page.complete && (!safe_through || page.objects.back() < *safe_through)) {
            // A short backend page may hide identities before another
            // backend's tail. Never advance the merged cursor past that gap.
            safe_through = page.objects.back();
        }
    }
    ObjectPage result;
    result.complete = complete && identities.size() <= limit;
    for (const auto& id : identities) {
        if (result.objects.size() == limit) break;
        if (safe_through && id > *safe_through) break;
        result.objects.push_back(id);
    }
    if (!result.complete) result.next = result.objects.back();
    return result;
}

DependencyResolution UnifiedKnowledge::resolve(const ObjectId& root, ResolutionLimits limits) {
    if (!limits.objects || limits.objects > maximum_items || !limits.encoded_bytes
        || limits.encoded_bytes > 64 * 1024 * 1024 || !limits.edges || limits.edges > 65536
        || limits.depth > maximum_items) {
        throw Error(ErrorCode::Budget, "Invalid dependency resolution limits");
    }
    DependencyResolution result;
    std::vector<std::pair<ObjectId, std::size_t>> pending{{root, 0}};
    std::set<ObjectId> seen{root};
    std::size_t used = 0, edges = 0;
    for (std::size_t position = 0; position < pending.size(); ++position) {
        const auto [id, depth] = pending[position];
        auto fetched = get(id);
        if (!fetched) {
            result.unavailable.push_back(id);
            continue;
        }
        const auto bytes = record_bytes(fetched->record);
        if (bytes > limits.encoded_bytes - used) {
            throw Error(ErrorCode::Budget, "Dependency object byte budget exceeded");
        }
        used += bytes;
        auto dependencies = prerequisites(fetched->record.object);
        if (const auto& assessment = fetched->record.assessment) {
            dependencies.insert(dependencies.end(), assessment->checked_dependencies.begin(),
                                 assessment->checked_dependencies.end());
            if (assessment->certificate && *assessment->certificate != id) {
                dependencies.push_back(*assessment->certificate);
            }
        }
        if (dependencies.size() > limits.edges - edges) {
            throw Error(ErrorCode::Budget, "Dependency edge budget exceeded");
        }
        edges += dependencies.size();
        for (const auto& dependency : dependencies) {
            if (seen.contains(dependency)) continue;
            if (depth == limits.depth || seen.size() == limits.objects) {
                throw Error(ErrorCode::Budget, "Dependency depth or object budget exceeded");
            }
            seen.insert(dependency);
            pending.emplace_back(dependency, depth + 1);
        }
        result.objects.push_back(std::move(*fetched));
    }
    return result;
}

KnowledgeStream::KnowledgeStream(UnifiedKnowledge& knowledge, KnowledgeQuery query, StreamLimits limits)
    : knowledge_(knowledge), query_(std::move(query)), limits_(limits) {
    validate_knowledge_query(query_, limits_.page_size);
    if (!limits_.pages || limits_.pages > 1000000 || !limits_.items || limits_.items > 1000000
        || !limits_.content_bytes || limits_.content_bytes > 1024 * 1024 * 1024) {
        throw Error(ErrorCode::Budget, "Invalid knowledge stream limits");
    }
}

std::optional<StreamItem> KnowledgeStream::next() {
    if (failed_) throw Error(ErrorCode::Unavailable, "Knowledge stream previously failed");
    if (ended_) return std::nullopt;
    try {
        if (position_ == page_.size()) {
            if (last_page_) {
                ended_ = true;
                return std::nullopt;
            }
            if (stats_.pages == limits_.pages) {
                throw Error(ErrorCode::Budget, "Knowledge stream page budget exceeded");
            }
            auto page = knowledge_.query(query_, after_, limits_.page_size);
            ++stats_.pages;
            page_ = std::move(page.objects);
            position_ = 0;
            after_ = page.next;
            last_page_ = page.complete;
            if (page_.empty()) {
                ended_ = true;
                return std::nullopt;
            }
        }
        if (stats_.items == limits_.items) {
            throw Error(ErrorCode::Budget, "Knowledge stream item budget exceeded");
        }
        const auto id = page_[position_];
        auto object = knowledge_.get(id);
        const auto bytes = Digest{}.size() + (object ? record_bytes(object->record) : 0);
        if (bytes > limits_.content_bytes - stats_.content_bytes) {
            throw Error(ErrorCode::Budget, "Knowledge stream content budget exceeded");
        }
        stats_.content_bytes += bytes;
        ++stats_.items;
        ++position_;
        return StreamItem{id, std::move(object)};
    } catch (...) {
        failed_ = true;
        throw;
    }
}

} // namespace axiom::knowledge
