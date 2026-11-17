#pragma once

#include "problem_transaction.hpp"

namespace axiom {

// source-signed-theory-v1: occurrence-bound declarations, explicit signed Horn
// clauses and an ordered, non-deduplicated list of ground goals.
// This compiles representation only; it does not certify English meaning.
CompiledProblemBatch compile_problem_signed_plan(const std::string& json,
    const ProblemSourceCatalog& catalog);
std::string problem_signed_plan_schema(const ProblemSourceCatalog& catalog);

namespace signed_plan_detail {
// Shared structural admission for controller prefixes. All six fields exist;
// declaration/goal lists may still be empty. No execution or batch compilation.
void validate_prefix(const std::string& json, const ProblemSourceCatalog& catalog);
// Strict bounded JSON, including duplicate object-key rejection.
void validate_json(const std::string& json);
}

} // namespace axiom
