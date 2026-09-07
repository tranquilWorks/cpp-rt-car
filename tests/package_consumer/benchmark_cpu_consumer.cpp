#include "cpu_provider.hpp"
#include <iostream>
namespace b = rtfw::benchmark;
namespace {
bool tick(void* user, std::uint64_t& ns) { auto& n=*static_cast<std::uint64_t*>(user);ns=n;n+=10;return true; }
}
int main(int argc,char** argv) {
    try {
        if (argc>2) return 2;
        b::cpu::Provider cpu; b::Runner runner; b::ProviderHandle h;
        if (runner.register_provider(cpu.table(),h)!=b::Status::ok || cpu.prepared()) return 1;
        if (cpu.prepare("host-adapter-64")!=b::Status::ok) return 1;
        std::uint64_t n=0; auto clock=b::steady_clock();clock.kind=b::ClockKind::fake;clock.user=&n;clock.read_ns=tick;
        // Fixed declaration for portable byte-equality; no machine observation.
        b::Identity identity;identity.host_label="cpu-fixture";
        const auto result=runner.run("rtfw.cpu","host-adapter-64",clock,identity);
        if (cpu.finish()!=b::Status::ok || result.status!=b::Status::ok || result.samples.size()!=5) return 1;
        for (const auto& s:result.samples)
            if (s.observation.checksum!=6112 || s.observation.counters[0]!=64 || s.end_ns-s.start_ns!=10) return 1;
        if (argc==2 && b::publish(result,argv[1])!=b::Status::ok) return 1;
        if (runner.unregister_provider(h)!=b::Status::ok) return 1;
        std::cout<<"CPU installed-source provider passed\n";return 0;
    } catch (...) { return 1; }
}
