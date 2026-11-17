#include "problem_transaction.hpp"
#include "problem_wire.hpp"

#include <json-c/json.h>

#include <memory>
#include <stdexcept>

namespace axiom {
std::vector<ProblemResult> execute_problem_batch(const CompiledProblemBatch& batch) {
    const auto require = [](bool condition, const char* message) {
        if (!condition) throw std::invalid_argument(message);
    };
    require(!batch.transactions.empty() && batch.transactions.size() <= 8,
        "Invalid signed batch size");
    const auto& input = batch.interpretation_record;
    require(!input.empty() && input.size() <= 1048576 && input.find('\0') == std::string::npos,
        "Invalid batch manifest size");
    const auto tokener = std::unique_ptr<json_tokener,decltype(&json_tokener_free)>(
        json_tokener_new_ex(48),json_tokener_free);
    json_tokener_set_flags(tokener.get(),JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
    const auto manifest = std::unique_ptr<json_object,decltype(&json_object_put)>(
        json_tokener_parse_ex(tokener.get(),input.data(),static_cast<int>(input.size())),json_object_put);
    require(manifest && json_tokener_get_error(tokener.get()) == json_tokener_success
        && json_object_get_type(manifest.get()) == json_type_object,"Invalid batch manifest JSON");
    for (auto i = json_tokener_get_parse_end(tokener.get()); i < input.size(); ++i)
        require(input[i] == ' ' || input[i] == '\t' || input[i] == '\r' || input[i] == '\n',
            "Trailing batch manifest JSON");
    const auto field = [&](const char* key) {
        json_object* result = nullptr;
        require(json_object_object_get_ex(manifest.get(),key,&result),"Missing batch manifest field");
        return result;
    };
    const auto work = field("total_work");
    require(json_object_get_type(work) == json_type_int
        && json_object_get_int64(work) > 0 && json_object_get_int64(work) <= 4000000,
        "Invalid aggregate batch allowance");
    const auto& first = batch.transactions.front().request;
    const ProblemSourceCatalog catalog(first.context,
        {static_cast<std::uint64_t>(json_object_get_int64(work)),first.limits.steps});
    const auto transaction = field("transaction");
    require(json_object_get_type(transaction) == json_type_object,"Invalid batch transaction");
    // Recompile the entire fixed interpretation before any execution. This
    // checks shared vocabulary, slot order, contexts, budgets and all origins.
    const auto expected = compile_problem_transaction_batch(
        json_object_to_json_string_ext(transaction,JSON_C_TO_STRING_PLAIN),catalog);
    require(expected.theory_digest == batch.theory_digest
        && expected.transactions.size() == batch.transactions.size(),"Batch theory mismatch");
    for (std::size_t i = 0; i < expected.transactions.size(); ++i) {
        const auto& original = expected.transactions[i];
        const auto& supplied = batch.transactions[i];
        require(original.request == supplied.request && original.request_digest == supplied.request_digest
            && original.interpretation_record == supplied.interpretation_record,"Altered batch request");
    }
    // A higher-level source plan is retained as provenance, not certified here
    // as an English interpretation. All native manifest fields must match.
    json_object_object_del(manifest.get(),"source_plan");
    require(expected.interpretation_record
        == json_object_to_json_string_ext(manifest.get(),JSON_C_TO_STRING_PLAIN),
        "Altered batch manifest");
    std::vector<ProblemResult> results;
    results.reserve(batch.transactions.size());
    for (const auto& compiled : batch.transactions) {
        const auto packet = encode_problem_request(compiled.request);
        auto result = decode_problem_result(run_problem_binary(packet));
        require(verify_problem_result(compiled.request,result),"Batch certificate verification failed");
        results.push_back(std::move(result));
    }
    return results;
}
} // namespace axiom
