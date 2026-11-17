#pragma once

#include "goal_planner.hpp"
#include "journal.hpp"
#include "model_manager.hpp"

#include <thread>

namespace axiom::knowledge {

enum class MeasurementBasis { Direct = 1, Simulation };
enum class ExperimentStop { Complete = 1, ResourceLimited, Unavailable };
enum class ExperimentMatch { Matched = 1, NotMatched, Mixed, Incomplete };
struct ExperimentProtocol {
    std::string name, revision, instrument;
    // Operational observation predicates, not replacement archive meaning IR.
    std::vector<GoalPredicate> expected;
    std::size_t trials = 1;
};
struct ExperimentLimits {
    std::size_t encoded_bytes = 16 * 1024 * 1024;
    std::chrono::milliseconds time{30000};
    std::uint64_t resident_bytes = 2999999999ULL;
};

// Host-selected native instrumentation only. Implementations bound allocation
// before returning and obey the cooperative deadline. Model/archive text
// cannot construct this capability or select arbitrary executable commands.
class ExperimentInstrument {
public:
    virtual ~ExperimentInstrument() = default;
    virtual std::string_view id() const noexcept = 0;
    virtual MeasurementBasis basis() const noexcept = 0;
    virtual GoalConditions sample(std::chrono::steady_clock::time_point deadline,
                                  std::size_t maximum_bytes) = 0;
};
class ProcessMemoryInstrument final : public ExperimentInstrument {
public:
    std::string_view id() const noexcept override { return "process-memory-v1"; }
    MeasurementBasis basis() const noexcept override { return MeasurementBasis::Direct; }
    GoalConditions sample(std::chrono::steady_clock::time_point deadline, std::size_t maximum_bytes) override;
};

class ExperimentRunner;
class ExperimentPermit {
    friend class ExperimentRunner;
    ExperimentPermit() = default;
    std::shared_ptr<const std::uint8_t> owner_;
    std::shared_ptr<ExperimentInstrument> instrument_;
    ObjectId experiment_;
    ObjectId scope_;
    Digest run_nonce_{};
    ExperimentLimits limits_;
    MeasurementBasis basis_ = MeasurementBasis::Simulation;
    bool used_ = false;
public:
    ExperimentPermit(const ExperimentPermit&) = delete;
    ExperimentPermit& operator=(const ExperimentPermit&) = delete;
    ExperimentPermit(ExperimentPermit&&) = default;
    ExperimentPermit& operator=(ExperimentPermit&&) = delete;
};
struct ExperimentReview {
    ObjectId experiment, scope, receipt;
    std::vector<ObjectId> observations;
    MeasurementBasis basis = MeasurementBasis::Simulation;
    ExperimentStop stop = ExperimentStop::Complete;
    ExperimentMatch match = ExperimentMatch::Incomplete;
    std::size_t attempted_trials = 0;
    // Mechanical criterion matching never certifies a hypothesis's meaning,
    // instrument calibration, empirical truth or a scientific discovery.
    bool domain_review_required = true;
};

// Separate proposal -> host permit -> actual observation -> independent
// record review. Ordered jointly required hypotheses remain provisional.
// No model inference, fact promotion, logical certificate or publication.
class ExperimentRunner {
public:
    ExperimentRunner(Journal& journal, neural::NativeModelManager& model) : journal_(journal), model_(model) {}
    ExperimentRunner(const ExperimentRunner&) = delete;
    ExperimentRunner& operator=(const ExperimentRunner&) = delete;
    ObjectId propose(std::span<const ObjectId> jointly_required, const ExperimentProtocol& protocol);
    ExperimentPermit authorize(const ObjectId& experiment, std::shared_ptr<ExperimentInstrument> instrument,
                               ExperimentLimits limits = {});
    ExperimentReview run(ExperimentPermit& permit);
    ExperimentReview review(const ObjectId& experiment, const ObjectId& receipt);
private:
    Journal& journal_;
    neural::NativeModelManager& model_;
    const std::thread::id thread_ = std::this_thread::get_id();
    const std::shared_ptr<const std::uint8_t> owner_ = std::make_shared<const std::uint8_t>(0);
    void current() const;
};

} // namespace axiom::knowledge
