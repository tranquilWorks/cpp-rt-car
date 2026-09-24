// Explicit opt-in host executable. Never called by portable metadata discovery.
#include "device_provider.hpp"
#include "increment_ptx.hpp"
#include <rt/cuda_driver.hpp>
#include <cuda.h>
#include <iostream>

namespace b = rtfw::benchmark;
namespace {
struct Resources {
    CUdevice device{};
    CUcontext context{};
    CUstream stream{};
    CUmodule module{};
    CUfunction function{};
    CUgraph graph{};
    CUgraphExec graph_exec{};
    CUdeviceptr graph_buffer{};
    bool current{};

    bool close() noexcept {
        if (!context) return true;
        if (!current) {
            if (cuCtxPushCurrent(context)!=CUDA_SUCCESS) return false;
            current=true;
        }
        if(graph_exec) {
            if(cuGraphExecDestroy(graph_exec)!=CUDA_SUCCESS) return false;
            graph_exec=nullptr;
        }
        if(graph) {
            if(cuGraphDestroy(graph)!=CUDA_SUCCESS) return false;
            graph=nullptr;
        }
        if(graph_buffer) {
            if(cuMemFree(graph_buffer)!=CUDA_SUCCESS) return false;
            graph_buffer=0;
        }
        if (module) {
            if (cuModuleUnload(module)!=CUDA_SUCCESS) return false;
            module=nullptr;
        }
        if (stream) {
            if (cuStreamDestroy(stream)!=CUDA_SUCCESS) return false;
            stream=nullptr;
        }
        CUcontext popped{};
        if (cuCtxPopCurrent(&popped)!=CUDA_SUCCESS || popped!=context) return false;
        current=false;
        if (cuDevicePrimaryCtxRelease(device)!=CUDA_SUCCESS) return false;
        context=nullptr;
        return true;
    }
    ~Resources() { if (!close()) std::terminate(); }
};
}
int main(int argc, char** argv) {
    if (argc!=3) {
        std::cerr<<"usage: rtfw-bench-cuda REAL_CUDA_CASE NEW_OUTPUT_DIRECTORY\n";
        return 2;
    }
    try {
        const std::string_view selected=argv[1];
        const auto catalog=b::device::cases();
        bool allowed=false;
        std::size_t bytes=0;
        for(const auto& c:catalog) if(c.real && selected==c.id && selected.starts_with("real-cuda-")) { allowed=true; bytes=c.bytes; }
        if(!allowed || b::check_destination(argv[2])!=b::Status::ok) return 2;
        Resources resources;
        b::device::CudaSession session;
        bool available=true;
        const auto initialized=cuInit(0);
        if(initialized==CUDA_ERROR_NO_DEVICE) available=false;
        else if(initialized!=CUDA_SUCCESS) return 1;
        if(available) {
            const auto found=cuDeviceGet(&resources.device,0);
            if(found==CUDA_ERROR_NO_DEVICE || found==CUDA_ERROR_INVALID_DEVICE) available=false;
            else if(found!=CUDA_SUCCESS) return 1;
        }
        int version=0;
        if(available) {
            if(cuDevicePrimaryCtxRetain(&resources.context,resources.device)!=CUDA_SUCCESS) return 1;
            if(cuCtxPushCurrent(resources.context)!=CUDA_SUCCESS) return 1;
            resources.current=true;
            if(cuStreamCreate(&resources.stream,CU_STREAM_NON_BLOCKING)!=CUDA_SUCCESS ||
               cuModuleLoadData(&resources.module,b::device::native::kPtx.data())!=CUDA_SUCCESS ||
               cuModuleGetFunction(&resources.function,resources.module,"rtfw_add_one")!=CUDA_SUCCESS ||
               cuDriverGetVersion(&version)!=CUDA_SUCCESS) return 1;
            if(selected.starts_with("real-cuda-graph-")) {
                if(cuMemAlloc(&resources.graph_buffer,bytes)!=CUDA_SUCCESS ||
                   cuGraphCreate(&resources.graph,0)!=CUDA_SUCCESS) return 1;
                auto count=static_cast<unsigned>(bytes/4);
                void* arguments[]{&resources.graph_buffer,&count};
                CUDA_KERNEL_NODE_PARAMS kernel{};
                kernel.func=resources.function; kernel.gridDimX=(count+127)/128;
                kernel.gridDimY=kernel.gridDimZ=1;
                kernel.blockDimX=128; kernel.blockDimY=kernel.blockDimZ=1;
                kernel.kernelParams=arguments;
                CUgraphNode node{};
                if(cuGraphAddKernelNode(&node,resources.graph,nullptr,0,&kernel)!=CUDA_SUCCESS ||
                   cuGraphInstantiate(&resources.graph_exec,resources.graph,nullptr,nullptr,0)!=CUDA_SUCCESS) return 1;
                session.increment_graph=reinterpret_cast<rt::CudaGraphExec>(resources.graph_exec);
                session.graph_buffer=resources.graph_buffer; session.graph_bytes=bytes;
            }
            CUcontext popped{};
            if(cuCtxPopCurrent(&popped)!=CUDA_SUCCESS || popped!=resources.context) return 1;
            resources.current=false;
            session.driver=rt::cuda_driver_api();
            session.context=reinterpret_cast<rt::CudaContext>(resources.context);
            session.stream=reinterpret_cast<rt::CudaStream>(resources.stream);
            session.increment_kernel=reinterpret_cast<rt::CudaFunction>(resources.function);
        }
        b::device::Provider provider(available ? &session : nullptr);
        b::Runner runner; b::ProviderHandle handle;
        if(runner.register_provider(provider.table(),handle)!=b::Status::ok) return 1;
        const auto prepared=provider.prepare(selected);
        if(prepared!=b::Status::ok && prepared!=b::Status::not_run) return 1;
        auto identity=b::capture_identity();
        identity.backend=available ? "cuda-driver-api" : "not_available";
        identity.driver=available ? "nvidia-driver-"+std::to_string(version) : "not_available";
        const auto result=runner.run("rtfw.device",selected,b::steady_clock(),identity);
        if(provider.finish()!=b::Status::ok || !resources.close()) return 1;
        if(b::publish(result,argv[2])!=b::Status::ok) return 1;
        std::cout<<"status="<<b::status_name(result.status)<<" measured="<<result.samples.size()<<'\n';
        return result.status==b::Status::ok ? 0 : result.status==b::Status::not_run ? 3 : 1;
    } catch(...) { std::cerr<<"CUDA benchmark failed\n"; return 1; }
}
