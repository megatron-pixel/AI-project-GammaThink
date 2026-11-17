#pragma once

#include "problem_sources.hpp"

#include <utility>

namespace axiom {

struct CompiledProblemTransaction {
    ProblemRequest request;
    std::string request_digest;
    std::string interpretation_record;
};

struct CompiledProblemBatch {
    std::vector<CompiledProblemTransaction> transactions;
    std::string theory_digest;
    std::string interpretation_record;
};

class ProblemDeclarationDraft {
public:
    const std::string& snapshot() const { return snapshot_; }
    const std::string& declarations() const { return declarations_; }
private:
    std::string snapshot_, declarations_;
    ProblemDeclarationDraft(std::string snapshot, std::string declarations)
        : snapshot_(std::move(snapshot)), declarations_(std::move(declarations)) {}
    friend ProblemDeclarationDraft prepare_problem_declarations(
        const std::string&, const ProblemSourceCatalog&);
};

// These functions compile model choices; they do not certify English meaning.
// Preparation never executes a partial theory or supplies solver feedback.
ProblemDeclarationDraft prepare_problem_declarations(const std::string& json,
    const ProblemSourceCatalog& catalog);
CompiledProblemTransaction commit_problem_assertions(const std::string& json,
    const ProblemDeclarationDraft& draft, const ProblemSourceCatalog& catalog);
CompiledProblemTransaction compile_problem_transaction(const std::string& json,
    const ProblemSourceCatalog& catalog);

// Signed-theory-only transaction with assertions.goals instead of assertions.goal.
// Every goal is an ordered ground atom with question-only supports. All goals
// share one pre-interned theory; compilation admits every request before return.
// The catalog's total work allowance is partitioned across the 1..8 goal slots.
CompiledProblemBatch compile_problem_transaction_batch(const std::string& json,
    const ProblemSourceCatalog& catalog);
// Executes every admitted slot once and verifies against its original request.
// No proof result changes the interpretation or the remaining goal list.
std::vector<ProblemResult> execute_problem_batch(const CompiledProblemBatch& batch);

} // namespace axiom
