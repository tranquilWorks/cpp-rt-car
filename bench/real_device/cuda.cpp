// Explicit opt-in host executable. Never called by portable metadata discovery.
#include "device_provider.hpp"
#include "cuda_resources.hpp"
#include <iostream>

namespace b = rtfw::benchmark;
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
        b::device::native::Resources resources;
        b::device::CudaSession session;
        const auto opened=resources.open(session,bytes,selected.starts_with("real-cuda-graph-"));
        if(opened!=b::Status::ok && opened!=b::Status::not_run) return 1;
        const bool available=opened==b::Status::ok;
        b::device::Provider provider(available ? &session : nullptr);
        b::Runner runner; b::ProviderHandle handle;
        if(runner.register_provider(provider.table(),handle)!=b::Status::ok) return 1;
        const auto prepared=provider.prepare(selected);
        if(prepared!=b::Status::ok && prepared!=b::Status::not_run) return 1;
        auto identity=b::capture_identity();
        identity.backend=available ? "cuda-driver-api" : "not_available";
        identity.driver=available ? "nvidia-driver-"+std::to_string(resources.version) : "not_available";
        const auto result=runner.run("rtfw.device",selected,b::steady_clock(),identity);
        if(provider.finish()!=b::Status::ok || !resources.close()) return 1;
        if(b::publish(result,argv[2])!=b::Status::ok) return 1;
        std::cout<<"status="<<b::status_name(result.status)<<" measured="<<result.samples.size()<<'\n';
        return result.status==b::Status::ok ? 0 : result.status==b::Status::not_run ? 3 : 1;
    } catch(...) { std::cerr<<"CUDA benchmark failed\n"; return 1; }
}
