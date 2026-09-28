#include "../samples/cuda_physics/benchmark/provider.hpp"
#include "cuda_physics/allocation_guard.hpp"
#include <iostream>
#include <thread>
namespace b=rtfw::benchmark;
namespace p=rtfw::cuda_physics::benchmark;
#define CHECK(x) do {if(!(x)){std::cerr<<__LINE__<<": " #x <<'\n';return false;}}while(false)
bool exercise(p::Provider& provider,const p::Case& c,bool track) {
    const auto table=provider.table();b::Observation out;out.counters.reserve(16);
    CHECK(provider.correlations().empty());
    CHECK(table.invoke(table.user,c.id,1,out)==b::Status::invalid);
    if(track) rtfw_physics_allocation::begin();
    b::Status status=b::Status::ok;
    for(std::uint64_t i=0;i<7 && status==b::Status::ok;++i) status=table.invoke(table.user,c.id,i,out);
    const auto allocations=track?rtfw_physics_allocation::end():0;
    CHECK(status==b::Status::ok && allocations==0 && out.correct);
    CHECK(table.invoke(table.user,c.id,7,out)==b::Status::invalid);
    CHECK(provider.correlations().empty());
    CHECK(provider.finish()==b::Status::ok && provider.finish()==b::Status::ok);
    const auto records=provider.correlations();const auto lanes=c.entities==1?1u:2u;
    CHECK(records.size()==7*lanes);
    for(std::size_t i=0;i<records.size();++i) {
        const auto& r=records[i];
        CHECK(r.id==i+1 && r.invocation==i/lanes && r.lane==i%lanes && r.timeline_value==i/lanes+1);
        CHECK(r.commands==5 && r.host_submit_ns<=r.host_complete_ns);
    }
    return true;
}
bool replay_boundary() {
    namespace pipe=rtfw::cuda_physics::pipeline;
    pipe::Options options{{1,1,1,1},false,false};
    auto driver=std::make_unique<pipe::SimulatedDriver>(options);
    const auto session=driver->session();
    rt::CudaBackendConfig backend_config;backend_config.context=session.context;
    backend_config.streams=std::span(session.streams.data(),1);
    rt::CudaDeviceBackend backend(session.driver,backend_config);
    rt::Runtime runtime;rt::RuntimeConfig config;
    config.worker_count=1;config.device_backend_capacity=1;
    config.determinism_tier=rt::DeterminismTier::schedule_independent;
    CHECK(runtime.configure(config)==rt::Status::ok);
    rt::DeviceBackendHandle handle;
    CHECK(runtime.register_device_backend(backend.hal_v2_registration("cuda-replay-boundary"),handle)==rt::Status::ok);
    rt::PhaseHandle phase;
    CHECK(runtime.register_device_phase({"cuda",handle,
        [](void*,const rt::DeviceCallbackContext&,rt::DeviceSubmission&) noexcept {
            return rt::CallbackResult::error;},nullptr},phase)==rt::Status::ok);
    CHECK(runtime.finalize()==rt::Status::invalid_config);
    CHECK(driver->clean());return true;
}
bool all() {
    CHECK(replay_boundary());
    p::Provider provider;const auto table=provider.table();b::Observation out;out.counters.reserve(16);
    CHECK(provider.prepare("unknown")==b::Status::not_found);
    CHECK(table.invoke(table.user,"unknown",0,out)==b::Status::invalid);
    for(const auto& c:p::cases()) {
        CHECK(provider.prepare(c.id,0)==b::Status::capacity);
        CHECK(provider.prepare(c.id,15)==b::Status::capacity);
        const auto capacity=c.entities==1?7u:14u;
        CHECK(provider.prepare(c.id,capacity-1)==b::Status::capacity);
        CHECK(provider.prepare(c.id,capacity)==b::Status::ok);
        CHECK(provider.prepare(c.id)==b::Status::busy);
        CHECK(exercise(provider,c,true));
    }
    CHECK(provider.prepare(p::cases()[0].id)==b::Status::ok);
    provider.corrupt_next_output();
    CHECK(table.invoke(table.user,p::cases()[0].id,0,out)==b::Status::invariant_failed && !out.correct);
    CHECK(table.invoke(table.user,p::cases()[0].id,1,out)==b::Status::invalid);
    CHECK(provider.finish()==b::Status::ok && provider.correlations().empty());
    // Configure on the owner thread; concurrently execute two independent kits.
    p::Provider left,right;const auto& a=p::cases()[6];const auto& c=p::cases()[19];
    CHECK(left.prepare(a.id)==b::Status::ok && right.prepare(c.id)==b::Status::ok);
    bool first=false,second=false;
    std::thread one([&]{first=exercise(left,a,false);});
    std::thread two([&]{second=exercise(right,c,false);});one.join();two.join();
    CHECK(first && second);return true;
}
int main(){try{return all()?0:1;}catch(...){return 1;}}
