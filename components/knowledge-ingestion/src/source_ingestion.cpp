#include "source_ingestion.hpp"

namespace axiom::knowledge {

SourceIngestion ingest_zim_entry(const ZimSourceArchive& archive, std::string_view entry,
    Journal& journal, const ProblemContext& context, const SourceTranslation& translation,
    SourceIngestionLimits limits) {
    if (limits.storage.objects == 0 || limits.storage.objects > maximum_items ||
        limits.storage.encoded_bytes == 0 || limits.storage.encoded_bytes > 64 * 1024 * 1024) {
        throw Error(ErrorCode::Budget, "Invalid complete source ingestion budget");
    }
    auto source = archive.read(entry);
    const auto compilation = compile_source(source, translation.source, translation.archive_dsl);
    const auto& reference = source.sources.front();
    if (context.views.size() != 1) {
        throw Error(ErrorCode::Schema, "Ingestion requires one complete host source view");
    }
    const auto& view = context.views.front();
    if (view.basis != PremiseBasis::Source || view.archive_id != reference.archive_id ||
        view.entry_path != reference.entry_path || view.extractor_version != reference.extractor_version ||
        view.text != std::string(source.payload.begin(), source.payload.end()) ||
        view.digest != evidence_digest(view.text)) {
        throw Error(ErrorCode::Integrity, "Ingestion context differs from the captured complete source");
    }
    const ProblemSourceCatalog catalog(context, limits.admission);
    if (catalog.snapshot() != translation.context_snapshot) {
        throw Error(ErrorCode::Integrity, "Source translation host snapshot changed");
    }
    SourceIngestion result;
    result.source = identify(source);
    result.kind = compilation.kind;
    result.reason = compilation.reason;
    result.encoded_bytes = encode_object(source).size();
    if (result.encoded_bytes > limits.storage.encoded_bytes) {
        throw Error(ErrorCode::Budget, "Complete raw source exceeds ingestion byte budget");
    }
    std::vector<Object> records;
    records.push_back(std::move(source));
    if (compilation.kind == SourceCompilationKind::Candidate) {
        if (limits.storage.objects < 3) throw Error(ErrorCode::Budget, "Source module exceeds ingestion object budget");
        const auto compiled = materialize_source(records.front(), *compilation.candidate, context,
            {limits.storage.objects - 1, limits.storage.encoded_bytes - result.encoded_bytes},
            limits.admission, limits.selectors);
        result.context = identify(compiled.context);
        result.module = identify(compiled.module);
        records.push_back(compiled.context);
        records.push_back(compiled.module);
        for (const auto& member : compiled.members) {
            result.members.push_back(identify(member));
            records.push_back(member);
        }
    }
    // All semantic and aggregate allocation checks completed before persistence.
    for (std::size_t i = 1; i < records.size(); ++i) result.encoded_bytes += encode_object(records[i]).size();
    for (const auto& record : records) {
        const auto id = identify(record);
        if (!journal.contains(id)) {
            journal.put(record);
            ++result.new_objects;
        }
    }
    return result;
}

} // namespace axiom::knowledge
