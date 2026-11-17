// task:native-model-faults-v1

#include "ggml.h"
#include "gguf.h"
#include "knowledge.hpp"
#include "model_manager.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unistd.h>
#include <vector>

namespace {

using axiom::knowledge::Digest;
using axiom::knowledge::Error;
using axiom::knowledge::ErrorCode;
using axiom::neural::ModelLimits;
using axiom::neural::ModelRole;
using axiom::neural::ModelSpec;
using axiom::neural::ModelState;
using axiom::neural::NativeModelManager;

class TempDir {
public:
    TempDir() {
        const std::filesystem::path base = std::filesystem::temp_directory_path();
        const std::string tmpl = (base / "axiom-model-faults-XXXXXX").string();
        std::vector<char> buf(tmpl.begin(), tmpl.end());
        buf.push_back('\0');
        char* created = ::mkdtemp(buf.data());
        if (!created) {
            throw std::runtime_error("mkdtemp failed");
        }
        path_ = created;
    }

    ~TempDir() noexcept {
        if (!path_.empty()) {
            std::error_code ec;
            std::filesystem::remove_all(path_, ec);
        }
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

struct GgmlContextDeleter {
    void operator()(ggml_context* ctx) const noexcept {
        if (ctx) {
            ggml_free(ctx);
        }
    }
};

struct GgufContextDeleter {
    void operator()(gguf_context* ctx) const noexcept {
        if (ctx) {
            gguf_free(ctx);
        }
    }
};

using GgmlContextPtr = std::unique_ptr<ggml_context, GgmlContextDeleter>;
using GgufContextPtr = std::unique_ptr<gguf_context, GgufContextDeleter>;

class Checker {
public:
    void check(bool condition, const char* message) {
        ++checks_;
        if (!condition) {
            throw std::runtime_error(message);
        }
    }

    template <typename Fn>
    void rejects(ErrorCode expected, Fn&& fn, const char* message) {
        ++checks_;
        bool caught_expected = false;
        try {
            fn();
        } catch (const Error& err) {
            if (err.code != expected) {
                throw std::runtime_error(std::string(message) + ": wrong ErrorCode (" + err.what() + ")");
            }
            caught_expected = true;
        } catch (const std::exception& err) {
            throw std::runtime_error(std::string(message) + ": unexpected exception (" + err.what() + ")");
        }
        if (!caught_expected) {
            throw std::runtime_error(std::string(message) + ": expected ErrorCode was not thrown");
        }
    }

    std::size_t count() const noexcept { return checks_; }

private:
    std::size_t checks_ = 0;
};

struct SyntheticGguf {
    std::filesystem::path path;
    Digest sha256{};
};

SyntheticGguf write_synthetic_gguf(const std::filesystem::path& dir, Checker& checker) {
    ggml_init_params params{};
    params.mem_size = 1024 * 1024;
    params.mem_buffer = nullptr;
    params.no_alloc = false;

    GgmlContextPtr ggml_ctx(ggml_init(params));
    checker.check(ggml_ctx != nullptr, "ggml_init must return non-null context");

    ggml_tensor* text_weight = ggml_new_tensor_1d(ggml_ctx.get(), GGML_TYPE_F32, 2);
    checker.check(text_weight != nullptr, "text.weight tensor allocation must succeed");
    ggml_set_name(text_weight, "text.weight");
    std::memset(ggml_get_data(text_weight), 0, ggml_nbytes(text_weight));

    ggml_tensor* sidecar_weight = ggml_new_tensor_1d(ggml_ctx.get(), GGML_TYPE_F32, 3);
    checker.check(sidecar_weight != nullptr, "sidecar.weight tensor allocation must succeed");
    ggml_set_name(sidecar_weight, "sidecar.weight");
    std::memset(ggml_get_data(sidecar_weight), 0, ggml_nbytes(sidecar_weight));

    GgufContextPtr gguf_ctx(gguf_init_empty());
    checker.check(gguf_ctx != nullptr, "gguf_init_empty must return non-null context");

    gguf_add_tensor(gguf_ctx.get(), text_weight);
    gguf_add_tensor(gguf_ctx.get(), sidecar_weight);

    const std::filesystem::path out_path = dir / "synthetic.gguf";
    const std::string out_path_str = out_path.string();
    checker.check(gguf_write_to_file(gguf_ctx.get(), out_path_str.c_str(), false),
                  "gguf_write_to_file must succeed");

    std::ifstream file(out_path, std::ios::binary | std::ios::ate);
    checker.check(file.is_open(), "synthetic GGUF must open for digest calculation");
    const std::streamsize file_size = file.tellg();
    checker.check(file_size > 0, "synthetic GGUF must have positive byte length");
    file.seekg(0, std::ios::beg);

    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(file_size));
    checker.check(static_cast<bool>(file.read(reinterpret_cast<char*>(bytes.data()), file_size)),
                  "synthetic GGUF bytes must be readable");

    const Digest sha256 = axiom::knowledge::digest(bytes);
    checker.check(sha256 != Digest{}, "synthetic GGUF digest must be nonzero");

    return SyntheticGguf{out_path, sha256};
}

} // namespace

int main() {
    try {
        Checker checker;
        TempDir temp_dir;
        const SyntheticGguf fixture = write_synthetic_gguf(temp_dir.path(), checker);

        // 1. Initial Unloaded state, token_count/generate before load, idempotent unload.
        {
            NativeModelManager mgr;
            checker.check(mgr.state() == ModelState::Unloaded, "new manager must start Unloaded");
            checker.rejects(
                ErrorCode::Schema,
                [&] { (void)mgr.token_count("{\"prompt\":\"hi\"}"); },
                "token_count before load must reject Schema");
            checker.check(mgr.state() == ModelState::Unloaded,
                          "state must remain Unloaded after token_count rejection");
            checker.rejects(
                ErrorCode::Schema,
                [&] { (void)mgr.generate("{\"prompt\":\"hi\"}", 4); },
                "generate before load must reject Schema");
            checker.check(mgr.state() == ModelState::Unloaded,
                          "state must remain Unloaded after generate rejection");
            mgr.unload();
            checker.check(mgr.state() == ModelState::Unloaded, "first unload from Unloaded stays Unloaded");
            mgr.unload();
            checker.check(mgr.state() == ModelState::Unloaded, "second unload from Unloaded stays Unloaded");
        }

        // 2. Constructor limit validation (one default limit changed per case).
        {
            ModelLimits limits;
            limits.retained_parameters = 3000000001ULL;
            NativeModelManager mgr(limits);
            checker.check(mgr.state() == ModelState::Unloaded,
                          "parameter counts above 3B are not the deployment byte ceiling");
        }
        {
            ModelLimits limits;
            limits.retained_parameters = 0;
            checker.rejects(
                ErrorCode::Schema,
                [&] { NativeModelManager mgr(limits); },
                "retained_parameters == 0 must reject Schema");
        }
        {
            const ModelLimits limits;
            checker.check(limits.file_bytes == 2999999999ULL &&
                          limits.resident_bytes == 2999999999ULL &&
                          limits.retained_parameters == UINT64_MAX,
                          "default limits enforce strict byte ceilings without a parameter ceiling");
        }
        {
            ModelLimits limits;
            limits.file_bytes = 3000000000ULL;
            checker.rejects(
                ErrorCode::Schema,
                [&] { NativeModelManager mgr(limits); },
                "file_bytes at 3 GB must reject Schema");
        }
        {
            ModelLimits limits;
            limits.resident_bytes = 3000000000ULL;
            checker.rejects(
                ErrorCode::Schema,
                [&] { NativeModelManager mgr(limits); },
                "resident_bytes at 3 GB must reject Schema");
        }
        {
            ModelLimits limits;
            limits.context_tokens = 0;
            checker.rejects(
                ErrorCode::Schema,
                [&] { NativeModelManager mgr(limits); },
                "context_tokens == 0 must reject Schema");
        }
        {
            ModelLimits limits;
            limits.threads = 0;
            checker.rejects(
                ErrorCode::Schema,
                [&] { NativeModelManager mgr(limits); },
                "threads == 0 must reject Schema");
        }
        {
            ModelLimits limits;
            limits.phase_time = std::chrono::milliseconds{0};
            checker.rejects(
                ErrorCode::Schema,
                [&] { NativeModelManager mgr(limits); },
                "phase_time == 0 ms must reject Schema");
        }

        // 3. All-zero expected_sha256 rejects Schema before loading, leaving state Unloaded.
        {
            NativeModelManager mgr;
            ModelSpec spec;
            spec.id = "fixture";
            spec.path = fixture.path;
            spec.expected_sha256 = {};
            checker.rejects(
                ErrorCode::Schema,
                [&] { mgr.load(spec); },
                "all-zero expected_sha256 must reject Schema");
            checker.check(mgr.state() == ModelState::Unloaded,
                          "all-zero expected_sha256 must leave state Unloaded");
        }

        // 4. Invalid role cast from 99 rejects Schema, leaving state Unloaded.
        {
            NativeModelManager mgr;
            ModelSpec spec;
            spec.id = "fixture";
            spec.path = fixture.path;
            spec.expected_sha256 = fixture.sha256;
            spec.role = static_cast<ModelRole>(99);
            checker.rejects(
                ErrorCode::Schema,
                [&] { mgr.load(spec); },
                "invalid ModelRole(99) must reject Schema");
            checker.check(mgr.state() == ModelState::Unloaded,
                          "invalid ModelRole must leave state Unloaded");
        }

        // 5. Missing path with nonzero digest rejects Storage -> Failed -> Schema -> unload -> Unloaded.
        {
            NativeModelManager mgr;
            ModelSpec missing_spec;
            missing_spec.id = "fixture";
            missing_spec.path = temp_dir.path() / "missing.gguf";
            missing_spec.expected_sha256 = fixture.sha256;
            checker.rejects(
                ErrorCode::Storage,
                [&] { mgr.load(missing_spec); },
                "missing path must reject Storage");
            checker.check(mgr.state() == ModelState::Failed, "missing path must enter Failed");

            ModelSpec valid_spec;
            valid_spec.id = "fixture";
            valid_spec.path = fixture.path;
            valid_spec.expected_sha256 = fixture.sha256;
            checker.rejects(
                ErrorCode::Schema,
                [&] { mgr.load(valid_spec); },
                "load while Failed must reject Schema");
            checker.check(mgr.state() == ModelState::Failed, "state must remain Failed until unload");

            mgr.unload();
            checker.check(mgr.state() == ModelState::Unloaded, "explicit unload must recover Unloaded");
        }

        // 6. Digest flipped at one byte rejects Integrity and enters Failed; unload recovers Unloaded.
        {
            NativeModelManager mgr;
            ModelSpec spec;
            spec.id = "fixture";
            spec.path = fixture.path;
            spec.expected_sha256 = fixture.sha256;
            spec.expected_sha256[0] ^= 0x01U;
            checker.rejects(
                ErrorCode::Integrity,
                [&] { mgr.load(spec); },
                "flipped digest must reject Integrity");
            checker.check(mgr.state() == ModelState::Failed, "Integrity failure must enter Failed");
            mgr.unload();
            checker.check(mgr.state() == ModelState::Unloaded,
                          "explicit unload after Integrity failure must recover Unloaded");
        }

        // 7. file_bytes=1 rejects Budget and enters Failed; unload recovers Unloaded.
        {
            ModelLimits limits;
            limits.file_bytes = 1;
            NativeModelManager mgr(limits);
            ModelSpec spec;
            spec.id = "fixture";
            spec.path = fixture.path;
            spec.expected_sha256 = fixture.sha256;
            checker.rejects(
                ErrorCode::Budget,
                [&] { mgr.load(spec); },
                "file_bytes=1 must reject Budget");
            checker.check(mgr.state() == ModelState::Failed, "file_bytes Budget failure must enter Failed");
            mgr.unload();
            checker.check(mgr.state() == ModelState::Unloaded,
                          "explicit unload after file_bytes failure must recover Unloaded");
        }

        // 8. retained_parameters=4 rejects Budget before native loading and reports retained_parameters==5.
        {
            ModelLimits limits;
            limits.retained_parameters = 4;
            NativeModelManager mgr(limits);
            ModelSpec spec;
            spec.id = "fixture";
            spec.path = fixture.path;
            spec.expected_sha256 = fixture.sha256;
            checker.rejects(
                ErrorCode::Budget,
                [&] { mgr.load(spec); },
                "retained_parameters=4 must reject Budget for 5-element synthetic GGUF");
            checker.check(mgr.metrics().retained_parameters == 5,
                          "metrics.retained_parameters must count all 5 tensor elements");
            checker.check(mgr.state() == ModelState::Failed,
                          "retained_parameters Budget failure must enter Failed");
            mgr.unload();
            checker.check(mgr.state() == ModelState::Unloaded,
                          "explicit unload after retained_parameters failure must recover Unloaded");
        }

        // 9. resident_bytes=1 rejects Budget before model I/O and enters Failed.
        {
            ModelLimits limits;
            limits.resident_bytes = 1;
            NativeModelManager mgr(limits);
            ModelSpec spec;
            spec.id = "fixture";
            spec.path = fixture.path;
            spec.expected_sha256 = fixture.sha256;
            checker.rejects(
                ErrorCode::Budget,
                [&] { mgr.load(spec); },
                "resident_bytes=1 must reject Budget");
            checker.check(mgr.state() == ModelState::Failed,
                          "resident_bytes Budget failure must enter Failed");
            mgr.unload();
            checker.check(mgr.state() == ModelState::Unloaded,
                          "explicit unload after resident_bytes failure must recover Unloaded");
        }

        // 10. POSIX symlink to synthetic file is rejected with Storage (O_NOFOLLOW).
        {
            const std::filesystem::path symlink_path = temp_dir.path() / "synthetic-link.gguf";
            std::filesystem::create_symlink(fixture.path, symlink_path);

            NativeModelManager mgr;
            ModelSpec spec;
            spec.id = "fixture";
            spec.path = symlink_path;
            spec.expected_sha256 = fixture.sha256;
            checker.rejects(
                ErrorCode::Storage,
                [&] { mgr.load(spec); },
                "symlink path must reject Storage via O_NOFOLLOW");
        }

        // 11. process_memory().resident_bytes is positive.
        checker.check(axiom::neural::process_memory().resident_bytes > 0,
                      "process_memory().resident_bytes must be positive");

        std::cout << "Native model fault checks passed: " << checker.count() << '\n';
        return 0;
    } catch (const std::exception& err) {
        std::cerr << err.what() << '\n';
        return 1;
    }
}
