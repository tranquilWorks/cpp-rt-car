#include "runtime_provider.hpp"
#include <iostream>
namespace b=rtfw::benchmark;
namespace {
bool tick(void* opaque,std::uint64_t& ns) {auto& n=*static_cast<std::uint64_t*>(opaque);ns=n;n+=10;return true;}
}
int main(int argc,char** argv) {
    try {
        if(argc>2) return 2;
        b::runtime::Provider provider;b::Runner runner;b::ProviderHandle handle;
        if(runner.register_provider(provider.table(),handle)!=b::Status::ok || provider.prepared()) return 1;
        constexpr auto id="rate-dispatch-8-d1-s1";
        if(provider.prepare(id)!=b::Status::ok) return 1;
        std::uint64_t time=0;auto clock=b::steady_clock();clock.kind=b::ClockKind::fake;clock.read_ns=tick;clock.user=&time;
        b::Identity identity;identity.host_label="runtime-fixture";
        const auto result=runner.run("rtfw.runtime",id,clock,identity);
        if(provider.finish()!=b::Status::ok || result.status!=b::Status::ok || result.samples.size()!=5) return 1;
        for(std::size_t i=0;i<result.samples.size();++i) {
            const auto& sample=result.samples[i];const auto n=8*(i+3);
            const auto expected=7*n*(n-1)/2+n;
            if(sample.observation.checksum!=expected || sample.observation.counters[0]!=8 || sample.end_ns-sample.start_ns!=10) return 1;
        }
        if(argc==2 && b::publish(result,argv[1])!=b::Status::ok) return 1;
        if(runner.unregister_provider(handle)!=b::Status::ok) return 1;
        std::cout<<"Runtime installed-source provider passed\n";return 0;
    } catch(...) {return 1;}
}
