#include "model_manager.hpp"
#include "model_prompt.hpp"
#include <llama.h>
#include <gguf.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <fcntl.h>
#include <limits>
#include <openssl/evp.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach/mach.h>
#endif

namespace axiom::neural {
namespace {
using knowledge::Error;
using knowledge::ErrorCode;
using Clock = std::chrono::steady_clock;
void require(bool value, ErrorCode code, const char* message) {
    if (!value) throw Error(code, message);
}
double seconds(Clock::time_point start) {
    return std::chrono::duration<double>(Clock::now() - start).count();
}
void initialize_backend() {
    struct Backend {
        Backend() { llama_backend_init(); }
        ~Backend() { llama_backend_free(); }
    };
    static Backend backend;
}
bool read_memory(ProcessMemory& result) noexcept {
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0 || usage.ru_maxrss < 0) return false;
#if defined(__APPLE__)
    mach_task_basic_info_data_t info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                  reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS) return false;
    result.resident_bytes = info.resident_size;
    result.lifetime_peak_bytes = static_cast<std::uint64_t>(usage.ru_maxrss);
    return true;
#elif defined(__linux__)
    auto* file = std::fopen("/proc/self/statm", "r");
    if (!file) return false;
    unsigned long long total = 0, resident = 0;
    const auto fields = std::fscanf(file, "%llu %llu", &total, &resident);
    std::fclose(file);
    const auto page = sysconf(_SC_PAGESIZE);
    if (fields != 2 || page <= 0 || resident > UINT64_MAX / static_cast<std::uint64_t>(page)) return false;
    result.resident_bytes = resident * static_cast<std::uint64_t>(page);
    result.lifetime_peak_bytes = static_cast<std::uint64_t>(usage.ru_maxrss) * 1024;
    return true;
#else
    return false;
#endif
}
struct Guard {
    std::uint64_t memory_limit;
    Clock::time_point deadline;
    std::atomic<std::uint64_t> peak{0};
    // First fault wins; no exceptions may cross a C callback.
    std::atomic<unsigned> fault{0}; // 1=RSS, 2=time, 3=measurement unavailable
    Guard(std::uint64_t limit, Clock::time_point until) : memory_limit(limit), deadline(until) {}
    bool poll() noexcept {
        ProcessMemory usage;
        unsigned failure = 0;
        if (!read_memory(usage)) failure = 3;
        else {
            auto previous = peak.load();
            while (previous < usage.resident_bytes && !peak.compare_exchange_weak(previous, usage.resident_bytes)) {}
            if (usage.resident_bytes > memory_limit) failure = 1;
            else if (Clock::now() >= deadline) failure = 2;
        }
        unsigned none = 0;
        if (failure) fault.compare_exchange_strong(none, failure);
        return fault.load() == 0;
    }
    void check() {
        poll();
        switch (fault.load()) {
        case 0: return;
        case 1: throw Error(ErrorCode::Budget, "Neural phase RSS budget exceeded");
        case 2: throw Error(ErrorCode::Budget, "Neural phase time budget exceeded");
        default: throw Error(ErrorCode::Unavailable, "Cannot measure neural phase RSS");
        }
    }
};
void rewind_file(FILE* file) {
    require(fseeko(file, 0, SEEK_SET) == 0, ErrorCode::Storage, "Cannot rewind model descriptor");
    clearerr(file);
}
knowledge::Digest hash_file(FILE* file, Guard& guard, std::uint64_t expected_bytes) {
    rewind_file(file);
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    require(context && EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) == 1,
            ErrorCode::Integrity, "Cannot initialize model digest");
    std::array<unsigned char, 65536> buffer{};
    std::uint64_t total = 0;
    for (;;) {
        guard.check();
        const auto bytes = std::fread(buffer.data(), 1, buffer.size(), file);
        require(bytes <= expected_bytes - total, ErrorCode::Integrity, "Model extent changed while hashing");
        total += bytes;
        if (bytes) require(EVP_DigestUpdate(context.get(), buffer.data(), bytes) == 1,
                           ErrorCode::Integrity, "Cannot hash model bytes");
        if (bytes < buffer.size()) {
            require(!std::ferror(file), ErrorCode::Storage, "Cannot read model bytes");
            break;
        }
    }
    require(total == expected_bytes, ErrorCode::Integrity, "Model extent changed while hashing");
    knowledge::Digest digest;
    unsigned length = 0;
    require(EVP_DigestFinal_ex(context.get(), digest.data(), &length) == 1 && length == digest.size(),
            ErrorCode::Integrity, "Cannot finalize model digest");
    rewind_file(file);
    return digest;
}
std::vector<llama_token> tokenize_part(const llama_vocab* vocab, const std::string& input, bool special) {
    require(input.size() <= INT32_MAX, ErrorCode::Budget, "Neural tokenizer input overflow");
    const auto length = static_cast<std::int32_t>(input.size());
    const auto size = llama_tokenize(vocab, input.data(), length, nullptr, 0, false, special);
    require(size < 0 && size != std::numeric_limits<std::int32_t>::min(),
            ErrorCode::Integrity, "Cannot size neural tokens");
    std::vector<llama_token> result(static_cast<std::size_t>(-size));
    require(llama_tokenize(vocab, input.data(), length, result.data(), -size, false, special) == -size,
            ErrorCode::Integrity, "Neural token count mismatch");
    return result;
}
std::vector<llama_token> tokenize(const llama_vocab* vocab, const ModelPrompt& prompt,
    std::string_view input, std::size_t limit) {
    knowledge::validate_text(input, limit);
    const auto parts = prompt.render(input, limit + 4096);
    std::vector<llama_token> result;
    if (llama_vocab_get_add_bos(vocab)) {
        require(llama_vocab_bos(vocab) >= 0, ErrorCode::Integrity, "Missing configured BOS token");
        result.push_back(llama_vocab_bos(vocab));
    }
    for (const auto& part : parts) {
        const auto tokens = tokenize_part(vocab, part.text, !part.input);
        result.insert(result.end(), tokens.begin(), tokens.end());
    }
    if (llama_vocab_get_add_eos(vocab)) {
        require(llama_vocab_eos(vocab) >= 0, ErrorCode::Integrity, "Missing configured EOS token");
        result.push_back(llama_vocab_eos(vocab));
    }
    return result;
}
} // namespace

ProcessMemory process_memory() {
    ProcessMemory result;
    require(read_memory(result), ErrorCode::Unavailable, "Cannot measure process RSS");
    return result;
}

struct NativeModelManager::Impl {
    Guard guard;
    std::unique_ptr<FILE, decltype(&std::fclose)> file{nullptr, std::fclose};
    std::unique_ptr<llama_model, decltype(&llama_model_free)> model{nullptr, llama_model_free};
    std::unique_ptr<llama_context, decltype(&llama_free)> context{nullptr, llama_free};
    std::unique_ptr<llama_sampler, decltype(&llama_sampler_free)> sampler{nullptr, llama_sampler_free};
    std::unique_ptr<llama_batch_ext, decltype(&llama_batch_ext_free)> batch{nullptr, llama_batch_ext_free};
    std::unique_ptr<ModelPrompt> prompt;
    explicit Impl(const ModelLimits& limits)
        : guard(limits.resident_bytes, Clock::now() + limits.phase_time) {}
};

NativeModelManager::NativeModelManager(ModelLimits limits) : limits_(limits) {
    require(limits.retained_parameters > 0 &&
            limits.resident_bytes > 0 && limits.resident_bytes < 3000000000ULL &&
            limits.file_bytes > 0 && limits.file_bytes < 3000000000ULL &&
            limits.context_tokens >= 32 && limits.context_tokens <= 32768 &&
            limits.batch_tokens > 0 && limits.batch_tokens <= limits.context_tokens &&
            limits.threads > 0 && limits.threads <= 10 && limits.input_bytes > 0 &&
            limits.input_bytes <= 4 * 1024 * 1024 && limits.output_bytes > 0 &&
            limits.output_bytes <= 4 * 1024 * 1024 &&
            limits.phase_time.count() > 0 && limits.phase_time <= std::chrono::minutes(30),
            ErrorCode::Schema, "Invalid native model limits");
}
NativeModelManager::~NativeModelManager() = default;

void NativeModelManager::load(const ModelSpec& spec, std::string_view trusted_system) {
    if (!trusted_system.empty()) knowledge::validate_text(trusted_system, 4096);
    require(state_ == ModelState::Unloaded, ErrorCode::Schema, "Model must be unloaded before loading");
    require(!spec.id.empty() && !spec.path.empty() &&
            spec.role >= ModelRole::Interpreter && spec.role <= ModelRole::CodeGeneration,
            ErrorCode::Schema, "Invalid model selection");
    knowledge::validate_text(spec.id, 256);
    require(std::any_of(spec.expected_sha256.begin(), spec.expected_sha256.end(),
                        [](auto byte) { return byte != 0; }),
            ErrorCode::Schema, "Missing expected model identity");
    metrics_ = {};
    state_ = ModelState::Loading;
    const auto started = Clock::now();
    try {
        impl_ = std::make_unique<Impl>(limits_);
        auto& loaded = *impl_;
        loaded.guard.check();
        const auto descriptor = ::open(spec.path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        require(descriptor >= 0, ErrorCode::Storage, "Cannot open local model");
        loaded.file.reset(fdopen(descriptor, "rb"));
        if (!loaded.file) { ::close(descriptor); throw Error(ErrorCode::Storage, "Cannot wrap model descriptor"); }
        struct stat status{};
        require(fstat(descriptor, &status) == 0 && S_ISREG(status.st_mode) && status.st_size > 0,
                ErrorCode::Schema, "Model must be one regular file");
        metrics_.file_bytes = static_cast<std::uint64_t>(status.st_size);
        require(metrics_.file_bytes <= limits_.file_bytes, ErrorCode::Budget, "Model file budget exceeded");
        require(hash_file(loaded.file.get(), loaded.guard, metrics_.file_bytes) == spec.expected_sha256,
                ErrorCode::Integrity, "Model digest does not match selected identity");
        {
            const gguf_init_params parameters{true, nullptr};
            std::unique_ptr<gguf_context, decltype(&gguf_free)> metadata(
                gguf_init_from_file_ptr(loaded.file.get(), parameters), gguf_free);
            require(metadata != nullptr, ErrorCode::Integrity, "Invalid model GGUF metadata");
            // Descriptor-backed loading never discovers unverified split paths.
            require(gguf_find_key(metadata.get(), "split.count") < 0,
                    ErrorCode::Schema, "Split GGUF models are not supported by this manager");
            const auto tensors = gguf_get_n_tensors(metadata.get());
            require(tensors > 0 && tensors <= 100000, ErrorCode::Schema, "Invalid model tensor count");
            for (std::int64_t i = 0; i < tensors; ++i) {
                loaded.guard.check();
                std::uint64_t elements = 1;
                const auto* shape = gguf_get_tensor_ne(metadata.get(), i);
                for (int d = 0; d < GGML_MAX_DIMS; ++d) {
                    require(shape[d] > 0 && static_cast<std::uint64_t>(shape[d]) <= UINT64_MAX / elements,
                            ErrorCode::Integrity, "Model parameter shape overflow");
                    elements *= static_cast<std::uint64_t>(shape[d]);
                }
                require(elements <= UINT64_MAX - metrics_.retained_parameters,
                        ErrorCode::Integrity, "Model parameter count overflow");
                metrics_.retained_parameters += elements;
            }
            require(metrics_.retained_parameters <= limits_.retained_parameters,
                    ErrorCode::Budget, "All retained model parameters exceed the configured cap");
        }
        rewind_file(loaded.file.get());
        initialize_backend();
        auto parameters = llama_model_default_params();
        parameters.n_gpu_layers = 0;
        parameters.load_mode = LLAMA_LOAD_MODE_NONE;
        parameters.lazy_mode = LLAMA_LAZY_MODE_OFF;
        parameters.progress_callback_user_data = &loaded.guard;
        parameters.progress_callback = [](float, void* pointer) {
            return static_cast<Guard*>(pointer)->poll();
        };
        loaded.model.reset(llama_model_load_from_file_ptr(loaded.file.get(), parameters));
        loaded.guard.check();
        require(loaded.model != nullptr, ErrorCode::Unavailable, "Native model load failed");
        metrics_.runtime_parameters = llama_model_n_params(loaded.model.get());
        require(metrics_.runtime_parameters > 0 &&
                metrics_.runtime_parameters <= metrics_.retained_parameters,
                ErrorCode::Integrity, "Runtime model parameter count exceeds admitted file");
        require(!llama_model_has_encoder(loaded.model.get()) && llama_model_has_decoder(loaded.model.get()) &&
                !llama_model_is_diffusion(loaded.model.get()),
                ErrorCode::Schema, "This manager supports autoregressive decoder-only models");
        const auto* template_source = llama_model_chat_template(loaded.model.get(), nullptr);
        require(template_source && *template_source, ErrorCode::Schema, "Selected model lacks a default chat template");
        const auto* vocabulary = llama_model_get_vocab(loaded.model.get());
        const auto piece = [&](llama_token token) {
            if (token < 0) return std::string{};
            const auto needed = llama_token_to_piece(vocabulary, token, nullptr, 0, 0, true);
            require(needed < 0 && needed != INT32_MIN, ErrorCode::Integrity, "Cannot size model framing token");
            std::string text(static_cast<std::size_t>(-needed), '\0');
            require(llama_token_to_piece(vocabulary, token, text.data(), -needed, 0, true) == -needed,
                    ErrorCode::Integrity, "Cannot read model framing token");
            return text;
        };
        loaded.prompt = std::make_unique<ModelPrompt>(template_source, piece(llama_vocab_bos(vocabulary)),
            piece(llama_vocab_eos(vocabulary)), llama_vocab_get_add_bos(vocabulary),
            llama_vocab_get_add_eos(vocabulary), spec.enable_thinking, std::time(nullptr),
            std::string(trusted_system));
        loaded.guard.check();
        require(hash_file(loaded.file.get(), loaded.guard, metrics_.file_bytes) == spec.expected_sha256,
                ErrorCode::Integrity, "Model changed while loading");
        auto context = llama_context_default_params();
        context.n_ctx = limits_.context_tokens;
        context.n_batch = limits_.batch_tokens;
        context.n_ubatch = limits_.batch_tokens;
        context.n_seq_max = 1;
        context.n_threads = static_cast<std::int32_t>(limits_.threads);
        context.n_threads_batch = context.n_threads;
        context.offload_kqv = false;
        context.op_offload = false;
        context.abort_callback_data = &loaded.guard;
        context.abort_callback = [](void* pointer) { return !static_cast<Guard*>(pointer)->poll(); };
        loaded.context.reset(llama_init_from_model(loaded.model.get(), context));
        loaded.guard.check();
        require(loaded.context != nullptr, ErrorCode::Unavailable, "Native context allocation failed");
        require(llama_n_ctx_seq(loaded.context.get()) >= limits_.context_tokens &&
                llama_n_batch(loaded.context.get()) >= limits_.batch_tokens,
                ErrorCode::Integrity, "Runtime context does not meet selected limits");
        loaded.sampler.reset(llama_sampler_init_greedy());
        loaded.batch.reset(llama_batch_ext_init(loaded.context.get()));
        require(loaded.sampler && loaded.batch, ErrorCode::Unavailable, "Native inference allocation failed");
        loaded.guard.check();
        metrics_.sampled_peak_rss = loaded.guard.peak.load();
        metrics_.ready_rss = process_memory().resident_bytes;
        metrics_.load_seconds = seconds(started);
        state_ = ModelState::Ready;
    } catch (...) {
        if (impl_) metrics_.sampled_peak_rss = impl_->guard.peak.load();
        impl_.reset();
        metrics_.load_seconds = seconds(started);
        state_ = ModelState::Failed;
        throw;
    }
}
std::size_t NativeModelManager::token_count(std::string_view input) {
    require(state_ == ModelState::Ready && impl_, ErrorCode::Schema, "Model is not ready for tokenization");
    auto& loaded = *impl_;
    loaded.guard.deadline = Clock::now() + limits_.phase_time;
    loaded.guard.fault.store(0);
    try {
        loaded.guard.check();
        const auto count = tokenize(llama_model_get_vocab(loaded.model.get()), *loaded.prompt, input, limits_.input_bytes).size();
        loaded.guard.check();
        metrics_.sampled_peak_rss = std::max(metrics_.sampled_peak_rss, loaded.guard.peak.load());
        return count;
    } catch (...) {
        metrics_.sampled_peak_rss = std::max(metrics_.sampled_peak_rss, loaded.guard.peak.load());
        impl_.reset();
        state_ = ModelState::Failed;
        throw;
    }
}
Generation NativeModelManager::generate(std::string_view input, std::uint32_t maximum, std::string_view grammar) {
    require(state_ == ModelState::Ready && impl_, ErrorCode::Schema, "Model is not ready for inference");
    require(maximum > 0 && maximum < limits_.context_tokens, ErrorCode::Budget, "Invalid output token budget");
    if (!grammar.empty()) knowledge::validate_text(grammar, 262144);
    auto& loaded = *impl_;
    loaded.guard.deadline = Clock::now() + limits_.phase_time;
    loaded.guard.fault.store(0);
    state_ = ModelState::Inferencing;
    const auto started = Clock::now();
    try {
        loaded.guard.check();
        std::unique_ptr<llama_sampler, decltype(&llama_sampler_free)> constrained(nullptr, llama_sampler_free);
        if (!grammar.empty()) {
            constrained.reset(llama_sampler_chain_init(llama_sampler_chain_default_params()));
            require(constrained != nullptr, ErrorCode::Unavailable, "Cannot allocate constrained sampler");
            const std::string source(grammar);
            auto* mask = llama_sampler_init_grammar(llama_model_get_vocab(loaded.model.get()), source.c_str(), "root");
            require(mask != nullptr, ErrorCode::Schema, "Invalid host generation grammar");
            llama_sampler_chain_add(constrained.get(), mask);
            auto* greedy = llama_sampler_init_greedy();
            require(greedy != nullptr, ErrorCode::Unavailable, "Cannot allocate constrained greedy sampler");
            llama_sampler_chain_add(constrained.get(), greedy);
        }
        auto* sampler = constrained ? constrained.get() : loaded.sampler.get();
        const auto tokens = tokenize(llama_model_get_vocab(loaded.model.get()), *loaded.prompt, input, limits_.input_bytes);
        loaded.guard.check();
        require(tokens.size() <= limits_.context_tokens - maximum, ErrorCode::Budget,
                "Input and reserved output exceed the context budget");
        llama_memory_clear(llama_get_memory(loaded.context.get()), true);
        llama_sampler_reset(sampler);
        const auto* vocab = llama_model_get_vocab(loaded.model.get());
        auto decode = [&](std::span<const llama_token> batch_tokens, std::size_t position) {
            loaded.guard.check();
            llama_batch_ext_clear(loaded.batch.get());
            for (std::size_t i = 0; i < batch_tokens.size(); ++i) {
                const auto index = llama_batch_ext_add_token(loaded.batch.get(), 0, batch_tokens[i]);
                const auto pos = static_cast<llama_pos>(position + i);
                require(index >= 0 && llama_batch_ext_set_pos(loaded.batch.get(), index, &pos),
                        ErrorCode::Integrity, "Cannot construct native input batch");
                require(llama_batch_ext_set_output_logits(loaded.batch.get(), index, i + 1 == batch_tokens.size()),
                        ErrorCode::Integrity, "Cannot select native output logits");
            }
            const auto status = llama_process(loaded.context.get(), LLAMA_PROCESS_TYPE_DECODE, loaded.batch.get());
            loaded.guard.check();
            require(status == 0, ErrorCode::Unavailable, "Native decoder failed");
        };
        for (std::size_t position = 0; position < tokens.size();) {
            const auto size = std::min<std::size_t>(limits_.batch_tokens, tokens.size() - position);
            decode(std::span<const llama_token>(tokens).subspan(position, size), position);
            position += size;
        }
        Generation result;
        result.input_tokens = static_cast<std::uint32_t>(tokens.size());
        for (std::uint32_t i = 0; i < maximum; ++i) {
            loaded.guard.check();
            // The pinned API accepts the token itself; a second accept would
            // advance a stateful grammar twice.
            const auto token = llama_sampler_sample(sampler, loaded.context.get(), -1);
            require(token >= 0 && token < llama_vocab_n_tokens(vocab), ErrorCode::Integrity, "Invalid sampled token");
            ++result.output_tokens;
            if (llama_vocab_is_eog(vocab, token)) { result.stop = GenerationStop::EndOfGeneration; break; }
            const auto needed = llama_token_to_piece(vocab, token, nullptr, 0, 0, true);
            require(needed <= 0 && needed != std::numeric_limits<std::int32_t>::min(),
                    ErrorCode::Integrity, "Cannot size native output token");
            const auto size = static_cast<std::size_t>(-needed);
            require(size <= limits_.output_bytes - result.text.size(),
                    ErrorCode::Budget, "Generated candidate byte budget exceeded");
            std::string piece(size, '\0');
            require(llama_token_to_piece(vocab, token, piece.data(), static_cast<std::int32_t>(size), 0, true) ==
                    static_cast<std::int32_t>(size), ErrorCode::Integrity, "Native output token size mismatch");
            result.text += piece;
            if (i + 1 < maximum) decode(std::span<const llama_token>(&token, 1), tokens.size() + i);
        }
        loaded.guard.check();
        metrics_.sampled_peak_rss = std::max(metrics_.sampled_peak_rss, loaded.guard.peak.load());
        metrics_.inference_seconds = seconds(started);
        state_ = ModelState::Ready;
        return result;
    } catch (...) {
        metrics_.sampled_peak_rss = std::max(metrics_.sampled_peak_rss, loaded.guard.peak.load());
        metrics_.inference_seconds = seconds(started);
        impl_.reset();
        state_ = ModelState::Failed;
        throw;
    }
}
void NativeModelManager::unload() {
    require(state_ == ModelState::Unloaded || state_ == ModelState::Ready || state_ == ModelState::Failed,
            ErrorCode::Schema, "Cannot unload an active neural phase");
    const auto started = Clock::now();
    state_ = ModelState::Unloading;
    impl_.reset();
    state_ = ModelState::Unloaded;
    metrics_.unload_seconds = seconds(started);
    metrics_.unloaded_rss = process_memory().resident_bytes;
}

} // namespace axiom::neural
