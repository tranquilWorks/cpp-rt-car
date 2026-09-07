#include <rtfw/benchmark.hpp>
#include "cpu_provider.hpp"
#include <iostream>
#include <map>

namespace b = rtfw::benchmark;
namespace {
struct Self { std::uint64_t calls{}; };
b::Status describe(void*, std::size_t index, b::Descriptor& d) {
    if (index != 0) return b::Status::not_found;
    d.case_id="structural"; d.subsystem="framework"; d.implementation="self-v1";
    d.configuration="fixed-16"; d.workload_kind="integer-checksum";
    d.workload_sha256=b::sha256("rtfw.self.structural.v1.count=16");
    d.parameters={{"count",16,16,16}}; d.counters={{"operations","count",16,16}};
    return b::Status::ok;
}
b::Status invoke(void* user, std::string_view id, std::uint64_t ordinal, b::Observation& out) {
    auto& state=*static_cast<Self*>(user);
    if (id!="structural" || ordinal!=state.calls++) return b::Status::provider_error;
    std::uint64_t sum=0;
    for (std::uint64_t n=0; n<16; ++n) sum+=n;
    out.counters={16}; out.checksum=sum; out.correct=sum==120;
    return b::Status::ok;
}
bool fake_clock(void* user,std::uint64_t& ns) {
    auto& counter=*static_cast<std::uint64_t*>(user);
    ns=counter; counter+=100; return true;
}
int usage() {
    std::cerr << "usage: rtfw-bench list | describe --provider ID --case ID [--clock fake|steady]\n"
                 "       rtfw-bench run --provider ID --case ID --output NEW_DIRECTORY [--clock fake|steady]\n";
    return 2;
}
}
int main(int argc,char** argv) {
    try {
        if (argc<2 || argc>12) return usage();
        const std::string command=argv[1];
        if (command!="list" && command!="describe" && command!="run") return usage();
        std::map<std::string,std::string> options;
        for (int n=2; n<argc; n+=2) {
            if (n+1>=argc) return usage();
            const std::string key=argv[n], value=argv[n+1];
            if ((key!="--provider" && key!="--case" && key!="--clock" && key!="--output") ||
                value.empty() || !options.emplace(key,value).second) return usage();
        }
        if (command=="list" && !options.empty()) return usage();
        if (command!="list" && (!options.count("--provider") || !options.count("--case"))) return usage();
        if ((command=="run") != static_cast<bool>(options.count("--output"))) return usage();
        if (options.count("--clock") && options.at("--clock")!="fake" && options.at("--clock")!="steady") return usage();
        if (command=="run") {
            const auto checked=b::check_destination(options.at("--output"));
            if (checked!=b::Status::ok) { std::cerr << "output rejected: " << b::status_name(checked) << '\n'; return 2; }
        }
        Self self;
        b::cpu::Provider cpu;
        b::ProviderV1 provider;
        provider.id="rtfw.self"; provider.case_count=1; provider.user=&self;
        provider.describe=describe; provider.invoke=invoke;
        b::Runner runner; b::ProviderHandle handle;
        if (runner.register_provider(provider,handle)!=b::Status::ok) return 1;
        b::ProviderHandle cpu_handle;
        if (runner.register_provider(cpu.table(),cpu_handle)!=b::Status::ok) return 1;
        if (command=="list") {
            for (const auto& id:runner.list()) std::cout << id << '\n';
        } else {
            b::Descriptor d;
            if (runner.describe(options.at("--provider"),options.at("--case"),d)!=b::Status::ok) return usage();
            const auto kind=options.count("--clock") && options.at("--clock")=="fake" ? b::ClockKind::fake : b::ClockKind::steady;
            const auto& selected=options.at("--provider");
            if (command=="describe") std::cout << b::encode_descriptor(selected,1,d,kind);
            else {
                if (selected=="rtfw.cpu") {
                    const auto prepared=cpu.prepare(d.case_id);
                    if (prepared!=b::Status::ok && prepared!=b::Status::not_run) {
                        std::cerr << "CPU fixture preparation failed\n";
                        return 1;
                    }
                }
                std::uint64_t counter=0;
                auto clock=b::steady_clock();
                if (kind==b::ClockKind::fake) { clock.kind=kind; clock.user=&counter; clock.read_ns=fake_clock; }
                const auto result=runner.run(selected,d.case_id,clock,b::capture_identity());
                // A failed checked stop must never publish a success bundle.
                // The unchanged schema cannot encode a post-run cleanup error.
                if (cpu.finish()!=b::Status::ok) {
                    std::cerr << "CPU fixture cleanup failed\n";
                    return 1;
                }
                const auto written=b::publish(result,options.at("--output"));
                if (written!=b::Status::ok) { std::cerr << "publication failed: " << b::status_name(written) << '\n'; return 1; }
                std::cout << "status=" << b::status_name(result.status) << " evidence="
                    << (kind==b::ClockKind::fake ? "structural_fixture" : "portable_characterization")
                    << " measured=" << result.samples.size() << '\n';
                if (result.status!=b::Status::ok) return result.status==b::Status::not_run ? 3 : 1;
            }
        }
        return runner.unregister_provider(cpu_handle)==b::Status::ok &&
               runner.unregister_provider(handle)==b::Status::ok ? 0 : 1;
    } catch (...) {
        // Native exceptions may contain paths or input; never echo their text.
        std::cerr << "benchmark operation failed\n";
        return 1;
    }
}
