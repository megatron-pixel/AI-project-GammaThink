#pragma once

#include "journal.hpp"

namespace axiom::knowledge {

enum class QueryKind { All, Kind, Concept, Text, Source, Related, RequiredBy };
struct KnowledgeQuery {
    QueryKind kind = QueryKind::All;
    std::string value;
    std::optional<std::string> source_version;
    ObjectKind object_kind = ObjectKind::Claim;
    ObjectId target;
    Relationship relationship = Relationship::DependsOn;
};
void validate_knowledge_query(const KnowledgeQuery& query, std::size_t limit);
struct KnowledgeRecord {
    Object object;
    // Observed metadata, not an authorization to trust, execute or reuse.
    // A caller must independently recheck current verification and provenance.
    std::optional<Assessment> assessment;
};

class KnowledgeArchive {
public:
    virtual ~KnowledgeArchive() = default;
    virtual std::optional<KnowledgeRecord> get(const ObjectId& id) = 0;
    // Strictly increasing unique IDs > after, at most limit. An incomplete
    // page must be nonempty and next must equal its final returned identity.
    virtual ObjectPage query(const KnowledgeQuery& query, std::optional<ObjectId> after,
                             std::size_t limit) = 0;
};

class JournalArchive final : public KnowledgeArchive {
public:
    explicit JournalArchive(Journal& journal) : journal_(journal) {}
    std::optional<KnowledgeRecord> get(const ObjectId& id) override;
    ObjectPage query(const KnowledgeQuery& query, std::optional<ObjectId> after,
                     std::size_t limit) override;
private:
    Journal& journal_;
};

struct RetrievedObject {
    ObjectId id;
    KnowledgeRecord record;
    std::size_t archive = 0;
};
struct ResolutionLimits {
    std::size_t objects = 256;
    std::size_t encoded_bytes = 16 * 1024 * 1024;
    std::size_t edges = 4096;
    std::size_t depth = 128;
};
struct DependencyResolution {
    std::vector<RetrievedObject> objects;
    std::vector<ObjectId> unavailable;
    // Complete means available at observation time, not acyclic or proven.
    bool complete() const { return unavailable.empty(); }
};

// Read-only ordered providers, owned by the caller and used on their owning
// thread. No publication, implicit network fallback, or inference occurs.
// Mount/catalogue management and cross-provider snapshot isolation are separate.
class UnifiedKnowledge {
public:
    explicit UnifiedKnowledge(std::vector<KnowledgeArchive*> archives);
    std::optional<RetrievedObject> get(const ObjectId& id);
    ObjectPage query(const KnowledgeQuery& query, std::optional<ObjectId> after = {},
                     std::size_t limit = 64);
    // Intrinsic prerequisites plus observed checked_dependencies and non-self
    // certificates. Bounded breadth-first traversal; original objects retain
    // ordered AND premises and distinct OR alternatives. Limits throw Budget,
    // never turn a missing dependency or incomplete search into a negative fact.
    DependencyResolution resolve(const ObjectId& root, ResolutionLimits limits = {});
private:
    std::vector<KnowledgeArchive*> archives_;
};

struct StreamLimits {
    std::size_t page_size = 64;
    std::size_t pages = 64;
    std::size_t items = 4096;
    std::size_t content_bytes = 16 * 1024 * 1024;
};
struct StreamStats {
    std::size_t pages = 0, items = 0, content_bytes = 0;
};
struct StreamItem {
    ObjectId id;
    // An indexed identity can become unavailable before its object is read.
    // Preserve it explicitly; an absent record is not end-of-stream or false.
    std::optional<RetrievedObject> object;
};

// Lazy one-page pull cursor over any indexed query, including concept, text
// and graph queries. Owner/providers must outlive the stream. Live keyset
// semantics are unchanged; this is not a transaction or snapshot.
class KnowledgeStream {
public:
    KnowledgeStream(UnifiedKnowledge& knowledge, KnowledgeQuery query, StreamLimits limits = {});
    KnowledgeStream(const KnowledgeStream&) = delete;
    KnowledgeStream& operator=(const KnowledgeStream&) = delete;
    // nullopt means a completely consumed query. Budget/provider failures throw
    // and permanently fail this cursor; subsequent pulls never silently resume.
    std::optional<StreamItem> next();
    const StreamStats& stats() const { return stats_; }
private:
    UnifiedKnowledge& knowledge_;
    KnowledgeQuery query_;
    StreamLimits limits_;
    StreamStats stats_;
    std::vector<ObjectId> page_;
    std::optional<ObjectId> after_;
    std::size_t position_ = 0;
    bool last_page_ = false, ended_ = false, failed_ = false;
};

} // namespace axiom::knowledge
