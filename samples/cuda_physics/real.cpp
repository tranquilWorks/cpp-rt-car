#include "cli.hpp"
#include "scenario.hpp"
#include <rt/cuda_driver.hpp>
#include <cuda.h>

namespace p = rtfw::cuda_physics;
namespace {
class Resources {
    CUdevice device_{};
    CUcontext context_{};
    CUstream stream_{};
    CUmodule module_{};
    bool current_{};
public:
    p::Session session;
    int open() noexcept {
        const auto initialized = cuInit(0);
        if (initialized == CUDA_ERROR_NO_DEVICE || initialized == CUDA_ERROR_STUB_LIBRARY) return 3;
        if (initialized != CUDA_SUCCESS) return 1;
        int count{};
        if (cuDeviceGetCount(&count) != CUDA_SUCCESS) return 1;
        if (count == 0) return 3;
        if (cuDeviceGet(&device_, 0) != CUDA_SUCCESS ||
            cuDevicePrimaryCtxRetain(&context_, device_) != CUDA_SUCCESS) return 1;
        if (cuCtxPushCurrent(context_) != CUDA_SUCCESS) return 1;
        current_ = true;
        CUfunction function{};
        if (cuStreamCreate(&stream_, CU_STREAM_NON_BLOCKING) != CUDA_SUCCESS ||
            cuModuleLoad(&module_, RTFW_PHYSICS_PTX_PATH) != CUDA_SUCCESS ||
            cuModuleGetFunction(&function, module_, "rtfw_particle_step") != CUDA_SUCCESS) return 1;
        CUcontext popped{};
        if (cuCtxPopCurrent(&popped) != CUDA_SUCCESS || popped != context_) return 1;
        current_ = false;
        session = {rt::cuda_driver_api(), reinterpret_cast<rt::CudaContext>(context_),
                   reinterpret_cast<rt::CudaStream>(stream_), reinterpret_cast<rt::CudaFunction>(function)};
        return 0;
    }
    bool close() noexcept {
        if (!context_) return true;
        if (!current_) {
            if (cuCtxPushCurrent(context_) != CUDA_SUCCESS) return false;
            current_ = true;
        }
        if (module_) {
            if (cuModuleUnload(module_) != CUDA_SUCCESS) return false;
            module_ = nullptr;
        }
        if (stream_) {
            if (cuStreamDestroy(stream_) != CUDA_SUCCESS) return false;
            stream_ = nullptr;
        }
        CUcontext popped{};
        if (cuCtxPopCurrent(&popped) != CUDA_SUCCESS || popped != context_) return false;
        current_ = false;
        if (cuDevicePrimaryCtxRelease(device_) != CUDA_SUCCESS) return false;
        context_ = nullptr;
        return true;
    }
    ~Resources() { if (!close()) std::terminate(); }
};
}
int main(int argc, char** argv) {
    p::Options options;
    const auto parsed = p::parse(argc, argv, options, true);
    if (parsed) return parsed == 3 ? 0 : parsed;
    try {
        Resources resources;
        const auto opened = resources.open();
        if (opened) {
            if (!resources.close()) return 1;
            if (opened == 3) std::cout << "status=NOT_RUN reason=CUDA_device_or_driver_unavailable\n";
            return opened;
        }
        auto scenario = std::make_unique<p::Scenario>(options, resources.session);
        auto status = scenario->prepare();
        if (status == rt::Status::ok) status = scenario->start();
        if (status == rt::Status::ok) status = scenario->run();
        const auto stopped = scenario->stop();
        if (stopped != rt::Status::ok && scenario->stop() != rt::Status::ok) std::terminate();
        const auto completed = scenario->completed();
        scenario.reset();
        const bool closed = resources.close();
        if (status != rt::Status::ok || stopped != rt::Status::ok || !closed) return 1;
        p::summary(options, completed, "real-CUDA-unqualified");
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
