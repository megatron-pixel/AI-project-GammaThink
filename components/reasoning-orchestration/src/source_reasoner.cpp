#include "source_reasoner.hpp"
#include "problem_wire.hpp"

#include <functional>
#include <map>
#include <set>

namespace axiom::knowledge {
namespace {
void require(bool value, ErrorCode code, const char* message) {
    if (!value) throw Error(code, message);
}
bool invalid(const std::optional<Assessment>& value) {
    return value && (value->state == KnowledgeState::Invalidated || value->logical == LogicalValidity::Invalid);
}
Object rule_locator(const Object& module, std::size_t index) {
    Object member;
    member.kind = ObjectKind::Rule; member.visibility = module.visibility;
    member.label = module.label + " / rules / " + std::to_string(index);
    member.format = "gammathink.source-member.v1";
    const auto pointer = "/theory/rules/" + std::to_string(index);
    member.payload.assign(pointer.begin(), pointer.end());
    member.concepts = module.concepts; member.sources = module.sources;
    member.links = {{Relationship::DependsOn, identify(module)}};
    return member;
}
}

SourceLogicRun run_source_logic(UnifiedKnowledge& knowledge, Journal& journal,
    const ObjectId& module_id, const ProblemContext& host_context, const Atom& requested,
    SourceLogicLimits limits) {
    require(limits.rule_queries > 0 && limits.rule_queries <= 512 && limits.work > 0
        && limits.work <= 100000000 && limits.operation_time.count() > 0
        && limits.operation_time <= std::chrono::minutes(5), ErrorCode::Budget, "Invalid source reasoning limits");
    SourceLogicRun run;
    auto& outcome = run.outcome;
    const auto deadline = std::chrono::steady_clock::now() + limits.operation_time;
    std::map<ObjectId, Object> records;
    std::map<ObjectId, ObservedAssessment> observations;
    std::set<ObjectId> unavailable, blocked;
    std::size_t encoded_bytes = 0;
    const auto spend = [&] {
        require(run.work < limits.work && std::chrono::steady_clock::now() < deadline,
            ErrorCode::Budget, "Source reasoning work or time budget exceeded");
        ++run.work;
    };
    const auto observe = [&](const ObjectId& id) {
        const auto current = journal.assessment(id);
        const auto [found, inserted] = observations.emplace(id, ObservedAssessment{id, current ? current->version : 0});
        require(inserted || found->second.version == (current ? current->version : 0),
            ErrorCode::Unavailable, "Source reasoning assessment changed");
        if (invalid(current)) blocked.insert(id);
    };
    const auto absorb = [&](const DependencyResolution& closure) {
        for (const auto& id : closure.unavailable) {
            spend(); unavailable.insert(id); observe(id);
        }
        for (const auto& item : closure.objects) {
            spend(); observe(item.id);
            if (invalid(item.record.assessment)) blocked.insert(item.id);
            if (records.contains(item.id)) continue;
            require(records.size() + unavailable.size() < limits.retrieval.objects,
                ErrorCode::Budget, "Source reasoning object budget exceeded");
            const auto size = encode_object(item.record.object).size();
            require(size <= limits.retrieval.encoded_bytes - encoded_bytes,
                ErrorCode::Budget, "Source reasoning record byte budget exceeded");
            encoded_bytes += size; records.emplace(item.id, item.record.object);
        }
        outcome.object_lookups = records.size() + unavailable.size();
    };
    try {
        journal.read_snapshot([&] { absorb(knowledge.resolve(module_id, limits.retrieval)); });
        if (!unavailable.empty() || !blocked.empty()) {
            outcome.status = blocked.empty() ? KnowledgeRunStatus::DependenciesUnavailable : KnowledgeRunStatus::RevalidationRequired;
            outcome.unavailable.assign(unavailable.begin(), unavailable.end());
            outcome.revalidation_required.assign(blocked.begin(), blocked.end());
            outcome.reason = blocked.empty() ? "Source scope dependencies are unavailable"
                : "Source scope assessment requires revalidation";
            return run;
        }
        const auto& module = records.at(module_id);
        require(module.kind == ObjectKind::Claim &&
            (module.format == "gammathink.archive-dsl.v1" || module.format == "gammathink.archive-dsl.v2") &&
            module.links.size() == 2,
            ErrorCode::Schema, "Source logic requires one compiled source module");
        require(module.links[0].relationship == Relationship::DependsOn
            && module.links[1].relationship == Relationship::DependsOn
            && records.contains(module.links[0].target) && records.contains(module.links[1].target),
            ErrorCode::Integrity, "Source logic scope dependencies differ");
        const auto source_id = module.links[0].target, context_id = module.links[1].target;
        const auto& source = records.at(source_id);
        const auto& context = records.at(context_id);
        const auto problem = source_query_request(source, module, context, host_context, requested);
        const auto& proposal = problem.proposal;
        require(proposal.quantities.empty() && proposal.numbers.empty() && proposal.bindings.empty()
            && proposal.nodes.empty() && proposal.equations.empty(), ErrorCode::Schema,
            "Source logic profile does not ignore quantity or equation assertions");
        RelationalRequest input{proposal.logic, requested, limits.search, false};
        input.module.name = "source_" + hex(module_id);
        input.module.version = "1";
        // Pure index normalization, never lexical unification across sources.
        for (std::size_t i = 0; i < input.module.constants.size(); ++i) input.module.constants[i] = "entity_" + std::to_string(i);
        for (std::size_t i = 0; i < input.module.predicates.size(); ++i) input.module.predicates[i].name = "predicate_" + std::to_string(i);
        std::vector<Object> locators;
        std::vector<ObjectId> member_ids;
        for (std::size_t i = 0; i < input.module.rules.size(); ++i) {
            locators.push_back(rule_locator(module, i));
            member_ids.push_back(identify(locators.back()));
            input.module.rules[i].provenance = hex(member_ids.back());
        }
        Grounding grounding;
        Object executable;
        try {
            grounding = ground_relational(input);
            executable.payload = encode_relational_request(input);
        } catch (const std::runtime_error& error) {
            throw Error(ErrorCode::Schema, error.what());
        }
        executable.kind = ObjectKind::Goal;
        executable.label = "Source-bound finite-Horn query";
        executable.format = native_request_format;
        executable.sources = source.sources;
        executable.links = {{Relationship::DependsOn, module_id}, {Relationship::DependsOn, context_id}};
        for (const auto& id : member_ids) executable.links.push_back({Relationship::DependsOn, id});
        for (const auto& [id, object] : records) {
            if (!journal.contains(id)) { journal.put(object); ++outcome.imported_objects; }
        }
        // Persist requested work before any symbolic search. Private by default.
        run.request = journal.put(executable);
        RelationalResult reply{input, grounding.instances, {}};
        auto& proof = reply.proof;
        proof.operation = Operation::Prove; proof.module = grounding.request.module;
        proof.goal = grounding.request.goal;
        const auto literal_count = proof.module.symbols.size() * 2;
        std::vector<bool> active(literal_count, false);
        std::vector<std::uint32_t> proved(literal_count, no_step);
        std::vector<std::optional<bool>> admitted(member_ids.size());
        journal.read_snapshot([&] {
            observe(*run.request);
            for (const auto& [id, object] : records) { (void)object; observe(id); }
            const auto rule = [&](std::uint32_t ground_index) -> const Rule* {
                const auto source_index = grounding.instances.at(ground_index).source_rule;
                if (!admitted[source_index]) {
                    require(run.rule_queries < limits.rule_queries, ErrorCode::Budget, "Source rule query budget exceeded");
                    ++run.rule_queries; spend();
                    const auto closure = knowledge.resolve(member_ids[source_index], limits.retrieval);
                    absorb(closure);
                    admitted[source_index] = closure.complete() && blocked.empty();
                    if (*admitted[source_index]) {
                        const auto& member = records.at(member_ids[source_index]);
                        require(member == locators[source_index], ErrorCode::Integrity, "Source rule locator differs");
                    } else proof.limited = true;
                }
                if (!*admitted[source_index]) return nullptr;
                // The fetched, verified locator selects this exact scoped native
                // tuple/ground instance. Missing records cannot supply a rule.
                return &grounding.request.module.rules.at(ground_index);
            };
            const auto event = [&](EventKind kind, std::uint32_t literal, std::uint32_t rule_index, std::uint32_t depth) {
                spend();
                if (proof.history.size() == limits.search.events) { proof.limited = true; return false; }
                proof.history.push_back({kind, literal, rule_index, depth}); return true;
            };
            std::function<std::uint32_t(std::uint32_t, std::uint32_t)> derive;
            derive = [&](std::uint32_t literal, std::uint32_t depth) -> std::uint32_t {
                if (!event(EventKind::Visit, literal, no_step, depth)) return no_step;
                if (depth > limits.search.depth) {
                    proof.limited = true; event(EventKind::Bound, literal, no_step, depth); return no_step;
                }
                if (proved[literal] != no_step) {
                    event(EventKind::Cached, literal, no_step, depth); return proved[literal];
                }
                if (active[literal]) { event(EventKind::Cycle, literal, no_step, depth); return no_step; }
                active[literal] = true;
                for (std::uint32_t i = 0; i < proof.module.rules.size(); ++i) {
                    spend();
                    if (proof.module.rules[i].head != literal) continue;
                    if (!event(EventKind::TryRule, literal, i, depth)) break;
                    const auto* candidate = rule(i);
                    if (!candidate) continue;
                    std::vector<std::uint32_t> dependencies;
                    bool success = true;
                    for (const auto premise : candidate->premises) {
                        const auto step = derive(premise, depth + 1);
                        if (step == no_step) { success = false; break; }
                        dependencies.push_back(step);
                    }
                    if (success) {
                        if (!event(EventKind::Derived, literal, i, depth)) break;
                        const auto step = static_cast<std::uint32_t>(proof.proof.size());
                        proof.proof.push_back({literal, i, std::move(dependencies)});
                        proved[literal] = step; active[literal] = false; return step;
                    }
                    if (!event(EventKind::Backtrack, literal, i, depth)) break;
                }
                active[literal] = false; return no_step;
            };
            if (blocked.empty()) {
                proof.positive_root = derive(proof.goal, 0);
                proof.negative_root = derive(proof.goal ^ 1U, 0);
            } else proof.limited = true;
        });
        const bool positive = proof.positive_root != no_step, negative = proof.negative_root != no_step;
        proof.status = positive && negative ? Status::Contradiction : proof.limited ? Status::Unknown :
            positive ? Status::Proved : negative ? Status::Disproved : Status::Unknown;
        require(verify_relational_result(input, reply), ErrorCode::Integrity, "Source reasoning certificate failed independent replay");
        outcome.execution.reply = encode_relational_result(reply);
        outcome.execution.check = check_native_reply(executable.payload, outcome.execution.reply);
        require(outcome.execution.check.valid, ErrorCode::Integrity, "Source native request binding failed");
        outcome.execution.producer_work = run.work;
        for (const auto& [id, object] : records) {
            if (!journal.contains(id)) { journal.put(object); ++outcome.imported_objects; }
        }
        const auto certificate = journal.put(native_execution_record(executable, *run.request, outcome.execution.reply));
        outcome.execution.certificate = certificate;
        observe(certificate);
        Assessment assessment;
        const bool conclusive = outcome.execution.check.conclusive && unavailable.empty() && blocked.empty();
        assessment.state = conclusive ? KnowledgeState::Verified : KnowledgeState::Unresolved;
        assessment.logical = conclusive ? LogicalValidity::Valid : LogicalValidity::Unchecked;
        assessment.checker = "source-bound-native-replay-v1"; assessment.certificate = certificate;
        std::vector<ObservedAssessment> guarded;
        for (const auto& [id, observation] : observations) {
            guarded.push_back(observation);
            if (id != certificate) assessment.checked_dependencies.push_back(id);
        }
        journal.record_revalidated_assessment(certificate, std::move(assessment), guarded);
        const auto stored_certificate = journal.get(certificate);
        const auto graph = materialize_native(executable, *stored_certificate, limits.materialization);
        for (const auto& object : graph.objects) journal.put(object);
        outcome.conclusions = graph.conclusions; outcome.derivations = graph.derivations;
        outcome.unavailable.assign(unavailable.begin(), unavailable.end());
        outcome.execution.unavailable = outcome.unavailable;
        outcome.revalidation_required.assign(blocked.begin(), blocked.end());
        outcome.status = !blocked.empty() ? KnowledgeRunStatus::RevalidationRequired :
            !unavailable.empty() ? KnowledgeRunStatus::DependenciesUnavailable :
            proof.limited ? KnowledgeRunStatus::ResourceLimited :
            outcome.execution.check.conflict ? KnowledgeRunStatus::Conflict :
            outcome.execution.check.conclusive ? KnowledgeRunStatus::Checked : KnowledgeRunStatus::Unresolved;
        outcome.reason = !blocked.empty() ? "A queried source record requires revalidation" :
            !unavailable.empty() ? "A queried rule or its recorded dependency is unavailable" :
            proof.limited ? "Source reasoning reached its search bound" :
            outcome.execution.check.conflict ? "Both signed roots are proved in the native certificate" :
            outcome.execution.check.conclusive ? "Native certificate replayed against the pinned source scope" :
            "Neither signed goal has a checked derivation in the available source scope";
        return run;
    } catch (const Error& error) {
        if (error.code == ErrorCode::Budget) outcome.status = KnowledgeRunStatus::ResourceLimited;
        else if (error.code == ErrorCode::Unavailable) outcome.status = KnowledgeRunStatus::DependenciesUnavailable;
        else throw;
        outcome.reason = error.what(); return run;
    }
}

SourceQuantityRun run_source_quantity(UnifiedKnowledge& knowledge, Journal& journal,
    const ObjectId& module_id, const ProblemContext& host_context, const ProblemQuantityGoal& goal,
    KnowledgeRunLimits limits) {
    SourceQuantityRun run;
    std::map<ObjectId, Object> records;
    std::set<ObjectId> unavailable, blocked;
    std::size_t bytes = 0;
    std::optional<ProblemRequest> request;
    try {
        const auto fetch = [&](const ObjectId& id) {
            const auto closure = knowledge.resolve(id, limits.retrieval);
            unavailable.insert(closure.unavailable.begin(), closure.unavailable.end());
            for (const auto& item : closure.objects) {
                if (invalid(item.record.assessment) || invalid(journal.assessment(item.id))) blocked.insert(item.id);
                if (records.contains(item.id)) continue;
                require(records.size() + unavailable.size() < limits.retrieval.objects,
                        ErrorCode::Budget, "Source quantity object budget exceeded");
                const auto size = encode_object(item.record.object).size();
                require(size <= limits.retrieval.encoded_bytes - bytes,
                        ErrorCode::Budget, "Source quantity record byte budget exceeded");
                bytes += size;
                records.emplace(item.id, item.record.object);
            }
        };
        journal.read_snapshot([&] {
            fetch(module_id);
            if (!unavailable.empty() || !blocked.empty()) return;
            const auto& module = records.at(module_id);
            require(module.links.size() == 2 &&
                    module.links[0].relationship == Relationship::DependsOn &&
                    module.links[1].relationship == Relationship::DependsOn &&
                    records.contains(module.links[0].target) && records.contains(module.links[1].target),
                    ErrorCode::Integrity, "Source quantity scope dependencies differ");
            const auto& source = records.at(module.links[0].target);
            const auto& context = records.at(module.links[1].target);
            request = source_query_request(source, module, context, host_context, ProblemGoal{goal});
            auto candidate = module;
            candidate.links.pop_back();
            const auto mode = context.format == "axiom.problem-context.offset-v1.json"
                ? ProblemSelectorMode::OffsetV1 : ProblemSelectorMode::QuoteV1;
            const auto compiled = materialize_source(source, candidate, host_context,
                {limits.retrieval.objects, limits.retrieval.encoded_bytes}, request->limits, mode);
            require(compiled.module == module && compiled.context == context, ErrorCode::Integrity,
                    "Source quantity materialization differs from archived scope");
            for (const auto& member : compiled.members) {
                ++run.member_queries;
                const auto id = identify(member);
                fetch(id);
                if (records.contains(id)) require(records.at(id) == member, ErrorCode::Integrity,
                                                "Source quantity member differs from scoped locator");
            }
        });
        run.outcome.object_lookups = records.size() + unavailable.size();
        if (!unavailable.empty() || !blocked.empty()) {
            run.outcome.status = blocked.empty() ? KnowledgeRunStatus::DependenciesUnavailable
                                                : KnowledgeRunStatus::RevalidationRequired;
            run.outcome.unavailable.assign(unavailable.begin(), unavailable.end());
            run.outcome.revalidation_required.assign(blocked.begin(), blocked.end());
            run.outcome.reason = blocked.empty() ? "Source quantity dependencies are unavailable"
                                                : "Source quantity dependencies require revalidation";
            return run;
        }
        require(request.has_value(), ErrorCode::Integrity, "Source quantity scope lost its request");
        Object executable;
        executable.kind = ObjectKind::Goal;
        executable.label = "Source-bound quantity query";
        executable.format = native_request_format;
        executable.payload = encode_problem_request(*request);
        executable.sources = records.at(module_id).sources;
        // Full-module profile: retain every observed prerequisite, including
        // guard rules and alternatives, rather than trimming to an inferred answer.
        for (const auto& [id, object] : records) {
            (void)object;
            executable.links.push_back({Relationship::DependsOn, id});
        }
        require(records.size() < limits.retrieval.objects &&
                encode_object(executable).size() <= limits.retrieval.encoded_bytes - bytes,
                ErrorCode::Budget, "Source quantity request exceeds aggregate record budget");
        std::size_t imported = 0;
        for (const auto& [id, object] : records) {
            if (!journal.contains(id)) { journal.put(object); ++imported; }
        }
        run.request = journal.put(executable);
        JournalArchive local(journal);
        UnifiedKnowledge checked({&local});
        auto outcome = run_knowledge_goal(checked, journal, *run.request, limits);
        outcome.object_lookups += run.outcome.object_lookups;
        outcome.imported_objects += imported;
        run.outcome = std::move(outcome);
        return run;
    } catch (const Error& error) {
        if (error.code == ErrorCode::Budget) run.outcome.status = KnowledgeRunStatus::ResourceLimited;
        else if (error.code == ErrorCode::Unavailable) run.outcome.status = KnowledgeRunStatus::DependenciesUnavailable;
        else throw;
        run.outcome.reason = error.what();
        return run;
    }
}

} // namespace axiom::knowledge
