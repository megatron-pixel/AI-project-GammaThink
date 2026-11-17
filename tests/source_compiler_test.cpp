#include "source_compiler.hpp"
#include "journal.hpp"
#include "archive_dsl/codec.hpp"

#include <iostream>
#include <stdexcept>

namespace {

using namespace axiom::knowledge;
using namespace axiom;
std::size_t assertions = 0;

void check(bool condition) {
    ++assertions;
    if (!condition) throw std::runtime_error("Source compiler assertion " + std::to_string(assertions));
}

template<class Function> void rejects(ErrorCode expected, Function function) {
    try {
        function();
    } catch (const Error& error) {
        check(error.code == expected);
        return;
    }
    throw std::runtime_error("Expected source compiler rejection");
}

} // namespace

int main() {
    try {
        Object source;
        source.kind = ObjectKind::Evidence;
        source.label = "Synthetic complete source";
        source.format = "source.zim.raw.v1:text/plain";
        const std::string text = "Not every bird flies. The source decimal is 1.2300.";
        source.payload.assign(text.begin(), text.end());
        source.sources.push_back({"zim:synthetic", "version-1", "article",
                                  "libzim-raw-entry-v1", digest(source.payload), 0, text.size()});
        const auto id = identify(source);
        const std::string dsl =
            "GAMMATHINK_DSL 1\nENTITY \"bird\"\nENTITY \"bird\"\n"
            "PREDICATE [\"flies\",1]\nRULE [false,[\"scoped\",\"unadmitted\"]]\n"
            "NUMBER [1.2300,9007199254740993,-0]\nEND\n";
        const auto result = compile_source(source, id, dsl);
        check(result.kind == SourceCompilationKind::Candidate);
        check(result.candidate.has_value());
        const auto& candidate = *result.candidate;
        check(candidate.kind == ObjectKind::Claim);
        check(candidate.visibility == Visibility::Private);
        check(candidate.format == "gammathink.archive-dsl.v1");
        check(std::string(candidate.payload.begin(), candidate.payload.end()) == dsl);
        check(candidate.sources == source.sources);
        check(candidate.links == std::vector<Link>{{Relationship::DependsOn, id}});
        check(candidate.concepts == std::vector<std::string>{"bird"});
        check(result.parsed_json == gammathink::archive_dsl::decode(dsl));
        check(result.parsed_json.find("1.2300") != std::string::npos);
        check(result.parsed_json.find("9007199254740993") != std::string::npos);
        check(result.parsed_json.find("-0") != std::string::npos);
        check(decode_object(identify(candidate), encode_object(candidate)) == candidate);
        // Deliberately unadmitted nested tuples stay candidates, not executable
        // proofs or claims of meaning fidelity. Nothing invokes the core.
        Journal journal(":memory:");
        check(journal.put(source) == id);
        const auto candidate_id = journal.put(candidate);
        check(!journal.assessment(candidate_id));
        check(journal.by_concept("bird").objects == std::vector<ObjectId>{candidate_id});
        check(journal.required_by(id).objects == std::vector<ObjectId>{candidate_id});
        check(compile_source(source, id, "GAMMATHINK_DSL 1\nEND\n").candidate.has_value());
        for (const auto& [role, kind] : {
             std::pair{"CLARIFY", SourceCompilationKind::Clarify},
             std::pair{"UNSUPPORTED", SourceCompilationKind::Unsupported}}) {
            const auto decline = compile_source(source, id,
                std::string("GAMMATHINK_DSL 1\n") + role + " \"scope ambiguous\"\nEND\n");
            check(decline.kind == kind && !decline.candidate && decline.reason == "scope ambiguous");
        }
        rejects(ErrorCode::Schema, [&] {
            (void)compile_source(source, id, "GAMMATHINK_DSL 1\nGOAL {\"q\":0}\nEND\n");
        });
        rejects(ErrorCode::Schema, [&] {
            (void)compile_source(source, id, "GAMMATHINK_DSL 1\nANSWER 4\nEND\n");
        });
        rejects(ErrorCode::Schema, [&] {
            (void)compile_source(source, id, "GAMMATHINK_DSL 1\nRULE {\"inferred\":true}\nEND\n");
        });
        auto changed = source;
        changed.payload.push_back('x');
        rejects(ErrorCode::Integrity, [&] { (void)compile_source(changed, id, dsl); });
        rejects(ErrorCode::Integrity, [&] { (void)compile_source(changed, identify(changed), dsl); });
        changed = source;
        changed.sources.front().offset = 1;
        rejects(ErrorCode::Integrity, [&] { (void)compile_source(changed, identify(changed), dsl); });
        changed = source;
        changed.sources.front().extractor_version = "excerpt";
        rejects(ErrorCode::Integrity, [&] { (void)compile_source(changed, identify(changed), dsl); });
        changed = source;
        changed.kind = ObjectKind::Fact;
        rejects(ErrorCode::Schema, [&] { (void)compile_source(changed, identify(changed), dsl); });
        changed = source;
        changed.visibility = Visibility::Public;
        check(compile_source(changed, identify(changed), dsl).candidate->visibility == Visibility::Public);
        check(!journal.assessment(id));
        Object native_source = source;
        const std::string native_text = "Robin flies. Robin does not fly.";
        native_source.payload.assign(native_text.begin(), native_text.end());
        native_source.sources.front().length = native_text.size();
        native_source.sources.front().source_digest = digest(native_source.payload);
        ProblemContext context;
        context.unit_registry_id = "synthetic";
        context.unit_registry_version = "1";
        context.views.push_back(make_evidence_view("source", PremiseBasis::Source, native_text,
            "zim:synthetic", "article", "libzim-raw-entry-v1"));
        const std::string support = "[[0,0," + std::to_string(native_text.size()) + ",null]]";
        const std::string native_dsl = "GAMMATHINK_DSL 1\nENTITY \"robin\"\n"
            "PREDICATE [\"flies\",1]\nRULE [0,[0,false,[[\"c\",0]]],[]," + support
            + "]\nRULE [0,[0,true,[[\"c\",0]]],[]," + support + "]\nEND\n";
        const auto native_candidate = *compile_source(
            native_source, identify(native_source), native_dsl).candidate;
        validate_source_candidate(native_source, native_candidate, context);
        check(native_candidate.kind == ObjectKind::Claim && !native_candidate.derivation);
        check(!journal.assessment(journal.put(native_candidate)));
        auto bad_context = context;
        bad_context.views.front().basis = PremiseBasis::Formal;
        rejects(ErrorCode::Integrity, [&] {
            validate_source_candidate(native_source, native_candidate, bad_context);
        });
        bad_context = context;
        bad_context.views.front().text += "different";
        rejects(ErrorCode::Integrity, [&] {
            validate_source_candidate(native_source, native_candidate, bad_context);
        });
        bad_context = context;
        bad_context.views.front().entry_path = "another";
        rejects(ErrorCode::Integrity, [&] {
            validate_source_candidate(native_source, native_candidate, bad_context);
        });
        bad_context = context;
        bad_context.views.push_back(context.views.front());
        rejects(ErrorCode::Schema, [&] {
            validate_source_candidate(native_source, native_candidate, bad_context);
        });
        auto forged = native_candidate;
        forged.visibility = Visibility::Public;
        rejects(ErrorCode::Integrity, [&] {
            validate_source_candidate(native_source, forged, context);
        });
        const auto invalid_native = [&](const std::string& body) {
            const auto value = *compile_source(native_source, identify(native_source),
                "GAMMATHINK_DSL 1\n" + body + "\nEND\n").candidate;
            rejects(ErrorCode::Schema, [&] { validate_source_candidate(native_source, value, context); });
        };
        invalid_native("ENTITY \"robin\"\nENTITY \"robin\"");
        invalid_native("ENTITY \"robin\"\nPREDICATE [\"flies\",1]\nRULE [0,[0,false,[[\"c\",1]]],[],"
                       + support + "]");
        invalid_native("ENTITY \"robin\"\nPREDICATE [\"flies\",1]\nRULE [1,[0,false,[[\"v\",0]]],[],"
                       + support + "]");
        invalid_native("ENTITY \"robin\"\nPREDICATE [\"flies\",1]\nRULE [0,[0,false,[[\"c\",0]]],[],[]]");
        invalid_native("NODE [\"n\",999]");
        invalid_native("QUANTITY [\"speed\",0,[true,[]],0]");
        invalid_native("ENTITY \"robin\"\nPREDICATE [\"flies\",1]\nRULE [0,[0,false,[[\"c\",0]]],[],"
                       "[[0,9999,1,null]]]");
        // Goal-free validation must not weaken executable admission.
        ProblemRequest no_goal{context, {}, {}};
        (void)admit_problem_theory(no_goal);
        check(no_goal.context == context && no_goal.proposal.quantities.empty());
        bool executable_rejected = false;
        try { (void)admit_problem(no_goal); }
        catch (const std::runtime_error&) { executable_rejected = true; }
        check(executable_rejected);
        const auto empty_candidate = *compile_source(
            native_source, identify(native_source), "GAMMATHINK_DSL 1\nEND\n").candidate;
        validate_source_candidate(native_source, empty_candidate, context);
        check(empty_candidate.links == std::vector<Link>{{Relationship::DependsOn, identify(native_source)}});
        const auto compiled = materialize_source(native_source, native_candidate, context);
        check(compiled.context.kind == ObjectKind::Assumption);
        check(parse_problem_context(std::string(compiled.context.payload.begin(),
                                               compiled.context.payload.end())) == context);
        check(compiled.module.payload == native_candidate.payload);
        check(compiled.module.links == std::vector<Link>({
            {Relationship::DependsOn, identify(native_source)},
            {Relationship::DependsOn, identify(compiled.context)}}));
        check(compiled.members.size() == 2);
        const Atom requested{0, false, {{TermKind::Constant, 0}}};
        const auto query = source_query_request(native_source, compiled.module, compiled.context, context, requested);
        check(query.context == context && std::get<ProblemAtomGoal>(query.proposal.goal).atom == requested);
        check(query.proposal.logic.rules.size() == 2 && query.proposal.logic.rules[1].head.negative);
        check(compiled.module.payload == native_candidate.payload); // No query written into archived knowledge.
        auto wrong_host = context; wrong_host.unit_registry_version = "different";
        rejects(ErrorCode::Integrity, [&] {
            source_query_request(native_source, compiled.module, compiled.context, wrong_host, requested);
        });
        auto wrong_scope = compiled.module; wrong_scope.links.back().target = id;
        rejects(ErrorCode::Integrity, [&] {
            source_query_request(native_source, wrong_scope, compiled.context, context, requested);
        });
        rejects(ErrorCode::Schema, [&] {
            source_query_request(native_source, compiled.module, compiled.context, context,
                Atom{0, false, {{TermKind::Variable, 0}}});
        });
        rejects(ErrorCode::Schema, [&] {
            source_query_request(native_source, compiled.module, compiled.context, context,
                Atom{9, false, {{TermKind::Constant, 0}}});
        });
        rejects(ErrorCode::Schema, [&] {
            source_query_request(native_source, compiled.module, compiled.context, context,
                Atom{0, false, {{TermKind::Constant, 9}}});
        });
        check(identify(compiled.members[0]) != identify(compiled.members[1]));
        check(compiled.members[0].kind == ObjectKind::Rule && compiled.members[1].kind == ObjectKind::Rule);
        check(source_member_json(compiled.module, compiled.context, compiled.members[0])
            == "[0,[0,false,[[\"c\",0]]],[]," + support + "]");
        check(source_member_json(compiled.module, compiled.context, compiled.members[1])
            == "[0,[0,true,[[\"c\",0]]],[]," + support + "]");
        journal.put(native_source);
        const auto context_id = journal.put(compiled.context);
        const auto module_id = journal.put(compiled.module);
        for (const auto& member : compiled.members) {
            const auto member_id = journal.put(member);
            check(!journal.assessment(member_id));
            check(journal.get(member_id)->sources == native_source.sources);
        }
        check(!journal.assessment(context_id) && !journal.assessment(module_id));
        check(journal.required_by(context_id).objects == std::vector<ObjectId>{module_id});
        auto revised_context = context;
        revised_context.unit_registry_version = "2";
        const auto revised = materialize_source(native_source, native_candidate, revised_context);
        check(identify(revised.context) != context_id && identify(revised.module) != module_id);
        check(identify(revised.members[0]) != identify(compiled.members[0]));
        auto scaled_context = context;
        scaled_context.units.push_back({"scalar", "1", {}, Rational(1), {"1"}});
        const auto scaled1 = materialize_source(native_source, native_candidate, scaled_context);
        scaled_context.units[0].scale = 2;
        const auto scaled2 = materialize_source(native_source, native_candidate, scaled_context);
        check(identify(scaled1.module) != identify(scaled2.module));
        const auto empty_offsets = materialize_source(native_source, empty_candidate, context);
        const auto empty_quotes = materialize_source(native_source, empty_candidate, context, {}, {},
                                                     ProblemSelectorMode::QuoteV1);
        check(empty_offsets.members.empty() && empty_quotes.members.empty());
        check(identify(empty_offsets.module) != identify(empty_quotes.module));
        auto quote_dsl = native_dsl;
        const auto quoted_support = "[[0,null,\"" + native_text + "\",null]]";
        for (std::size_t pos = 0; (pos = quote_dsl.find(support, pos)) != std::string::npos;) {
            quote_dsl.replace(pos, support.size(), quoted_support);
            pos += quoted_support.size();
        }
        const auto quote_candidate = *compile_source(
            native_source, identify(native_source), quote_dsl).candidate;
        const auto quotes = materialize_source(native_source, quote_candidate, context, {}, {},
                                                ProblemSelectorMode::QuoteV1);
        check(source_member_json(quotes.module, quotes.context, quotes.members[0])
              == "[0,[0,false,[[\"c\",0]]],[]," + quoted_support + "]");
        Object quantity_source = native_source;
        const std::string quantity_text =
            "The first speed is exactly 7 m/s. The second speed equals the first speed.";
        quantity_source.payload.assign(quantity_text.begin(), quantity_text.end());
        quantity_source.sources[0].length = quantity_text.size();
        quantity_source.sources[0].source_digest = digest(quantity_source.payload);
        auto quantity_context = context;
        quantity_context.views[0] = make_evidence_view("source", PremiseBasis::Source, quantity_text,
            "zim:synthetic", "article", "libzim-raw-entry-v1");
        quantity_context.units.push_back({"m_per_s", "m/s", {0,1,-1,0,0,0,0}, Rational(1), {"m/s"}});
        const auto all_support = "[[0,0," + std::to_string(quantity_text.size()) + ",null]]";
        const std::string quantity_dsl =
            "GAMMATHINK_DSL 1\nENTITY \"cart\"\n"
            "QUANTITY [\"first_speed\",0,[true,[]],0]\n"
            "QUANTITY [\"second_speed\",0,[true,[]],0]\n"
            "NUMBER [[0," + std::to_string(quantity_text.find("7"))
            + ",1,null],[0," + std::to_string(quantity_text.find("m/s"))
            + ",3,null],0,\"exact\",[0," + std::to_string(quantity_text.find("exactly"))
            + ",7,null]]\nBINDING [0,0," + all_support + "]\n"
            "NODE [\"q\",0]\nNODE [\"q\",1]\nEQUATION [1,0,[true,[]],null,"
            + all_support + "]\nEND\n";
        const auto quantity_candidate = *compile_source(
            quantity_source, identify(quantity_source), quantity_dsl).candidate;
        const auto quantities = materialize_source(quantity_source, quantity_candidate, quantity_context);
        check(quantities.members.size() == 2);
        check(quantities.members[0].kind == ObjectKind::Claim && quantities.members[1].kind == ObjectKind::Claim);
        check(source_member_json(quantities.module, quantities.context, quantities.members[0])
              == "[0,0," + all_support + "]");
        check(source_member_json(quantities.module, quantities.context, quantities.members[1])
              == "[1,0,[true,[]],null," + all_support + "]");
        check(quantities.module.payload == quantity_candidate.payload);
        auto bad_member = compiled.members[0];
        const auto replace_pointer = [&](std::string_view value) {
            bad_member = compiled.members[0];
            bad_member.payload.assign(value.begin(), value.end());
        };
        replace_pointer("/theory/rules/999");
        rejects(ErrorCode::Schema, [&] {
            (void)source_member_json(compiled.module, compiled.context, bad_member);
        });
        replace_pointer("/theory/rules/00");
        rejects(ErrorCode::Schema, [&] {
            (void)source_member_json(compiled.module, compiled.context, bad_member);
        });
        replace_pointer("/theory/numbers/0");
        rejects(ErrorCode::Schema, [&] {
            (void)source_member_json(compiled.module, compiled.context, bad_member);
        });
        bad_member = compiled.members[0];
        bad_member.visibility = Visibility::Public;
        rejects(ErrorCode::Integrity, [&] {
            (void)source_member_json(compiled.module, compiled.context, bad_member);
        });
        rejects(ErrorCode::Integrity, [&] {
            (void)source_member_json(revised.module, compiled.context, compiled.members[0]);
        });
        rejects(ErrorCode::Integrity, [&] {
            (void)source_member_json(compiled.module, revised.context, compiled.members[0]);
        });
        auto bad_module = compiled.module;
        bad_module.links.erase(bad_module.links.begin());
        rejects(ErrorCode::Integrity, [&] {
            (void)source_member_json(bad_module, compiled.context, compiled.members[0]);
        });
        rejects(ErrorCode::Budget, [&] {
            (void)materialize_source(native_source, native_candidate, context, {3, 16 * 1024 * 1024});
        });
        rejects(ErrorCode::Budget, [&] {
            (void)materialize_source(native_source, native_candidate, context, {512, 1});
        });
        std::size_t package_bytes = encode_object(compiled.context).size() + encode_object(compiled.module).size();
        for (const auto& member : compiled.members) package_bytes += encode_object(member).size();
        const auto exact_package = materialize_source(native_source, native_candidate, context,
                                                       {4, package_bytes});
        check(exact_package.members == compiled.members);
        rejects(ErrorCode::Budget, [&] {
            (void)materialize_source(native_source, native_candidate, context, {4, package_bytes - 1});
        });
        (void)journal.invalidate(context_id, "Host registry context withdrawn");
        check(journal.assessment(module_id)->state == KnowledgeState::Invalidated);
        for (const auto& member : compiled.members) {
            check(journal.assessment(identify(member))->state == KnowledgeState::Invalidated);
        }
        check(!journal.assessment(identify(native_source)));
        std::cout << "source compiler tests passed: " << assertions << " deterministic assertions\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
