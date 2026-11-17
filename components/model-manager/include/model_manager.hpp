#pragma once

#include "knowledge.hpp"

#include <chrono>
#include <filesystem>
#include <limits>
#include <memory>

namespace axiom::neural {

enum class ModelState { Unloaded, Loading, Ready, Inferencing, Unloading, Failed };
enum class ModelRole { Interpreter = 1, Explanation, KnowledgeExtraction, CodeGeneration };
enum class GenerationStop { EndOfGeneration, TokenLimit };
struct ProcessMemory {
    std::uint64_t resident_bytes = 0, lifetime_peak_bytes = 0;
};
ProcessMemory process_memory();

struct ModelSpec {
    std::string id;
    std::filesystem::path path;
    knowledge::Digest expected_sha256{};
    ModelRole role = ModelRole::Interpreter;
    bool enable_thinking = false;
};
struct ModelLimits {
    // Count every retained weight; the deployment ceiling is bytes, not parameters.
    // A host may still impose a tighter parameter bound.
    std::uint64_t retained_parameters = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t resident_bytes = 2999999999ULL;
    std::uint64_t file_bytes = 2999999999ULL;
    std::uint32_t context_tokens = 1024, batch_tokens = 64, threads = 2;
    std::size_t input_bytes = 262144, output_bytes = 65536;
    std::chrono::milliseconds phase_time{180000};
};
struct ModelMetrics {
    std::uint64_t file_bytes = 0, retained_parameters = 0, runtime_parameters = 0;
    std::uint64_t sampled_peak_rss = 0, ready_rss = 0, unloaded_rss = 0;
    double load_seconds = 0, inference_seconds = 0, unload_seconds = 0;
};
struct Generation {
    // Untrusted candidate text, never an admitted Task IR, proof or answer.
    std::string text;
    GenerationStop stop = GenerationStop::TokenLimit;
    std::uint32_t input_tokens = 0, output_tokens = 0;
};

// One owner thread. CPU-only, one local single-file GGUF, no external adapters,
// sidecars, network, model training or reasoning-core calls. The hash-bound
// metadata Jinja template is shared by token_count() and generate(). No fallback.
// RSS/time limits are cooperative checkpoints, not OS-enforced hard caps.
class NativeModelManager {
public:
    explicit NativeModelManager(ModelLimits limits = {});
    ~NativeModelManager();
    NativeModelManager(const NativeModelManager&) = delete;
    NativeModelManager& operator=(const NativeModelManager&) = delete;

    ModelState state() const noexcept { return state_; }
    const ModelLimits& limits() const noexcept { return limits_; }
    const ModelMetrics& metrics() const noexcept { return metrics_; }
    void load(const ModelSpec& spec, std::string_view trusted_system = {});
    std::size_t token_count(std::string_view context_json);
    // Optional host-owned GBNF restricts serialization, not source meaning.
    Generation generate(std::string_view context_json, std::uint32_t maximum_output_tokens,
                        std::string_view grammar = {});
    // Idempotent from Unloaded; also clears Failed for explicit recovery.
    // Destroys sampler, context/KV, weights and the open model descriptor.
    void unload();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    ModelLimits limits_;
    ModelState state_ = ModelState::Unloaded;
    ModelMetrics metrics_;
};

} // namespace axiom::neural
