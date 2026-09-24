// Both real adapters, explicit XDMA scratch window and caller-created CUDA graph.
#include "cuda_resources.hpp"
#include <rt/xdma_linux.hpp>
#include <array>
#include <charconv>
#include <filesystem>
#include <iostream>
namespace b=rtfw::benchmark;
int main(int argc,char** argv) {
    if(argc!=7) {
        std::cerr<<"usage: rtfw-bench-pipeline REAL_PIPELINE_CASE NEW_OUTPUT_DIRECTORY H2C_PATH C2H_PATH CONFIRMED_OFFSET CONFIRMED_BYTES\n";
        return 2;
    }
    try {
        const std::string_view selected=argv[1];
        const b::device::Case* chosen=nullptr;
        for(const auto& c:b::device::cases()) if(c.real && selected==c.id && selected.starts_with("real-pipeline-")) chosen=&c;
        if(!chosen || b::check_destination(argv[2])!=b::Status::ok) return 2;
        const auto number=[](std::string_view text,std::uint64_t& value) {
            const auto parsed=std::from_chars(text.data(),text.data()+text.size(),value);
            return parsed.ec==std::errc{} && parsed.ptr==text.data()+text.size();
        };
        b::device::XdmaSession xs;
        if(!number(argv[5],xs.device_offset) || !number(argv[6],xs.confirmed_window_bytes) ||
            xs.confirmed_window_bytes<chosen->bytes || xs.device_offset>UINT64_MAX-chosen->bytes) return 2;
        bool available=std::filesystem::exists(argv[3]) && std::filesystem::exists(argv[4]);
        const std::array<std::string_view,1> h2c{argv[3]},c2h{argv[4]};
        rt::LinuxXdmaConfig config{}; config.h2c_paths=h2c; config.c2h_paths=c2h;
        rt::LinuxXdmaDriver xdma(config);
        xs.driver=xdma.api(); xs.config.queue_capacity=4; xs.config.buffer_capacity=1; xs.config.worker_count=1;
        xs.config.max_transfer_bytes=4096; xs.config.max_buffer_bytes=4096;
        b::device::native::Resources cuda;
        b::device::CudaSession cs;
        if(available) {
            const auto status=cuda.open(cs,chosen->bytes,std::string_view(chosen->operation)=="pipeline-graph");
            if(status!=b::Status::ok && status!=b::Status::not_run) return 1;
            available=status==b::Status::ok;
        }
        b::device::Provider provider(available ? &cs : nullptr,available ? &xs : nullptr);
        b::Runner runner; b::ProviderHandle handle;
        if(runner.register_provider(provider.table(),handle)!=b::Status::ok) return 1;
        const auto prepared=provider.prepare(selected);
        if(prepared!=b::Status::ok && prepared!=b::Status::not_run) return 1;
        auto identity=b::capture_identity();
        identity.backend=available ? "runtime-cuda-host-staging-xdma" : "not_available";
        identity.driver=available ? "nvidia-"+std::to_string(cuda.version)+"-linux-xdma" : "not_available";
        const auto result=runner.run("rtfw.device",selected,b::steady_clock(),identity);
        if(provider.finish()!=b::Status::ok || !cuda.close() || b::publish(result,argv[2])!=b::Status::ok) return 1;
        std::cout<<"status="<<b::status_name(result.status)<<" measured="<<result.samples.size()<<'\n';
        return result.status==b::Status::ok ? 0 : result.status==b::Status::not_run ? 3 : 1;
    } catch(...) { std::cerr<<"Combined device benchmark failed\n"; return 1; }
}
