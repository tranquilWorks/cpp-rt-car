#include "runner.hpp"
#include <cuda.h>
#include <rt/cuda_driver.hpp>
namespace g = golden::cuda;
namespace {
// All native resources belong to this host and outlive checked Owner teardown.
class NativeResources {
  CUdevice device_{};
  CUcontext context_{};
  CUstream stream_{};
  CUmodule module_{};
  CUgraph graph_{};
  CUgraphExec executable_{};
  std::array<CUdeviceptr, 2> buffers_{};
  bool current_{};

public:
  g::Resources resources;
  int open(const g::Arguments &a) noexcept {
    const auto initialized = cuInit(0);
    if (initialized == CUDA_ERROR_NO_DEVICE ||
        initialized == CUDA_ERROR_STUB_LIBRARY ||
        initialized == CUDA_ERROR_SYSTEM_DRIVER_MISMATCH)
      return 3;
    if (initialized != CUDA_SUCCESS)
      return 1;
    int count{};
    if (cuDeviceGetCount(&count) != CUDA_SUCCESS)
      return 1;
    if (!count)
      return 3;
    if (cuDeviceGet(&device_, 0) != CUDA_SUCCESS ||
        cuDevicePrimaryCtxRetain(&context_, device_) != CUDA_SUCCESS)
      return 1;
    if (cuCtxPushCurrent(context_) != CUDA_SUCCESS)
      return 1;
    current_ = true;
    CUfunction function{};
    if (cuStreamCreate(&stream_, CU_STREAM_NON_BLOCKING) != CUDA_SUCCESS ||
        cuModuleLoad(&module_, RTFW_GOLDEN_CUDA_PTX_PATH) != CUDA_SUCCESS ||
        cuModuleGetFunction(&function, module_, "golden_physics_step") !=
            CUDA_SUCCESS)
      return 1;
    for (auto &buffer : buffers_)
      if (cuMemAlloc(&buffer, sizeof(g::State)) != CUDA_SUCCESS)
        return 1;
    if (a.dispatch == "graph") {
      if (cuGraphCreate(&graph_, 0) != CUDA_SUCCESS)
        return 1;
      auto elements = static_cast<unsigned>(a.scenario.count);
      void *arguments[]{&buffers_[1], &elements};
      CUDA_KERNEL_NODE_PARAMS p{};
      p.func = function;
      p.gridDimX = (elements + 127) / 128;
      p.gridDimY = p.gridDimZ = 1;
      p.blockDimX = 128;
      p.blockDimY = p.blockDimZ = 1;
      p.kernelParams = arguments;
      CUgraphNode node{};
      if (cuGraphAddKernelNode(&node, graph_, nullptr, 0, &p) != CUDA_SUCCESS ||
          cuGraphInstantiateWithFlags(&executable_, graph_, 0) != CUDA_SUCCESS)
        return 1;
    }
    CUcontext popped{};
    if (cuCtxPopCurrent(&popped) != CUDA_SUCCESS || popped != context_)
      return 1;
    current_ = false;
    resources = {rt::cuda_driver_api(),
                 reinterpret_cast<rt::CudaContext>(context_),
                 reinterpret_cast<rt::CudaStream>(stream_),
                 reinterpret_cast<rt::CudaFunction>(function),
                 reinterpret_cast<rt::CudaGraphExec>(executable_),
                 {buffers_[0], buffers_[1]}};
    return 0;
  }
  bool close() noexcept {
    if (!context_)
      return true;
    if (!current_) {
      if (cuCtxPushCurrent(context_) != CUDA_SUCCESS)
        return false;
      current_ = true;
    }
    if (executable_) {
      if (cuGraphExecDestroy(executable_) != CUDA_SUCCESS)
        return false;
      executable_ = nullptr;
    }
    if (graph_) {
      if (cuGraphDestroy(graph_) != CUDA_SUCCESS)
        return false;
      graph_ = nullptr;
    }
    for (auto &buffer : buffers_)
      if (buffer) {
        if (cuMemFree(buffer) != CUDA_SUCCESS)
          return false;
        buffer = 0;
      }
    if (module_) {
      if (cuModuleUnload(module_) != CUDA_SUCCESS)
        return false;
      module_ = nullptr;
    }
    if (stream_) {
      if (cuStreamDestroy(stream_) != CUDA_SUCCESS)
        return false;
      stream_ = nullptr;
    }
    CUcontext popped{};
    if (cuCtxPopCurrent(&popped) != CUDA_SUCCESS || popped != context_)
      return false;
    current_ = false;
    if (cuDevicePrimaryCtxRelease(device_) != CUDA_SUCCESS)
      return false;
    context_ = nullptr;
    return true;
  }
  ~NativeResources() {
    if (!close())
      std::terminate();
  }
};
} // namespace
int main(int argc, char **argv) {
  if (argc == 2 && std::string_view(argv[1]) == "--help") {
    std::cout << "golden_cuda_real [--dispatch kernel|graph] [--mode "
                 "native|host] [--count 1..256] [--ticks 1..1024] [--workers "
                 "1..3] [--grain 1|4|16|64] [--output DIRECTORY]\nNative "
                 "replay unsupported; absent driver/device exits3 NOT_RUN.\n";
    return 0;
  }
  g::Arguments a;
  if (!g::arguments(argc, argv, a) || a.dispatch == "cpu" ||
      a.fault != g::Fault::none || a.scenario.external ||
      a.scenario.campaign != golden::Campaign::nominal)
    return 2;
  try {
    NativeResources native;
    const auto opened = native.open(a);
    if (opened) {
      if (!native.close())
        return 1;
      if (opened == 3)
        std::cout
            << "status=NOT_RUN reason=CUDA_device_or_driver_unavailable\n";
      return opened;
    }
    auto owner = std::make_unique<g::Owner>(
        a.scenario,
        a.dispatch == "graph" ? g::Dispatch::graph : g::Dispatch::kernel,
        &native.resources);
    auto &s = *owner->session;
    auto status = owner->prepare();
    rt::CompiledDeviceRatePhase phase;
    if (status == rt::Status::ok &&
        (!s.runtime->compiled_device_rate_phase_at(0, phase) ||
         phase.simulation || owner->physics->replay_enabled()))
      status = rt::Status::internal_error;
    if (status == rt::Status::ok && !s.controls())
      status = rt::Status::internal_error;
    golden::Oracle oracle(a.scenario);
    g::Telemetry telemetry;
    for (std::size_t t = 0; status == rt::Status::ok && t < a.scenario.ticks;
         ++t) {
      status = s.step(t);
      if (status == rt::Status::ok && !oracle.step(t, s.world))
        status = rt::Status::internal_error;
      if (status == rt::Status::ok)
        status = telemetry.drain(*s.runtime);
    }
    rt::DeviceTimelineInfo timeline;
    if (status == rt::Status::ok &&
        (!owner->physics->timeline(timeline) ||
         timeline.completed_value != a.scenario.ticks ||
         owner->physics->providers != a.scenario.ticks ||
         owner->physics->publications != a.scenario.ticks))
      status = rt::Status::internal_error;
    const auto state = s.world.canonical;
    const auto providers = owner->physics ? owner->physics->providers : 0,
               publications = owner->physics ? owner->physics->publications : 0;
    if (status != rt::Status::ok)
      std::cerr << s.runtime->last_error() << '\n';
    const auto stopped = owner->close();
    if (stopped != rt::Status::ok && owner->close() != rt::Status::ok)
      std::terminate();
    owner.reset();
    const bool closed = native.close();
    if (status != rt::Status::ok || stopped != rt::Status::ok || !closed)
      return 1;
    if (!a.output.empty()) {
      // A fresh directory prevents stale simulator transcripts from appearing
      // beside an actual native run. Physical evidence is never synthesized.
      if (fs::exists(a.output) && !fs::is_empty(a.output))
        return 2;
      fs::create_directories(a.output);
      if (!g::write(a.output / "state.bin", state))
        return 1;
      std::ofstream f(a.output / "execution.json");
      f << "{\"variant\":\"native_cuda\",\"dispatch\":\"" << a.dispatch
        << "\",\"replay\":\"UNSUPPORTED\",\"qualification\":\"UNQUALIFIED\","
           "\"ticks\":"
        << a.scenario.ticks << ",\"device_providers\":" << providers
        << ",\"device_publications\":" << publications
        << ",\"device_timeline\":" << timeline.completed_value
        << ",\"oracle\":true,\"cleanup\":true}\n";
      f.close();
      if (f.fail())
        return 1;
    }
    std::cout << "native_cuda execution=PASS replay=UNSUPPORTED "
                 "qualification=UNQUALIFIED cleanup=PASS\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
