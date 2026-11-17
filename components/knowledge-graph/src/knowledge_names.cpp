#include "knowledge.hpp"

namespace axiom::knowledge {

std::string_view name(ObjectKind value) {
    switch (value) {
        case ObjectKind::Claim:
            return "CLAIM";
        case ObjectKind::Fact:
            return "FACT";
        case ObjectKind::Rule:
            return "RULE";
        case ObjectKind::Assumption:
            return "ASSUMPTION";
        case ObjectKind::Hypothesis:
            return "HYPOTHESIS";
        case ObjectKind::Derivation:
            return "DERIVATION";
        case ObjectKind::Evidence:
            return "EVIDENCE";
        case ObjectKind::Goal:
            return "GOAL";
        case ObjectKind::Action:
            return "ACTION";
        case ObjectKind::Experiment:
            return "EXPERIMENT";
        case ObjectKind::TestResult:
            return "TEST_RESULT";
        case ObjectKind::Contradiction:
            return "CONTRADICTION";
        default:
            throw Error(ErrorCode::Schema, "Unknown enumeration value");
    }
}

std::string_view name(Relationship value) {
    switch (value) {
        case Relationship::Requires:
            return "REQUIRES";
        case Relationship::Implies:
            return "IMPLIES";
        case Relationship::Supports:
            return "SUPPORTS";
        case Relationship::Contradicts:
            return "CONTRADICTS";
        case Relationship::DependsOn:
            return "DEPENDS_ON";
        case Relationship::Produces:
            return "PRODUCES";
        case Relationship::Tests:
            return "TESTS";
        case Relationship::AlternativeTo:
            return "ALTERNATIVE_TO";
        default:
            throw Error(ErrorCode::Schema, "Unknown enumeration value");
    }
}

std::string_view name(KnowledgeState value) {
    switch (value) {
        case KnowledgeState::Provisional:
            return "PROVISIONAL";
        case KnowledgeState::Verified:
            return "VERIFIED";
        case KnowledgeState::Disputed:
            return "DISPUTED";
        case KnowledgeState::Invalidated:
            return "INVALIDATED";
        case KnowledgeState::Unresolved:
            return "UNRESOLVED";
        case KnowledgeState::Archived:
            return "ARCHIVED";
        default:
            throw Error(ErrorCode::Schema, "Unknown enumeration value");
    }
}

std::string_view name(LogicalValidity value) {
    switch (value) {
        case LogicalValidity::Unchecked:
            return "UNCHECKED";
        case LogicalValidity::Valid:
            return "VALID";
        case LogicalValidity::Invalid:
            return "INVALID";
        default:
            throw Error(ErrorCode::Schema, "Unknown enumeration value");
    }
}

std::string_view name(EvidenceAssessment value) {
    switch (value) {
        case EvidenceAssessment::Unassessed:
            return "UNASSESSED";
        case EvidenceAssessment::Supported:
            return "SUPPORTED";
        case EvidenceAssessment::Challenged:
            return "CHALLENGED";
        case EvidenceAssessment::Conflicting:
            return "CONFLICTING";
        default:
            throw Error(ErrorCode::Schema, "Unknown enumeration value");
    }
}

std::string_view name(Visibility value) {
    switch (value) {
        case Visibility::Private:
            return "PRIVATE";
        case Visibility::Public:
            return "PUBLIC";
        default:
            throw Error(ErrorCode::Schema, "Unknown enumeration value");
    }
}

} // namespace axiom::knowledge
