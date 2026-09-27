#include "cli.hpp"
#include <rt/cuda_driver.hpp>
#include <cuda.h>
namespace p=rtfw::cuda_physics::pipeline;
namespace {
class Resources {
    CUdevice device_{};
    CUcontext context_{};
    CUmodule module_{};
    std::array<CUstream,2> streams_{};
    std::array<CUgraph,2> graphs_{};
    std::array<CUgraphExec,2> executables_{};
    std::array<CUdeviceptr,4> buffers_{};
    bool current_{};
public:
    p::Session session;
    int open(const p::Options& o) noexcept {
        const auto initialized=cuInit(0);
        if (initialized==CUDA_ERROR_NO_DEVICE || initialized==CUDA_ERROR_STUB_LIBRARY) return 3;
        if (initialized!=CUDA_SUCCESS) return 1;
        int devices{};
        if (cuDeviceGetCount(&devices)!=CUDA_SUCCESS) return 1;
        if (!devices) return 3;
        if (cuDeviceGet(&device_,0)!=CUDA_SUCCESS || cuDevicePrimaryCtxRetain(&context_,device_)!=CUDA_SUCCESS) return 1;
        if (cuCtxPushCurrent(context_)!=CUDA_SUCCESS) return 1;
        current_=true;
        CUfunction function{};
        if (cuModuleLoad(&module_,RTFW_PHYSICS_PTX_PATH)!=CUDA_SUCCESS ||
            cuModuleGetFunction(&function,module_,"rtfw_particle_step")!=CUDA_SUCCESS) return 1;
        for (std::size_t i=0;i<p::lanes(o);++i) {
            if (cuStreamCreate(&streams_[i],CU_STREAM_NON_BLOCKING)!=CUDA_SUCCESS) return 1;
            const auto n=p::count(o,i);
            for (std::size_t j=2*i;j<2*i+2;++j) {
                session.bytes[j]=n*sizeof(rtfw::cuda_physics::Particle);
                if (cuMemAlloc(&buffers_[j],static_cast<std::size_t>(session.bytes[j]))!=CUDA_SUCCESS) return 1;
                session.buffers[j]=buffers_[j];
            }
            if (o.graph) {
                if (cuGraphCreate(&graphs_[i],0)!=CUDA_SUCCESS) return 1;
                auto elements=n;
                void* arguments[]{&buffers_[2*i+1],&elements};
                CUDA_KERNEL_NODE_PARAMS parameters{};
                parameters.func=function; parameters.gridDimX=(n+127)/128;
                parameters.gridDimY=parameters.gridDimZ=1;
                parameters.blockDimX=128; parameters.blockDimY=parameters.blockDimZ=1;
                parameters.kernelParams=arguments;
                CUgraphNode node{};
                if (cuGraphAddKernelNode(&node,graphs_[i],nullptr,0,&parameters)!=CUDA_SUCCESS ||
                    cuGraphInstantiateWithFlags(&executables_[i],graphs_[i],0)!=CUDA_SUCCESS) return 1;
                session.graphs[i]=reinterpret_cast<rt::CudaGraphExec>(executables_[i]);
            }
            session.streams[i]=reinterpret_cast<rt::CudaStream>(streams_[i]);
        }
        CUcontext popped{};
        if (cuCtxPopCurrent(&popped)!=CUDA_SUCCESS || popped!=context_) return 1;
        current_=false;
        session.driver=rt::cuda_driver_api(); session.context=reinterpret_cast<rt::CudaContext>(context_);
        session.function=reinterpret_cast<rt::CudaFunction>(function); return 0;
    }
    bool close() noexcept {
        if (!context_) return true;
        if (!current_) { if (cuCtxPushCurrent(context_)!=CUDA_SUCCESS) return false; current_=true; }
        for (auto& graph:executables_) if (graph) { if (cuGraphExecDestroy(graph)!=CUDA_SUCCESS) return false; graph=nullptr; }
        for (auto& graph:graphs_) if (graph) { if (cuGraphDestroy(graph)!=CUDA_SUCCESS) return false; graph=nullptr; }
        for (auto& buffer:buffers_) if (buffer) { if (cuMemFree(buffer)!=CUDA_SUCCESS) return false; buffer=0; }
        if (module_) { if (cuModuleUnload(module_)!=CUDA_SUCCESS) return false; module_=nullptr; }
        for (auto& stream:streams_) if (stream) { if (cuStreamDestroy(stream)!=CUDA_SUCCESS) return false; stream=nullptr; }
        CUcontext popped{};
        if (cuCtxPopCurrent(&popped)!=CUDA_SUCCESS || popped!=context_) return false;
        current_=false;
        if (cuDevicePrimaryCtxRelease(device_)!=CUDA_SUCCESS) return false;
        context_=nullptr; return true;
    }
    ~Resources() { if (!close()) std::terminate(); }
};
}
int main(int argc,char** argv) {
    p::Options o; const auto parsed=p::parse(argc,argv,o,true); if (parsed) return parsed==3?0:parsed;
    try {
        Resources resources;
        const auto opened=resources.open(o);
        if (opened) {
            if (!resources.close()) return 1;
            if (opened==3) std::cout<<"status=NOT_RUN reason=CUDA_device_or_driver_unavailable\n";
            return opened;
        }
        auto scenario=std::make_unique<p::Scenario>(o,resources.session);
        auto status=scenario->prepare();
        if (status==rt::Status::ok && o.active && !scenario->active_plan()) status=rt::Status::internal_error;
        if (status==rt::Status::ok) status=scenario->start();
        if (status==rt::Status::ok) status=scenario->run();
        if (status!=rt::Status::ok) std::cerr<<scenario->error()<<'\n';
        const auto stopped=scenario->stop();
        if (stopped!=rt::Status::ok && scenario->stop()!=rt::Status::ok) std::terminate();
        const auto completed=scenario->completed(); scenario.reset();
        const bool closed=resources.close();
        if (status!=rt::Status::ok || stopped!=rt::Status::ok || !closed) return 1;
        p::summary(o,completed,"real-CUDA-unqualified"); return 0;
    } catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
