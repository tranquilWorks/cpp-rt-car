#include "provider.hpp"
#include <iostream>
#include <set>
namespace b=rtfw::benchmark;
namespace p=rtfw::cuda_physics::benchmark;
namespace {
bool tick(void* user,std::uint64_t& out) {
    auto& value=*static_cast<std::uint64_t*>(user);out=value;value+=100;return true;
}
}
int main(int argc,char** argv) {
    try {
        if(argc==2 && std::string_view(argv[1])=="--help") {
            std::cout<<"rtfw-bench-cuda-physics list | --case ID --clock fake|steady --output NEW_DIRECTORY\n"
                <<"Portable simulated-driver-protocol; full step includes copies, oracle and host correlation.\n";
            return 0;
        }
        p::Provider provider;
        if(argc==2 && std::string_view(argv[1])=="list") {
            for(const auto& c:p::cases()) {std::cout<<c.id<<'\n';}
            return 0;
        }
        std::string id,kind="steady",destination;std::set<std::string> seen;
        for(int i=1;i<argc;i+=2) {
            const std::string option=argv[i];if(i+1>=argc || !seen.insert(option).second) return 2;
            if(option=="--case") id=argv[i+1];
            else if(option=="--clock") kind=argv[i+1];
            else if(option=="--output") destination=argv[i+1];
            else return 2;
        }
        if(id.empty() || destination.empty() || (kind!="fake" && kind!="steady") ||
            b::check_destination(destination)!=b::Status::ok) return 2;
        b::Runner runner;b::ProviderHandle handle;
        if(runner.register_provider(provider.table(),handle)!=b::Status::ok) return 1;
        const auto prepared=provider.prepare(id);
        if(prepared!=b::Status::ok) return prepared==b::Status::not_found?2:1;
        auto identity=b::capture_identity();identity.backend="public-runtime-cuda-particle";
        identity.driver="simulated-driver-protocol";
        std::uint64_t time=0;auto clock=b::steady_clock();
        if(kind=="fake") {clock.kind=b::ClockKind::fake;clock.user=&time;clock.read_ns=tick;}
        const auto result=runner.run("rtfw.cuda-physics",id,clock,identity);
        if(provider.finish()!=b::Status::ok || result.status!=b::Status::ok) return 1;
        if(b::publish(result,destination)!=b::Status::ok) return 1;
        // stdout is the separate version1 host correlation stream; the M23
        // artifact directory remains exactly its unchanged three-file bundle.
        std::cout<<"{\"version\":1,\"clock\":\"host-steady-ns\",\"device_timestamps\":\"not_available\","
            <<"\"provider\":\"rtfw.cuda-physics\",\"case\":\""<<id<<"\",\"records\":[";
        bool first=true;
        for(const auto& r:provider.correlations()) {
            if(!first) {std::cout<<',';}
            first=false;
            std::cout<<"{\"id\":"<<r.id<<",\"invocation\":"<<r.invocation<<",\"lane\":"<<r.lane
                <<",\"timeline_value\":"<<r.timeline_value<<",\"commands\":"<<r.commands
                <<",\"host_submit_ns\":"<<r.host_submit_ns<<",\"host_complete_ns\":"<<r.host_complete_ns<<'}';
        }
        std::cout<<"]}\n";return std::cout?0:1;
    } catch(...) {return 1;}
}
