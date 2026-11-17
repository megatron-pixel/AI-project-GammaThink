#include "source_compiler.hpp"
#include "archive_dsl/codec.hpp"

#include <json-c/json.h>
#include <algorithm>
#include <charconv>
#include <memory>

namespace axiom::knowledge {
namespace {
using Json = std::unique_ptr<json_object, decltype(&json_object_put)>;

Json source_json(std::string_view text) {
    const auto parser = std::unique_ptr<json_tokener, decltype(&json_tokener_free)>(
        json_tokener_new_ex(gammathink::archive_dsl::max_depth + 2), json_tokener_free);
    if (!parser) throw Error(ErrorCode::Budget, "Cannot allocate source profile parser");
    Json root(json_tokener_parse_ex(parser.get(), text.data(), static_cast<int>(text.size())),
              json_object_put);
    if (!root || json_tokener_get_error(parser.get()) != json_tokener_success) {
        throw Error(ErrorCode::Schema, "Cannot inspect decoded source profile");
    }
    return root;
}

std::string decoded_source(std::string_view dsl) {
    try {
        const auto first = dsl.find_first_not_of(" \t\r\n");
        if (first != std::string_view::npos && dsl.substr(first).starts_with("GAMMATHINK_DSL 2"))
            return gammathink::archive_dsl::decode_v2(dsl);
        return gammathink::archive_dsl::decode(dsl);
    } catch (const gammathink::archive_dsl::Error& error) {
        throw Error(ErrorCode::Schema, error.what());
    }
}
ProblemProposal source_theory(json_object* theory, const ProblemContext& context,
    ProblemLimits limits, ProblemSelectorMode mode) {
    const auto text = json_object_to_json_string_ext(theory, JSON_C_TO_STRING_PLAIN);
    if (json_object_object_get(theory, "quantified_equations")) {
        auto parsed = parse_quantified_problem_theory_proposal(text, context, limits, mode);
        try {
            return instantiate_equations({context, std::move(parsed.base), limits}, parsed.schemas).request.proposal;
        } catch (const QuantifiedResourceLimit& error) {
            throw Error(ErrorCode::Budget, error.what());
        }
    }
    return parse_problem_theory_proposal(text, context, limits, mode);
}
}

SourceCompilation compile_source(const Object& source, const ObjectId& translated_source,
                                  std::string_view archive_dsl) {
    if (identify(source) != translated_source) {
        throw Error(ErrorCode::Integrity, "Translation input identity differs from source");
    }
    if (source.kind != ObjectKind::Evidence
        || !source.format.starts_with("source.zim.raw.v1:") || source.sources.size() != 1) {
        throw Error(ErrorCode::Schema, "Expected a complete raw source document");
    }
    const auto& reference = source.sources.front();
    if (reference.extractor_version != "libzim-raw-entry-v1" || reference.offset != 0
        || reference.length != source.payload.size()
        || reference.source_digest != digest(source.payload)) {
        throw Error(ErrorCode::Integrity, "Source coverage or content digest differs");
    }
    SourceCompilation result;
    result.parsed_json = decoded_source(archive_dsl);
    const auto root = source_json(result.parsed_json);
    if (const auto kind = json_object_object_get(root.get(), "kind")) {
        const std::string_view value(json_object_get_string(kind), json_object_get_string_len(kind));
        result.kind = value == "clarify" ? SourceCompilationKind::Clarify
                                        : SourceCompilationKind::Unsupported;
        const auto reason = json_object_object_get(root.get(), "reason");
        result.reason.assign(json_object_get_string(reason), json_object_get_string_len(reason));
        return result;
    }
    const auto goals = json_object_object_get(root.get(), "goals");
    if (json_object_array_length(goals) != 0) {
        throw Error(ErrorCode::Schema, "Source compilation cannot introduce execution goals");
    }
    Object candidate;
    candidate.kind = ObjectKind::Claim;
    candidate.visibility = source.visibility;
    candidate.label = source.label;
    const auto theory = json_object_object_get(root.get(), "theory");
    candidate.format = json_object_object_get(theory, "quantified_equations")
        ? "gammathink.archive-dsl.v2" : "gammathink.archive-dsl.v1";
    candidate.payload.assign(archive_dsl.begin(), archive_dsl.end());
    candidate.sources = source.sources;
    candidate.links.push_back({Relationship::DependsOn, translated_source});
    const auto entities = json_object_object_get(theory, "entities");
    // Index names only; the complete DSL retains duplicates, order and scopes.
    for (std::size_t i = 0; i < json_object_array_length(entities); ++i) {
        const auto entity = json_object_array_get_idx(entities, i);
        std::string value(json_object_get_string(entity), json_object_get_string_len(entity));
        validate_text(value);
        if (!value.empty()) candidate.concepts.push_back(std::move(value));
    }
    std::sort(candidate.concepts.begin(), candidate.concepts.end());
    candidate.concepts.erase(std::unique(candidate.concepts.begin(), candidate.concepts.end()),
                              candidate.concepts.end());
    (void)encode_object(candidate);
    result.candidate = std::move(candidate);
    return result;
}

void validate_source_candidate(const Object& source, const Object& candidate,
                                const ProblemContext& context, ProblemLimits limits,
                                ProblemSelectorMode mode) {
    const std::string dsl(candidate.payload.begin(), candidate.payload.end());
    const auto compilation = compile_source(source, identify(source), dsl);
    if (!compilation.candidate || *compilation.candidate != candidate) {
        throw Error(ErrorCode::Integrity, "Source candidate differs from bound translation");
    }
    if (context.views.size() != 1) {
        throw Error(ErrorCode::Schema, "Source compilation requires one complete source view");
    }
    const auto& view = context.views.front();
    const auto& reference = source.sources.front();
    if (view.basis != PremiseBasis::Source || view.archive_id != reference.archive_id
        || view.entry_path != reference.entry_path
        || view.extractor_version != reference.extractor_version
        || view.text != std::string(source.payload.begin(), source.payload.end())
        || view.digest != evidence_digest(view.text)) {
        throw Error(ErrorCode::Integrity, "Host source view differs from complete document");
    }
    const auto root = source_json(compilation.parsed_json);
    const auto theory = json_object_object_get(root.get(), "theory");
    try {
        (void)source_theory(theory, context, limits, mode);
    } catch (const Error&) {
        throw;
    } catch (const std::runtime_error& error) {
        throw Error(ErrorCode::Schema, error.what());
    }
}

CompiledSourceKnowledge materialize_source(const Object& source, const Object& candidate,
    const ProblemContext& context, SourceKnowledgeLimits budget,
    ProblemLimits limits, ProblemSelectorMode mode) {
    if (budget.objects < 2 || budget.objects > maximum_items
        || !budget.encoded_bytes || budget.encoded_bytes > 64 * 1024 * 1024) {
        throw Error(ErrorCode::Budget, "Invalid source materialization budget");
    }
    validate_source_candidate(source, candidate, context, limits, mode);
    CompiledSourceKnowledge result;
    result.context.kind = ObjectKind::Assumption;
    result.context.visibility = source.visibility;
    result.context.label = "Host interpretation context";
    result.context.format = mode == ProblemSelectorMode::OffsetV1
        ? "axiom.problem-context.offset-v1.json" : "axiom.problem-context.quote-v1.json";
    std::string context_json;
    try {
        context_json = serialize_problem_context(context);
    } catch (const std::runtime_error& error) {
        throw Error(ErrorCode::Schema, error.what());
    }
    result.context.payload.assign(context_json.begin(), context_json.end());
    result.context.sources = source.sources;
    result.context.links.push_back({Relationship::DependsOn, identify(source)});
    result.module = candidate;
    result.module.links.push_back({Relationship::DependsOn, identify(result.context)});
    const auto module_id = identify(result.module);
    const std::string dsl(candidate.payload.begin(), candidate.payload.end());
    const auto root = source_json(decoded_source(dsl));
    const auto theory = json_object_object_get(root.get(), "theory");
    std::size_t used = 0;
    const auto charge = [&](const Object& object) {
        const auto bytes = encode_object(object).size();
        if (bytes > budget.encoded_bytes - used) {
            throw Error(ErrorCode::Budget, "Source materialization byte budget exceeded");
        }
        used += bytes;
    };
    charge(result.context);
    charge(result.module);
    for (const auto section : {"bindings", "equations", "rules", "quantified_equations"}) {
        const auto records = json_object_object_get(theory, section);
        if (!records) continue; // Version 1 has no quantified declarations.
        for (std::size_t i = 0; i < json_object_array_length(records); ++i) {
            if (result.members.size() >= budget.objects - 2) {
                throw Error(ErrorCode::Budget, "Source materialization object budget exceeded");
            }
            Object member;
            member.kind = std::string_view(section) == "rules" || std::string_view(section) == "quantified_equations"
                ? ObjectKind::Rule : ObjectKind::Claim;
            member.visibility = source.visibility;
            member.label = source.label + " / " + section + " / " + std::to_string(i);
            member.format = "gammathink.source-member.v1";
            const auto pointer = "/theory/" + std::string(section) + "/" + std::to_string(i);
            member.payload.assign(pointer.begin(), pointer.end());
            member.concepts = candidate.concepts;
            member.sources = source.sources;
            member.links.push_back({Relationship::DependsOn, module_id});
            charge(member);
            result.members.push_back(std::move(member));
        }
    }
    return result;
}

std::string source_member_json(const Object& module, const Object& context, const Object& member) try {
    const auto module_id = identify(module);
    const auto context_id = identify(context);
    (void)identify(member);
    if (module.kind != ObjectKind::Claim ||
        (module.format != "gammathink.archive-dsl.v1" && module.format != "gammathink.archive-dsl.v2")
        || module.sources.size() != 1 || module.links.size() != 2
        || module.links[0].relationship != Relationship::DependsOn
        || module.links[1] != Link{Relationship::DependsOn, context_id}
        || context.kind != ObjectKind::Assumption || context.visibility != module.visibility
        || context.sources != module.sources
        || context.links != std::vector<Link>{module.links.front()}
        || member.format != "gammathink.source-member.v1" || member.visibility != module.visibility
        || member.sources != module.sources || member.concepts != module.concepts
        || member.links != std::vector<Link>{{Relationship::DependsOn, module_id}}) {
        throw Error(ErrorCode::Integrity, "Source member context or module binding differs");
    }
    ProblemSelectorMode mode;
    if (context.format == "axiom.problem-context.offset-v1.json") mode = ProblemSelectorMode::OffsetV1;
    else if (context.format == "axiom.problem-context.quote-v1.json") mode = ProblemSelectorMode::QuoteV1;
    else throw Error(ErrorCode::Schema, "Unknown source selector context");
    const auto host = parse_problem_context(std::string(context.payload.begin(), context.payload.end()));
    const auto& ref = module.sources.front();
    if (host.views.size() != 1 || host.views.front().basis != PremiseBasis::Source
        || host.views.front().archive_id != ref.archive_id || host.views.front().entry_path != ref.entry_path
        || host.views.front().extractor_version != ref.extractor_version
        || ref.offset != 0 || ref.length != host.views.front().text.size()
        || ref.source_digest != digest(std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(host.views.front().text.data()),
            host.views.front().text.size()))) {
        throw Error(ErrorCode::Integrity, "Source member input coverage differs");
    }
    const auto root = source_json(decoded_source(std::string(module.payload.begin(), module.payload.end())));
    const auto goals = json_object_object_get(root.get(), "goals");
    const auto theory = json_object_object_get(root.get(), "theory");
    if (!goals || !theory || json_object_array_length(goals)) {
        throw Error(ErrorCode::Schema, "Source module is not a goal-free theory");
    }
    if ((module.format == "gammathink.archive-dsl.v2") !=
        bool(json_object_object_get(theory, "quantified_equations")))
        throw Error(ErrorCode::Integrity, "Source module format differs from its declared grammar");
    (void)source_theory(theory, host, {}, mode);
    const std::string pointer(member.payload.begin(), member.payload.end());
    for (const auto section : {"bindings", "equations", "rules", "quantified_equations"}) {
        const auto prefix = "/theory/" + std::string(section) + "/";
        if (!pointer.starts_with(prefix)) continue;
        const auto index_text = std::string_view(pointer).substr(prefix.size());
        std::size_t index = 0;
        const auto parsed = std::from_chars(index_text.data(), index_text.data() + index_text.size(), index);
        const auto records = json_object_object_get(theory, section);
        if (!records) continue;
        if (parsed.ec != std::errc{} || parsed.ptr != index_text.data() + index_text.size()
            || std::to_string(index) != index_text || index >= json_object_array_length(records)
            || member.kind != (std::string_view(section) == "rules" || std::string_view(section) == "quantified_equations"
                ? ObjectKind::Rule : ObjectKind::Claim)) {
            throw Error(ErrorCode::Schema, "Invalid source member locator");
        }
        return json_object_to_json_string_ext(json_object_array_get_idx(records, index),
                                              JSON_C_TO_STRING_PLAIN);
    }
    throw Error(ErrorCode::Schema, "Unknown source member section");
} catch (const Error&) {
    throw;
} catch (const std::runtime_error& error) {
    throw Error(ErrorCode::Schema, error.what());
}

ProblemRequest source_query_request(const Object& source, const Object& module, const Object& context,
    const ProblemContext& host_context, const Atom& goal, ProblemLimits limits) {
    return source_query_request(source, module, context, host_context, ProblemGoal{ProblemAtomGoal{goal}}, limits);
}

ProblemRequest source_query_request(const Object& source, const Object& module, const Object& context,
    const ProblemContext& host_context, const ProblemGoal& goal, ProblemLimits limits) try {
    auto proposal = source_theory_proposal(source, module, context, host_context, limits);
    if (const auto* atom = std::get_if<ProblemAtomGoal>(&goal)) {
        if (atom->atom.terms.size() > 4) throw Error(ErrorCode::Budget, "Source query exceeds native atom arity");
        for (const auto& term : atom->atom.terms)
            if (term.kind != TermKind::Constant) throw Error(ErrorCode::Schema, "Source query requires a ground atom");
    }
    proposal.goal = goal;
    ProblemRequest request{host_context, std::move(proposal), limits};
    (void)admit_problem(request);
    return request;
} catch (const Error&) {
    throw;
} catch (const std::runtime_error& error) {
    throw Error(ErrorCode::Schema, error.what());
}

ProblemProposal source_theory_proposal(const Object& source, const Object& module, const Object& context,
    const ProblemContext& host_context, ProblemLimits limits) try {
    const auto source_id = identify(source);
    const auto context_id = identify(context);
    (void)identify(module);
    if (context.kind != ObjectKind::Assumption || context.visibility != source.visibility
        || context.sources != source.sources
        || context.links != std::vector<Link>{{Relationship::DependsOn, source_id}}
        || module.links != std::vector<Link>{{Relationship::DependsOn, source_id},
                                            {Relationship::DependsOn, context_id}}) {
        throw Error(ErrorCode::Integrity, "Source query module or context binding differs");
    }
    ProblemSelectorMode mode;
    if (context.format == "axiom.problem-context.offset-v1.json") mode = ProblemSelectorMode::OffsetV1;
    else if (context.format == "axiom.problem-context.quote-v1.json") mode = ProblemSelectorMode::QuoteV1;
    else throw Error(ErrorCode::Schema, "Unknown source query selector context");
    const auto stored_context = parse_problem_context(std::string(context.payload.begin(), context.payload.end()));
    if (stored_context != host_context)
        throw Error(ErrorCode::Integrity, "Source query differs from host-owned registry or complete source view");
    auto candidate = module;
    candidate.links.pop_back(); // The original translation remains goal-free.
    validate_source_candidate(source, candidate, host_context, limits, mode);
    const auto root = source_json(decoded_source(std::string(module.payload.begin(), module.payload.end())));
    auto* theory = json_object_object_get(root.get(), "theory");
    return source_theory(theory, host_context, limits, mode);
} catch (const Error&) {
    throw;
} catch (const std::runtime_error& error) {
    throw Error(ErrorCode::Schema, error.what());
}

} // namespace axiom::knowledge
