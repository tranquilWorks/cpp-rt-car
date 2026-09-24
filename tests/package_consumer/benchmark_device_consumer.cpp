#include "device_provider.hpp"
#include <iostream>
namespace b=rtfw::benchmark;
namespace {
bool tick(void* opaque,std::uint64_t& ns) { auto& n=*static_cast<std::uint64_t*>(opaque); ns=n; n+=10; return true; }
}
int main(int argc,char** argv) {
    try {
        if(argc>2) return 2;
        b::device::Provider provider; b::Runner runner; b::ProviderHandle handle;
        if(runner.register_provider(provider.table(),handle)!=b::Status::ok) return 1;
        constexpr auto id="cuda-graph-4096";
        if(provider.prepare(id)!=b::Status::ok) return 1;
        std::uint64_t time=0; auto clock=b::steady_clock(); clock.kind=b::ClockKind::fake; clock.read_ns=tick; clock.user=&time;
        b::Identity identity; identity.host_label="device-fixture";
        const auto result=runner.run("rtfw.device",id,clock,identity);
        if(provider.finish()!=b::Status::ok || result.status!=b::Status::ok || result.samples.size()!=5) return 1;
        for(std::size_t i=0;i<result.samples.size();++i) {
            const auto& sample=result.samples[i];
            const auto expected=1024*((i+2)*7+1)+3*1024*1023/2;
            if(sample.observation.checksum!=expected || sample.observation.counters[0]!=1 ||
                sample.observation.counters[1]!=1 || sample.observation.counters[4]!=8192 || sample.end_ns-sample.start_ns!=10) return 1;
        }
        if(argc==2 && b::publish(result,argv[1])!=b::Status::ok) return 1;
        if(runner.unregister_provider(handle)!=b::Status::ok) return 1;
        std::cout<<"Device installed-source provider passed\n"; return 0;
    } catch(...) { return 1; }
}
