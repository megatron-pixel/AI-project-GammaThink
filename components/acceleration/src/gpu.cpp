#include "gpu.hpp"

#include <chrono>
#include <cstring>
#include <dlfcn.h>
#include <stdexcept>

namespace axiom {
namespace {

// CUDA Driver API ABI types; no toolkit or training runtime is required.
using Device = int;
using Result = int;
using Context = void*;
using Module = void*;
using Function = void*;
using Stream = void*;
using DevicePointer = unsigned long long;

constexpr char rule_kernel[] = R"ptx(
.version 6.4
.target sm_75
.address_size 64
.visible .entry match_rules(
    .param .u64 masks,
    .param .u64 facts,
    .param .u64 output,
    .param .u32 count
) {
    .reg .pred %p<3>;
    .reg .b32 %r<7>;
    .reg .b64 %d<9>;
    ld.param.u64 %d0, [masks];
    ld.param.u64 %d1, [facts];
    ld.param.u64 %d2, [output];
    ld.param.u32 %r0, [count];
    mov.u32 %r1, %ctaid.x;
    mov.u32 %r2, %ntid.x;
    mov.u32 %r3, %tid.x;
    mad.lo.u32 %r4, %r1, %r2, %r3;
    setp.ge.u32 %p0, %r4, %r0;
    @%p0 bra done;
    mul.wide.u32 %d3, %r4, 8;
    add.u64 %d4, %d0, %d3;
    ld.global.u64 %d5, [%d4];
    and.b64 %d6, %d5, %d1;
    setp.eq.u64 %p1, %d6, %d5;
    selp.u32 %r5, 1, 0, %p1;
    cvt.u64.u32 %d7, %r4;
    add.u64 %d8, %d2, %d7;
    st.global.u8 [%d8], %r5;
done:
    ret;
}
)ptx";

class Driver {
    void* handle_ = nullptr;
public:
    Result (*init)(unsigned int) = nullptr;
    Result (*device_get)(Device*, int) = nullptr;
    Result (*device_name)(char*, int, Device) = nullptr;
    Result (*context_create)(Context*, unsigned int, Device) = nullptr;
    Result (*context_destroy)(Context) = nullptr;
    Result (*module_load)(Module*, const void*) = nullptr;
    Result (*module_unload)(Module) = nullptr;
    Result (*function_get)(Function*, Module, const char*) = nullptr;
    Result (*memory_allocate)(DevicePointer*, std::size_t) = nullptr;
    Result (*memory_free)(DevicePointer) = nullptr;
    Result (*memory_info)(std::size_t*, std::size_t*) = nullptr;
    Result (*copy_to_device)(DevicePointer, const void*, std::size_t) = nullptr;
    Result (*copy_to_host)(void*, DevicePointer, std::size_t) = nullptr;
    Result (*launch)(Function, unsigned int, unsigned int, unsigned int,
                     unsigned int, unsigned int, unsigned int,
                     unsigned int, Stream, void**, void**) = nullptr;
    Result (*synchronize)() = nullptr;
    Result (*error_name)(Result, const char**) = nullptr;

    template<class T> void bind(T& function, const char* name) {
        void* symbol = dlsym(handle_, name);
        if (symbol == nullptr) {
            throw std::runtime_error(std::string("Missing CUDA driver symbol: ") + name);
        }
        static_assert(sizeof(function) == sizeof(symbol));
        std::memcpy(&function, &symbol, sizeof(symbol));
    }

    Driver() {
        handle_ = dlopen("libcuda.so.1", RTLD_NOW | RTLD_LOCAL);
        if (handle_ == nullptr) {
            throw std::runtime_error("CUDA driver library unavailable");
        }
        try {
            bind(init, "cuInit");
            bind(error_name, "cuGetErrorName");
            check(init(0), "cuInit");
            bind(device_get, "cuDeviceGet");
            bind(device_name, "cuDeviceGetName");
            bind(context_create, "cuCtxCreate_v2");
            bind(context_destroy, "cuCtxDestroy_v2");
            bind(module_load, "cuModuleLoadData");
            bind(module_unload, "cuModuleUnload");
            bind(function_get, "cuModuleGetFunction");
            bind(memory_allocate, "cuMemAlloc_v2");
            bind(memory_free, "cuMemFree_v2");
            bind(memory_info, "cuMemGetInfo_v2");
            bind(copy_to_device, "cuMemcpyHtoD_v2");
            bind(copy_to_host, "cuMemcpyDtoH_v2");
            bind(launch, "cuLaunchKernel");
            bind(synchronize, "cuCtxSynchronize");
        } catch (...) {
            dlclose(handle_);
            throw;
        }
    }

    Driver(const Driver&) = delete;
    Driver& operator=(const Driver&) = delete;
    ~Driver() { dlclose(handle_); }

    void check(Result code, const char* operation) const {
        if (code == 0) return;
        const char* name = nullptr;
        error_name(code, &name);
        throw std::runtime_error(std::string(operation) + ": "
            + (name == nullptr ? std::to_string(code) : name)
            + ". No CPU fallback was counted as GPU execution.");
    }
};

class GpuRules {
    Driver driver_;
    Context context_ = nullptr;
    Module module_ = nullptr;
    Function function_ = nullptr;
public:
    std::string name;
    std::size_t total_bytes = 0;
    std::size_t free_bytes = 0;

    GpuRules() {
        Device device = 0;
        driver_.check(driver_.device_get(&device, 0), "cuDeviceGet");
        char buffer[256]{};
        driver_.check(driver_.device_name(buffer, sizeof(buffer), device), "cuDeviceGetName");
        name = buffer;
        driver_.check(driver_.context_create(&context_, 0, device), "cuCtxCreate");
        try {
            driver_.check(driver_.module_load(&module_, rule_kernel), "cuModuleLoadData");
            driver_.check(driver_.function_get(&function_, module_, "match_rules"),
                          "cuModuleGetFunction");
            driver_.check(driver_.memory_info(&free_bytes, &total_bytes), "cuMemGetInfo");
        } catch (...) {
            if (module_ != nullptr) driver_.module_unload(module_);
            driver_.context_destroy(context_);
            throw;
        }
    }

    GpuRules(const GpuRules&) = delete;
    GpuRules& operator=(const GpuRules&) = delete;
    ~GpuRules() {
        driver_.module_unload(module_);
        driver_.context_destroy(context_);
    }

    std::vector<std::uint8_t> run(const std::vector<std::uint64_t>& masks,
                                  std::uint64_t facts) {
        if (masks.size() > 1048576) {
            throw std::invalid_argument("GPU rule batch exceeds 1048576 rules");
        }
        std::vector<std::uint8_t> output(masks.size());
        if (masks.empty()) return output;
        DevicePointer inputs = 0;
        DevicePointer outputs = 0;
        try {
            driver_.check(driver_.memory_allocate(&inputs, masks.size() * sizeof(masks[0])),
                          "cuMemAlloc(inputs)");
            driver_.check(driver_.memory_allocate(&outputs, output.size()),
                          "cuMemAlloc(outputs)");
            driver_.check(driver_.copy_to_device(inputs, masks.data(),
                          masks.size() * sizeof(masks[0])), "cuMemcpyHtoD");
            auto count = static_cast<unsigned int>(masks.size());
            void* arguments[]{&inputs, &facts, &outputs, &count};
            driver_.check(driver_.launch(function_, (count + 255) / 256, 1, 1,
                                        256, 1, 1, 0, nullptr, arguments, nullptr),
                          "cuLaunchKernel");
            driver_.check(driver_.synchronize(), "cuCtxSynchronize");
            driver_.check(driver_.copy_to_host(output.data(), outputs, output.size()),
                          "cuMemcpyDtoH");
        } catch (...) {
            if (outputs != 0) driver_.memory_free(outputs);
            if (inputs != 0) driver_.memory_free(inputs);
            throw;
        }
        driver_.check(driver_.memory_free(outputs), "cuMemFree(outputs)");
        driver_.check(driver_.memory_free(inputs), "cuMemFree(inputs)");
        return output;
    }
};

}

std::vector<std::uint8_t> gpu_rule_matches(
    const std::vector<std::uint64_t>& masks, std::uint64_t known_facts) {
    GpuRules gpu;
    return gpu.run(masks, known_facts);
}

GpuReport test_gpu_rules() {
    GpuRules gpu;
    // Exhaust all 8-fact knowledge sets against all 8-fact rule masks.
    std::vector<std::uint64_t> masks(256);
    for (std::size_t index = 0; index < masks.size(); ++index) masks[index] = index;
    std::size_t checked = 0;
    const auto begin = std::chrono::steady_clock::now();
    for (std::uint64_t facts = 0; facts < 256; ++facts) {
        const auto results = gpu.run(masks, facts);
        for (std::size_t index = 0; index < masks.size(); ++index) {
            if (results[index] != static_cast<std::uint8_t>((masks[index] & facts) == masks[index])) {
                throw std::runtime_error("GPU rule eligibility differs from CPU exhaustive oracle");
            }
            ++checked;
        }
    }
    // Exercise bit 63 and a non-multiple-of-block-size launch.
    masks = {0, 1ULL << 63, ~0ULL};
    const auto high = gpu.run(masks, 1ULL << 63);
    if (high != std::vector<std::uint8_t>{1, 1, 0}) {
        throw std::runtime_error("GPU high-bit rule test failed");
    }
    checked += masks.size();
    const auto end = std::chrono::steady_clock::now();
    return {gpu.name, checked,
            std::chrono::duration<double, std::milli>(end - begin).count(),
            gpu.total_bytes, gpu.free_bytes, 256 * (sizeof(std::uint64_t) + 1)};
}

}
