#pragma once
#include "device_provider.hpp"
#include "increment_ptx.hpp"
#include <rt/cuda_driver.hpp>
#include <cuda.h>

namespace rtfw::benchmark::device::native {
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
    int version{};

    Status open(CudaSession& session,std::size_t bytes,bool use_graph) {
        bool available=true;
        const auto initialized=cuInit(0);
        if(initialized==CUDA_ERROR_NO_DEVICE) available=false;
        else if(initialized!=CUDA_SUCCESS) return Status::provider_error;
        if(available) {
            const auto found=cuDeviceGet(&device,0);
            if(found==CUDA_ERROR_NO_DEVICE || found==CUDA_ERROR_INVALID_DEVICE) available=false;
            else if(found!=CUDA_SUCCESS) return Status::provider_error;
        }
        if(available) {
            if(cuDevicePrimaryCtxRetain(&context,device)!=CUDA_SUCCESS) return Status::provider_error;
            if(cuCtxPushCurrent(context)!=CUDA_SUCCESS) return Status::provider_error;
            current=true;
            if(cuStreamCreate(&stream,CU_STREAM_NON_BLOCKING)!=CUDA_SUCCESS ||
               cuModuleLoadData(&module,native::kPtx.data())!=CUDA_SUCCESS ||
               cuModuleGetFunction(&function,module,"rtfw_add_one")!=CUDA_SUCCESS ||
               cuDriverGetVersion(&version)!=CUDA_SUCCESS) return Status::provider_error;
            if(use_graph) {
                if(cuMemAlloc(&graph_buffer,bytes)!=CUDA_SUCCESS ||
                   cuGraphCreate(&graph,0)!=CUDA_SUCCESS) return Status::provider_error;
                auto count=static_cast<unsigned>(bytes/4);
                void* arguments[]{&graph_buffer,&count};
                CUDA_KERNEL_NODE_PARAMS kernel{};
                kernel.func=function; kernel.gridDimX=(count+127)/128;
                kernel.gridDimY=kernel.gridDimZ=1;
                kernel.blockDimX=128; kernel.blockDimY=kernel.blockDimZ=1;
                kernel.kernelParams=arguments;
                CUgraphNode node{};
                if(cuGraphAddKernelNode(&node,graph,nullptr,0,&kernel)!=CUDA_SUCCESS ||
                   cuGraphInstantiateWithFlags(&graph_exec,graph,0)!=CUDA_SUCCESS) return Status::provider_error;
                session.increment_graph=reinterpret_cast<rt::CudaGraphExec>(graph_exec);
                session.graph_buffer=graph_buffer; session.graph_bytes=bytes;
            }
            CUcontext popped{};
            if(cuCtxPopCurrent(&popped)!=CUDA_SUCCESS || popped!=context) return Status::provider_error;
            current=false;
            session.driver=rt::cuda_driver_api();
            session.context=reinterpret_cast<rt::CudaContext>(context);
            session.stream=reinterpret_cast<rt::CudaStream>(stream);
            session.increment_kernel=reinterpret_cast<rt::CudaFunction>(function);
        }
        return available ? Status::ok : Status::not_run;
    }


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
} // namespace rtfw::benchmark::device::native
