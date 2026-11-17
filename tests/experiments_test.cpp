#include "experiments.hpp"
#include "hypotheses.hpp"
#include "core.hpp"

#include <iostream>
#include <json-c/json.h>
#include <unistd.h>

namespace {
using namespace axiom;
using namespace axiom::knowledge;
std::size_t assertions = 0;
void check(bool value) {
    ++assertions;
    if (!value) throw std::runtime_error("Experiment assertion " + std::to_string(assertions));
}
template<class Function> void rejects(ErrorCode code, Function function) {
    try { function(); } catch (const Error& error) { check(error.code == code); return; }
    throw std::runtime_error("Expected experiment rejection");
}
struct Directory {
    std::filesystem::path path;
    Directory() {
        auto pattern = (std::filesystem::temp_directory_path() / "experiments-XXXXXX").string();
        const auto* created = mkdtemp(pattern.data());
        if (!created) throw std::runtime_error("Cannot create experiment test directory");
        path = created;
    }
    ~Directory() { std::filesystem::remove_all(path); }
};
struct Instrument final : ExperimentInstrument {
    std::vector<GoalConditions> values;
    std::size_t calls = 0;
    std::optional<ErrorCode> stop;
    std::string_view id() const noexcept override { return "synthetic-instrument-v1"; }
    MeasurementBasis basis() const noexcept override { return MeasurementBasis::Simulation; }
    GoalConditions sample(std::chrono::steady_clock::time_point, std::size_t) override {
        const auto index = calls++;
        if (index < values.size()) return values[index];
        if (stop) throw Error(*stop, "Synthetic instrument boundary");
        throw Error(ErrorCode::Unavailable, "Synthetic observation missing");
    }
};
ExperimentProtocol protocol(std::size_t trials = 1) {
    return {"bounded-prediction", "1", "synthetic-instrument-v1",
        {{"value", GoalComparison::Greater, std::int64_t{1}}}, trials};
}
void alter(Object& value, const char* key, json_object* replacement) {
    std::unique_ptr<json_object, decltype(&json_object_put)> fields(
        json_tokener_parse(std::string(value.payload.begin(), value.payload.end()).c_str()), json_object_put);
    json_object_object_add(fields.get(), key, replacement);
    const std::string encoded = json_object_to_json_string_ext(fields.get(), JSON_C_TO_STRING_PLAIN);
    value.payload.assign(encoded.begin(), encoded.end());
}
Object formal_goal() {
    Object request; request.kind = ObjectKind::Goal; request.format = native_request_format;
    request.payload = encode_request(compile_proof(parse_module(
        "axioms 1 experiments 1\nrule a & a -> g | repeated-joint-premise\n"), "g"));
    return request;
}
}

int main(int argc, char** argv) {
    try {
        if (argc == 3 && std::string_view(argv[1]) == "--fixture") {
            Journal journal(std::filesystem::path(argv[2]) / "journal.db");
            std::cout << "{\"goal\":\"" << hex(journal.put(formal_goal())) << "\"}\n";
            return 0;
        }
        if (argc != 1) throw std::runtime_error("Unsupported experiment fixture arguments");
        Directory directory;
        const auto database = directory.path / "journal.db";
        neural::NativeModelManager model;
        ObjectId physical_experiment, physical_result, hypothesis;
        Object original_hypothesis;
        {
            Journal journal(database);
            const auto request = formal_goal();
            journal.put(request);
            const auto proposed = propose_hypotheses(request);
            check(proposed.alternatives.size() == 1 && proposed.alternatives[0].jointly_required.size() == 2);
            check(proposed.alternatives[0].jointly_required[0] == proposed.alternatives[0].jointly_required[1]);
            for (const auto& candidate : proposed.hypotheses) hypothesis = journal.put(candidate);
            original_hypothesis = *journal.get(hypothesis);
            ExperimentRunner runner(journal, model);
            auto physical = protocol();
            physical.name = "native-process-memory"; physical.instrument = "process-memory-v1";
            physical.expected = {{"resident_bytes", GoalComparison::Greater, std::int64_t{0}},
                {"peak_bytes", GoalComparison::Greater, std::int64_t{0}}};
            physical_experiment = runner.propose(proposed.alternatives[0].jointly_required, physical);
            const auto plan = *journal.get(physical_experiment);
            check(plan.kind == ObjectKind::Experiment && plan.visibility == Visibility::Private);
            check(plan.links.size() == 3 && plan.links[1] == Link{Relationship::Tests, hypothesis} &&
                plan.links[2] == Link{Relationship::Tests, hypothesis});
            auto permit = runner.authorize(physical_experiment, std::make_shared<ProcessMemoryInstrument>());
            const auto result = runner.run(permit); physical_result = result.receipt;
            check(result.basis == MeasurementBasis::Direct && result.stop == ExperimentStop::Complete);
            check(result.match == ExperimentMatch::Matched && result.observations.size() == 1);
            check(result.domain_review_required && !journal.assessment(hypothesis));
            check(journal.get(hypothesis) == original_hypothesis && journal.by_kind(ObjectKind::Fact).objects.empty());
            const auto replay = runner.review(physical_experiment, result.receipt);
            check(replay.match == result.match && replay.scope == result.scope && replay.observations == result.observations);
            rejects(ErrorCode::Permission, [&] { (void)runner.run(permit); });
            rejects(ErrorCode::Permission, [&] { (void)runner.authorize(physical_experiment, std::make_shared<Instrument>()); });
            ExperimentLimits tiny; tiny.resident_bytes = 1;
            rejects(ErrorCode::Budget, [&] { (void)runner.authorize(physical_experiment, std::make_shared<ProcessMemoryInstrument>(), tiny); });
            tiny = {}; tiny.encoded_bytes = 1;
            rejects(ErrorCode::Budget, [&] { (void)runner.authorize(physical_experiment, std::make_shared<ProcessMemoryInstrument>(), tiny); });
            auto empty = physical; empty.expected.clear();
            rejects(ErrorCode::Budget, [&] { (void)runner.propose({&hypothesis, 1}, empty); });
            rejects(ErrorCode::Unavailable, [&] { const ObjectId missing{Digest{1}}; (void)runner.propose({&missing, 1}, physical); });
            const auto goal = identify(request);
            rejects(ErrorCode::Integrity, [&] { (void)runner.propose({&goal, 1}, physical); });
        }
        {
            // Reopen all records; review has no instrument and cannot resample.
            Journal journal(database);
            ExperimentRunner runner(journal, model);
            const auto reviewed = runner.review(physical_experiment, physical_result);
            check(reviewed.match == ExperimentMatch::Matched && reviewed.domain_review_required);
            check(model.state() == neural::ModelState::Unloaded && model.metrics().file_bytes == 0);
            check(journal.get(hypothesis) == original_hypothesis && !journal.assessment(hypothesis));
            const auto experiment = runner.propose({&hypothesis, 1}, protocol(3));
            const auto run = [&](std::vector<GoalConditions> values, std::optional<ErrorCode> stop = {}) {
                auto instrument = std::make_shared<Instrument>(); instrument->values = std::move(values); instrument->stop = stop;
                auto permit = runner.authorize(experiment, instrument);
                ExperimentRunner foreign(journal, model);
                rejects(ErrorCode::Permission, [&] { (void)foreign.run(permit); });
                auto moved = std::move(permit);
                rejects(ErrorCode::Permission, [&] { (void)runner.run(permit); });
                const auto result = runner.run(moved);
                const auto calls = instrument->calls;
                const auto restored = runner.review(experiment, result.receipt);
                check(restored.match == result.match && restored.stop == result.stop && restored.observations == result.observations);
                check(instrument->calls == calls && result.basis == MeasurementBasis::Simulation);
                check(result.domain_review_required && !journal.assessment(hypothesis));
                return result;
            };
            const GoalConditions yes{{"value", std::int64_t{2}}}, no{{"value", std::int64_t{0}}},
                missing{{"unrelated", true}}, wrong_type{{"value", std::string("2")}};
            const auto matched = run({yes, yes, yes});
            check(matched.match == ExperimentMatch::Matched && matched.stop == ExperimentStop::Complete);
            check(run({no, no, no}).match == ExperimentMatch::NotMatched);
            check(run({yes, no, yes}).match == ExperimentMatch::Mixed);
            check(run({yes, missing, yes}).match == ExperimentMatch::Incomplete);
            check(run({yes, wrong_type, yes}).match == ExperimentMatch::Incomplete);
            const auto unavailable = run({yes}, ErrorCode::Unavailable);
            check(unavailable.stop == ExperimentStop::Unavailable && unavailable.match == ExperimentMatch::Incomplete);
            check(unavailable.attempted_trials == 2 && unavailable.observations.size() == 1);
            const auto limited = run({yes}, ErrorCode::Budget);
            check(limited.stop == ExperimentStop::ResourceLimited && limited.match == ExperimentMatch::Incomplete);
            const auto again = run({yes, yes, yes});
            check(again.receipt != matched.receipt && again.observations != matched.observations && again.scope != matched.scope);

            auto forged = *journal.get(matched.receipt);
            alter(forged, "empirical_truth_verified", json_object_new_boolean(true));
            const auto forged_id = journal.put(forged);
            rejects(ErrorCode::Integrity, [&] { (void)runner.review(experiment, forged_id); });
            forged = *journal.get(matched.receipt);
            alter(forged, "match", json_object_new_int(static_cast<int>(ExperimentMatch::NotMatched)));
            const auto wrong_match = journal.put(forged);
            rejects(ErrorCode::Integrity, [&] { (void)runner.review(experiment, wrong_match); });
            rejects(ErrorCode::Integrity, [&] { (void)runner.review(physical_experiment, matched.receipt); });

            auto instrument = std::make_shared<Instrument>(); instrument->values = {yes, yes, yes};
            auto pending = runner.authorize(experiment, instrument);
            Assessment withdrawn; withdrawn.state = KnowledgeState::Invalidated;
            withdrawn.checker = "synthetic-domain-review"; withdrawn.reason = "Requires explicit revalidation";
            journal.record_assessment(hypothesis, withdrawn);
            rejects(ErrorCode::Integrity, [&] { (void)runner.run(pending); });
            check(instrument->calls == 0);
            rejects(ErrorCode::Integrity, [&] { (void)runner.review(experiment, matched.receipt); });
            check(journal.get(hypothesis) == original_hypothesis && journal.by_kind(ObjectKind::Fact).objects.empty());
        }
        std::cout << "Experiment checks passed: " << assertions << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
