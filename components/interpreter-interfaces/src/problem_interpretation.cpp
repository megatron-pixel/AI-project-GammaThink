#include "problem_interpretation.hpp"
#include "problem_semantic.hpp"
#include "problem_source_semantic.hpp"
#include "problem_transaction_semantic.hpp"
#include "problem_transaction_objects.hpp"
#include "problem_quantity_frame_semantic.hpp"
#include "problem_quantity_decisions.hpp"
#include "semantic.hpp"

#include <json-c/json.h>

#include <memory>

namespace axiom {

CompiledProblemTransaction interpret_problem_source(const ProblemContext& context,
    ProblemInterpretationArm arm, MeteredSemanticSession& session) {
    const bool objects = arm == ProblemInterpretationArm::ObjectTyped || arm == ProblemInterpretationArm::ObjectStaged;
    const bool staged = arm == ProblemInterpretationArm::Staged || arm == ProblemInterpretationArm::ObjectStaged;
    const auto budget = arm == ProblemInterpretationArm::QuantityDecisions
        ? SemanticBudgetProfile::QuantityDecisionsExperimental : SemanticBudgetProfile::Standard;
    if (session.staged() != staged || session.calls() != 0 || session.budget_profile() != budget)
        throw std::invalid_argument("Interpretation arm requires a fresh matching session");
    session.ensure_time();
    const ProblemSourceCatalog catalog(context);
    const auto complete = [&](const std::string& prompt, const std::string& input, const std::string& schema) {
        const auto result = session.complete(prompt,input,schema);
        if (result.finish_reason != "stop") throw IncompleteInterpretation(result.content);
        return result.content;
    };
    CompiledProblemTransaction result;
    switch (arm) {
    case ProblemInterpretationArm::QuantityDecisions:
        result = interpret_problem_quantity_decisions(catalog,session);
        break;
    case ProblemInterpretationArm::QuantityFrame:
        result = compile_problem_quantity_frame(parse_problem_quantity_frame(
            complete(problem_quantity_frame_prompt(),catalog.input(),problem_quantity_frame_schema(catalog)),
            catalog),catalog);
        break;
    case ProblemInterpretationArm::Quote:
        result.request = parse_problem_proposal(complete(problem_semantic_prompt(ProblemSelectorMode::QuoteV1),
            problem_semantic_input(context),problem_proposal_schema(ProblemSelectorMode::QuoteV1)),
            context,catalog.limits(),ProblemSelectorMode::QuoteV1);
        break;
    case ProblemInterpretationArm::Handles:
        result.request = parse_problem_source_proposal(complete(problem_source_semantic_prompt(),
            catalog.input(),problem_source_proposal_schema(catalog)),catalog);
        break;
    case ProblemInterpretationArm::Typed:
    case ProblemInterpretationArm::ObjectTyped: {
        const auto content = complete(objects ? problem_object_transaction_prompt() : problem_transaction_prompt(),
            catalog.input(),objects ? problem_object_transaction_schema(catalog) : problem_transaction_schema(catalog));
        result = compile_problem_transaction(objects
            ? lower_problem_object_transaction(content,catalog) : content,catalog);
        break;
    }
    case ProblemInterpretationArm::Staged:
    case ProblemInterpretationArm::ObjectStaged: {
        const auto declarations = complete(
            objects ? problem_object_transaction_prompt(ProblemTransactionStage::Declarations)
                : problem_transaction_prompt(ProblemTransactionStage::Declarations),catalog.input(),
            objects ? problem_object_transaction_schema(catalog,ProblemTransactionStage::Declarations)
                : problem_transaction_schema(catalog,ProblemTransactionStage::Declarations));
        const auto draft = prepare_problem_declarations(objects
            ? lower_problem_object_transaction(declarations,catalog,ProblemTransactionStage::Declarations)
            : declarations,catalog);
        session.ensure_time();
        const auto input = std::unique_ptr<json_object, decltype(&json_object_put)>(
            json_object_new_object(),json_object_put);
        json_object_object_add(input.get(),"snapshot",json_object_new_string(catalog.snapshot().c_str()));
        json_object_object_add(input.get(),"catalog",json_tokener_parse(catalog.input().c_str()));
        if (objects) {
            const auto original = std::unique_ptr<json_object, decltype(&json_object_put)>(
                json_tokener_parse(declarations.c_str()),json_object_put);
            json_object_object_add(input.get(),"declarations",
                json_object_get(json_object_object_get(original.get(),"declarations")));
        } else json_object_object_add(input.get(),"declarations",json_tokener_parse(draft.declarations().c_str()));
        auto remaining = json_object_new_object();
        json_object_object_add(input.get(),"remaining_budget",remaining);
        json_object_object_add(remaining,"prompt_tokens",json_object_new_int64(6144-session.prompt_tokens()));
        json_object_object_add(remaining,"output_tokens",json_object_new_int(1024));
        json_object_object_add(remaining,"generation_calls",json_object_new_int(1));
        json_object_object_add(remaining,"wall_ms",json_object_new_int64(session.remaining_ms()));
        const auto assertions = complete(
            objects ? problem_object_transaction_prompt(ProblemTransactionStage::Assertions)
                : problem_transaction_prompt(ProblemTransactionStage::Assertions),
            json_object_to_json_string_ext(input.get(),JSON_C_TO_STRING_PLAIN),
            objects ? problem_object_transaction_schema(catalog,ProblemTransactionStage::Assertions)
                : problem_transaction_schema(catalog,ProblemTransactionStage::Assertions));
        result = commit_problem_assertions(objects
            ? lower_problem_object_transaction(assertions,catalog,ProblemTransactionStage::Assertions)
            : assertions,draft,catalog);
        break;
    }
    default: throw std::invalid_argument("Invalid interpretation arm");
    }
    if (result.request_digest.empty())
        result.request_digest = evidence_digest("interpreted-problem-v1\n"+catalog.snapshot()+"\n"
            +serialize_problem_proposal(result.request));
    session.ensure_time();
    return result;
}

} // namespace axiom
