#include "core.hpp"
#include "binary_dispatch.hpp"
#include "gpu.hpp"
#include "library.hpp"
#include "semantic.hpp"
#include "relational.hpp"
#include "problem_semantic.hpp"
#include "problem_wire.hpp"
#include "translator.hpp"

#include <iomanip>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void evidence_report(const axiom::LibrarySearch& search) {
    std::cout << "retrieval only; evidence is not a proven axiom\n"
              << "archives searched: " << search.archives_searched
              << "; coverage complete: " << (search.complete ? "yes" : "no") << '\n';
    for (std::size_t i = 0; i < search.evidence.size(); ++i) {
        const auto& item = search.evidence[i];
        std::cout << "citation " << i + 1 << ": archive=" << std::quoted(item.archive)
                  << " path=" << std::quoted(item.path) << " title=" << std::quoted(item.title)
                  << "\nexcerpt: " << std::quoted(item.text) << '\n';
    }
    for (const auto& error : search.errors) {
        std::cout << "retrieval error: " << std::quoted(error) << '\n';
    }
}

void arity(const std::vector<std::string>& args, std::size_t count) {
    if (args.size() != count) {
        throw std::runtime_error("wrong argument count; use --help");
    }
}

std::string read_text(const std::string& path) {
    std::ifstream input(path,std::ios::binary);
    if (!input) throw std::runtime_error("Cannot open problem input: " + path);
    const auto bytes = axiom::read_binary(input);
    return {bytes.begin(),bytes.end()};
}

int ask(const std::vector<std::string>& args, bool binary) {
    if (args.size() < 2) throw std::runtime_error("ask requires one quoted question");
    std::string module;
    std::string library;
    bool gpu = false;
    bool semantic = false;
    for (std::size_t index = 2; index < args.size(); ++index) {
        if (args[index] == "--semantic") {
            if (semantic) throw std::runtime_error("Duplicate --semantic option");
            semantic = true;
        } else if (args[index] == "--gpu") {
            if (gpu) throw std::runtime_error("Duplicate --gpu option");
            gpu = true;
        } else if (args[index] == "--module" || args[index] == "--library") {
            auto& target = args[index] == "--module" ? module : library;
            if (!target.empty() || index + 1 == args.size() || args[index + 1].empty()) {
                throw std::runtime_error("Missing or duplicate English bridge option");
            }
            target = args[++index];
        } else {
            throw std::runtime_error("Unknown English bridge option");
        }
    }
    const auto translated = semantic ? axiom::interpret_local(args[1])
                                     : axiom::translate_english(args[1]);
    if (semantic) {
        std::cerr << "learned interpretation; English meaning is not independently verified\n";
        for (const auto& part : translated.command) std::cerr << std::quoted(part) << ' ';
        std::cerr << "goal=" << std::quoted(translated.goal)
                  << " query=" << std::quoted(translated.query)
                  << " quantity=" << std::quoted(translated.quantity)
                  << " unit=" << std::quoted(translated.unit)
                  << " expression=" << std::quoted(translated.expression) << '\n';
    }
    if (binary && !library.empty()) {
        throw std::runtime_error("Binary compile does not perform retrieval; use ask without compile");
    }
    if (translated.kind == axiom::EnglishKind::Research
        || translated.kind == axiom::EnglishKind::LibraryCalculation) {
        if (binary || library.empty() || !module.empty() || gpu) {
            throw std::runtime_error("Research/library questions require --library, without --module or --gpu");
        }
        const auto search = axiom::search_library(library, translated.query);
        if (translated.kind == axiom::EnglishKind::Research) {
            evidence_report(search);
        } else {
            const auto result = axiom::calculate_from_library(translated, search);
            switch (result.status) {
            case axiom::GroundingStatus::Unknown:
                std::cout << "grounding: UNKNOWN\n";
                break;
            case axiom::GroundingStatus::Conflict:
                std::cout << "grounding: CONFLICTING_SOURCE_VALUES\n";
                break;
            case axiom::GroundingStatus::Conditional:
                std::cout << "grounding: CONDITIONAL\n"
                          << "arithmetic is checked; source truth is not established\n";
                break;
            }
            std::cout << "quantity: " << std::quoted(translated.quantity)
                      << "\nsource unit: " << std::quoted(translated.unit)
                      << "\nquery coverage complete: " << (result.coverage_complete ? "yes" : "no") << '\n';
            for (const auto& premise : result.premises) {
                std::cout << "source value: " << premise.value.get_str()
                          << " archive=" << std::quoted(premise.source.archive)
                          << " path=" << std::quoted(premise.source.path)
                          << " excerpt-offset=" << premise.offset
                          << " excerpt-length=" << premise.length
                          << "\nsource statement: "
                          << std::quoted(premise.source.text.substr(premise.offset, premise.length)) << '\n';
            }
            if (result.status == axiom::GroundingStatus::Conditional) {
                std::cout << axiom::render(axiom::decode_result(result.binary_result));
            }
            evidence_report(search);
        }
        return search.complete && search.errors.empty() ? 0 : 1;
    }
    axiom::Request request;
    if (translated.kind == axiom::EnglishKind::Calculation) {
        if (!module.empty() || !library.empty() || gpu) {
            throw std::runtime_error("Pure math questions do not accept module, library, or GPU options");
        }
        request = axiom::compile(translated.command);
    } else {
        if (module.empty()) throw std::runtime_error("English proof questions require --module");
        request = axiom::compile_proof(axiom::load_module(module), translated.goal);
        request.gpu = gpu;
    }
    const auto wire = axiom::encode_request(request);
    if (binary) {
        axiom::write_binary(std::cout, wire);
        return 0;
    }
    const auto result = axiom::decode_result(axiom::run_binary(wire, axiom::gpu_rule_matches));
    std::cout << axiom::render(result);
    if (result.operation == axiom::Operation::Prove && result.status == axiom::Status::Unknown
        && !library.empty()) {
        evidence_report(axiom::search_library(library, translated.query));
    }
    return 0;
}
} // namespace

int main(int argc, char** argv) {
    try {
        std::vector<std::string> args(argv + 1, argv + argc);
        if (args.empty() || args.front() == "--help" || args.front() == "help") {
            if (!args.empty()) {
                arity(args, 1);
            }
            std::cout << axiom::help() << axiom::translator_help();
            std::cout << "  ask QUESTION --semantic: experimental local CPU interpreter\n"
                         "    requires scripts/interpreter-server.sh; meaning remains unchecked\n";
            std::cout << "  compile-rel MODULE GOAL: compile finite relational axioms to AXRM v2\n"
                         "  prove-rel MODULE GOAL: run a checked finite relational proof on CPU\n";
            std::cout << "  compile-problem CONTEXT.json PROPOSAL.json: compile admitted AXRM v3\n"
                         "  check-problem CONTEXT.json PROPOSAL.json: execute and independently check v3\n"
                         "  revise-problem CONTEXT.json PREVIOUS.json CORRECTED.json REASON:\n"
                         "    log a reviewer-supplied correction and recompute without old conclusions\n"
                         "  ask-problem CONTEXT.json: opt-in local learned v3 proposal (CPU)\n"
                         "    append --quote-v1 for exact quotation selectors instead of byte offsets\n";
            std::cout << "  library-context REGISTRY.json LIBRARY QUESTION QUERY: export bounded Kiwix source views\n";
            return 0;
        }
        const auto command = args.front();
        if (command == "ask") return ask(args, false);
        if (command == "library-context") {
            arity(args,5);
            auto context = axiom::parse_problem_context(read_text(args[1]));
            if (!context.views.empty()) throw std::runtime_error("Library registry context must have no evidence views");
            if (args[3].empty() || args[3].size() > 4096) {
                throw std::runtime_error("Library question must contain 1 to 4096 bytes");
            }
            const auto search = axiom::search_library(args[2],args[4],3,1024);
            context.views.push_back(axiom::make_evidence_view("question",axiom::PremiseBasis::Question,
                args[3],"","question","literal-v1"));
            for (std::size_t i = 0; i < search.evidence.size(); ++i) {
                const auto& evidence = search.evidence[i];
                auto id = "source_"+std::to_string(i+1)+": "+evidence.title;
                if (id.size() > 128) {
                    std::size_t end = 128;
                    while ((static_cast<unsigned char>(id[end])&0xc0) == 0x80) --end;
                    id.resize(end);
                }
                context.views.push_back(axiom::make_evidence_view(id,axiom::PremiseBasis::Source,
                    evidence.text,evidence.archive,evidence.path,"libzim-text-math-excerpt-v2"));
            }
            std::cerr << "retrieval_context archives=" << search.archives_searched
                      << " excerpts=" << search.evidence.size() << " coverage_complete=" << search.complete
                      << "; retrieved text is not a proven axiom\n";
            for (const auto& error : search.errors) std::cerr << "retrieval error: " << error << '\n';
            std::cout << axiom::serialize_problem_context(context) << '\n';
            return search.complete && search.errors.empty() ? 0 : 1;
        }
        if (command == "revise-problem") {
            const bool quotes = args.back() == "--quote-v1";
            if (quotes) args.pop_back();
            arity(args,5);
            const auto mode = quotes ? axiom::ProblemSelectorMode::QuoteV1 : axiom::ProblemSelectorMode::OffsetV1;
            const auto context = axiom::parse_problem_context(read_text(args[1]));
            const auto previous = axiom::parse_problem_proposal(read_text(args[2]),context,{},mode);
            const auto corrected = axiom::parse_problem_proposal(read_text(args[3]),context,{},mode);
            axiom::ProblemCorrectionSession session(previous,2);
            session.correct(corrected.proposal,args[4]);
            std::cerr << "reviewer-supplied correction; source meaning remains unchecked\n";
            for (std::size_t i = 0; i < session.attempts().size(); ++i) {
                const auto& attempt = session.attempts()[i];
                std::cerr << "correction_attempt=" << i
                          << " status=" << static_cast<unsigned>(attempt.result.status)
                          << " work=" << attempt.result.work
                          << " invalidated_previous_steps="
                          << (i ? session.attempts()[i-1].result.steps.size() : 0)
                          << " reason=" << std::quoted(attempt.correction_reason) << '\n';
            }
            std::cout << axiom::render_problem(session.attempts().back().result);
            return 0;
        }
        if (command == "compile-problem" || command == "check-problem" || command == "ask-problem") {
            const bool quotes = args.back() == "--quote-v1";
            if (quotes) args.pop_back();
            const auto mode = quotes ? axiom::ProblemSelectorMode::QuoteV1 : axiom::ProblemSelectorMode::OffsetV1;
            arity(args,command == "ask-problem" ? 2 : 3);
            const auto context = axiom::parse_problem_context(read_text(args[1]));
            std::string proposal;
            if (command == "ask-problem") {
                axiom::verify_local_interpreter_context(4096);
                proposal = axiom::interpret_problem_json(context,mode);
                std::cerr << "learned unsolved proposal; English meaning remains unchecked\n"
                          << proposal << '\n';
            } else proposal = read_text(args[2]);
            const auto request = axiom::parse_problem_proposal(proposal,context,{},mode);
            const auto wire = axiom::encode_problem_request(request);
            if (command == "compile-problem") axiom::write_binary(std::cout,wire);
            else std::cout << axiom::render_binary_result(axiom::run_request_binary(wire));
            return 0;
        }
        if (command == "compile-rel" || command == "prove-rel") {
            arity(args, 3);
            const auto request = axiom::compile_relational(axiom::load_relational_module(args[1]), args[2]);
            const auto wire = axiom::encode_relational_request(request);
            if (command == "compile-rel") axiom::write_binary(std::cout, wire);
            else std::cout << axiom::render_binary_result(axiom::run_request_binary(wire));
            return 0;
        }
        if (command == "gpu-test") {
            arity(args, 1);
            const auto report = axiom::test_gpu_rules();
            std::cout << "GPU kernel parity passed: " << std::quoted(report.device)
                      << "; rules tested: " << report.rules_tested
                      << "; elapsed_ms: " << report.elapsed_ms << '\n';
            return 0;
        }
        if (command == "search") {
            arity(args, 3);
            const auto search = axiom::search_library(args[1], args[2]);
            evidence_report(search);
            return search.complete && search.errors.empty() ? 0 : 1;
        }
        if (command == "library-check") {
            arity(args, 2);
            axiom::check_library(args[1], std::cout);
            return 0;
        }
        if (command == "compile") {
            args.erase(args.begin());
            if (!args.empty() && args.front() == "ask") return ask(args, true);
            axiom::write_binary(std::cout, axiom::encode_request(axiom::compile(args)));
            return 0;
        }
        if (command == "run") {
            arity(args, 1);
            axiom::write_binary(std::cout, axiom::run_request_binary(axiom::read_binary(std::cin), axiom::gpu_rule_matches));
            return 0;
        }
        if (command == "decode") {
            arity(args, 1);
            std::cout << axiom::render_binary_result(axiom::read_binary(std::cin));
            return 0;
        }
        std::string library;
        if (command == "prove") {
            std::vector<std::string> formal;
            for (std::size_t i = 0; i < args.size(); ++i) {
                if (i >= 3 && args[i] == "--library") {
                    if (!library.empty() || i + 1 == args.size() || args[i + 1].empty()) {
                        throw std::runtime_error("invalid or duplicate --library option");
                    }
                    library = args[++i];
                } else {
                    formal.push_back(args[i]);
                }
            }
            args = std::move(formal);
        }
        const auto request = axiom::encode_request(axiom::compile(args));
        const auto result = axiom::decode_result(axiom::run_binary(request, axiom::gpu_rule_matches));
        std::cout << axiom::render(result);
        if (result.operation == axiom::Operation::Prove && result.status == axiom::Status::Unknown &&
            !library.empty()) {
            auto query = args[2];
            if (query.starts_with('!')) query.erase(0, 1);
            for (char& character : query) {
                if (character == '.' || character == '_') character = ' ';
            }
            evidence_report(axiom::search_library(library, query));
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 2;
    }
}
