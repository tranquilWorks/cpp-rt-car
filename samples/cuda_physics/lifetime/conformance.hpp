#pragma once
#include "protocol.hpp"
#include "../pipeline/simulated_driver.hpp"
#include <iostream>
#include <thread>

namespace rtfw::cuda_physics::lifetime {
namespace p=pipeline;
#define LIFETIME_CHECK(x) do { if (!(x)) { std::cerr<<__func__<<':'<<__LINE__<<" failed: " #x <<'\n'; return false; } } while(false)
using Status=rt::Status;
struct Fixture {
    p::Options options;
    Protocol protocol;
    std::unique_ptr<p::SimulatedDriver> driver;
    std::unique_ptr<p::Scenario> scenario;
    explicit Fixture(bool active=false,bool graph=true,std::uint32_t count=1,std::uint32_t seed=1)
        :options{{count,2,seed,2},graph,active},driver(std::make_unique<p::SimulatedDriver>(options)),
         scenario(std::make_unique<p::Scenario>(options,driver->session(),protocol.hooks())) {}
    bool start() {
        auto status=scenario->prepare();
        if(status==Status::ok) status=scenario->start();
        if(status!=Status::ok) std::cerr<<"start status "<<static_cast<int>(status)<<": "<<scenario->error()<<'\n';
        return status==Status::ok;
    }
    bool conserved() const {
        return driver->clean() && driver->event_creates==driver->event_destroys.load() &&
            driver->registrations==driver->unregistrations.load() && driver->backend_allocations==0 &&
            driver->backend_frees==0 && driver->protocol_ok.load();
    }
    bool finish() { return scenario->stop()==Status::ok && scenario->stop()==Status::ok && conserved(); }
};
template<class Predicate> bool await(Predicate predicate) {
    const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(3);
    while (!predicate() && std::chrono::steady_clock::now()<until) std::this_thread::yield();
    return predicate();
}
inline bool healthy(bool active,bool graph,std::uint32_t seed=1) {
    Fixture f(active,graph,17,seed); LIFETIME_CHECK(f.start());
    LIFETIME_CHECK(f.scenario->run()==Status::ok);
    LIFETIME_CHECK(f.scenario->completed()==2 && f.scenario->publications(0)==2 && f.scenario->publications(1)==2);
    LIFETIME_CHECK(f.driver->stream_syncs==0 && f.driver->event_syncs==0);
    LIFETIME_CHECK(f.finish());
    LIFETIME_CHECK(f.scenario->start()==Status::invalid_state);
    return true;
}
inline bool protocol_failure(bool active,bool graph,Protocol::Fault fault,Status expected) {
    Fixture f(active,graph); f.protocol.fault=fault; LIFETIME_CHECK(f.start());
    const auto status=f.scenario->step();
    if(status!=expected) std::cerr<<"status "<<static_cast<int>(status)<<" expected "<<static_cast<int>(expected)<<": "<<f.scenario->error()<<'\n';
    LIFETIME_CHECK(status==expected && f.protocol.injected==1 && f.protocol.submits==1);
    LIFETIME_CHECK(f.scenario->completed()==0 && f.scenario->publications(0)==0);
    rt::DeviceTimelineInfo timeline;
    LIFETIME_CHECK(f.scenario->timeline(0,timeline) && timeline.completed_value==0);
    LIFETIME_CHECK(f.finish()); return true;
}
inline bool invalid_provider(bool active,bool graph) {
    Fixture f(active,graph); f.protocol.invalid_provider=true; LIFETIME_CHECK(f.start());
    LIFETIME_CHECK(f.scenario->step()==Status::invalid_argument && f.protocol.provider_faults==1);
    LIFETIME_CHECK(f.protocol.submits==0 && f.driver->records==0 && f.scenario->publications(0)==0);
    LIFETIME_CHECK(f.finish()); return true;
}
inline bool stale(bool active,bool graph) {
    Fixture f(active,graph); f.protocol.fault=Protocol::Fault::stale_batch; LIFETIME_CHECK(f.start());
    LIFETIME_CHECK(f.scenario->step()==Status::ok && f.scenario->publications(0)==1);
    LIFETIME_CHECK(f.scenario->step()==Status::device_error && f.protocol.injected==1);
    LIFETIME_CHECK(f.scenario->completed()==1 && f.scenario->publications(0)==1);
    rt::DeviceTimelineInfo timeline;
    LIFETIME_CHECK(f.scenario->timeline(0,timeline) && timeline.completed_value==1 && timeline.last_accepted_value==2);
    LIFETIME_CHECK(f.finish()); return true;
}
inline bool timeout(bool active,bool graph) {
    Fixture f(active,graph); LIFETIME_CHECK(f.start()); f.driver->hold=true;
    Status result=Status::internal_error; std::thread worker([&]{result=f.scenario->step();});
    const bool reached=await([&]{return f.driver->records==1 && f.driver->not_ready>0;});
    const auto before=f.driver->not_ready.load();
    f.driver->clock_offset=p::completion_ns+1;
    const bool expired=await([&]{return f.driver->not_ready>before+4;});
    const bool unpublished=f.scenario->publications(0)==0;
    f.driver->hold=false; worker.join();
    LIFETIME_CHECK(reached && expired && unpublished && result==Status::device_timeout);
    rt::DeviceHealth health=rt::make_device_health();
    LIFETIME_CHECK(f.scenario->health(0,health)==Status::ok && health.timeouts==1);
    LIFETIME_CHECK(f.scenario->publications(0)==0 && f.scenario->completed()==0 && f.finish()); return true;
}
inline bool pending_stop(bool active,bool graph,bool fail_cleanup) {
    Fixture f(active,graph,17); LIFETIME_CHECK(f.start()); f.driver->hold=true;
    Status result=Status::internal_error; std::thread worker([&]{result=f.scenario->step();});
    const bool reached=await([&]{return f.driver->records==2 && f.driver->not_ready>0;});
    const auto requested=f.scenario->request_stop(); worker.join();
    LIFETIME_CHECK(reached && requested==Status::invalid_state && result==(active?Status::device_timeout:Status::device_canceled));
    LIFETIME_CHECK(f.protocol.stops>=2 && f.protocol.cancels>=2 && f.protocol.unsupported_cancels==f.protocol.cancels.load());
    LIFETIME_CHECK(f.scenario->publications(0)==0 && f.scenario->publications(1)==0 && !f.driver->clean());
    if(fail_cleanup) {
        f.driver->fail_event_sync=true;
        const auto stopped=f.scenario->stop();
        const bool retained=!f.driver->clean();
        const auto injected=f.driver->faults.load();
        f.driver->hold=false;
        const bool cleaned=f.finish();
        LIFETIME_CHECK(stopped!=Status::ok && injected==1 && retained && cleaned);
    }
    f.driver->hold=false;
    LIFETIME_CHECK(f.finish());
    LIFETIME_CHECK(f.scenario->publications(0)==0 && f.scenario->publications(1)==0);
    return true;
}
inline bool quarantine(bool active,bool graph,bool query) {
    Fixture f(active,graph); LIFETIME_CHECK(f.start());
    if(query) f.driver->fail_query=true;
    else if(graph) f.driver->fail_graph=true;
    else f.driver->fail_kernel=true;
    const auto result=f.scenario->step();
    if(result!=Status::device_reset_required) std::cerr<<"quarantine status "<<static_cast<int>(result)<<": "<<f.scenario->error()<<'\n';
    LIFETIME_CHECK(result==Status::device_reset_required && f.driver->faults==1);
    rt::DeviceHealth before=rt::make_device_health(),after=rt::make_device_health();
    LIFETIME_CHECK(f.scenario->health(0,before)==Status::ok && before.state==RTFW_DEVICE_HEALTH_RESET_REQUIRED && before.outstanding==1);
    f.driver->fail_stream_sync=true;
    LIFETIME_CHECK(f.scenario->reset(0)!=Status::ok && !f.driver->clean() && f.driver->faults==2);
    LIFETIME_CHECK(f.scenario->reset(0)==Status::ok);
    LIFETIME_CHECK(f.scenario->health(0,after)==Status::ok && after.state==RTFW_DEVICE_HEALTH_HEALTHY && after.resets==before.resets+1 && after.generation>before.generation && after.outstanding==0);
    LIFETIME_CHECK(f.scenario->completed()==0 && f.scenario->publications(0)==0 && f.finish());
    return healthy(active,graph,0xffffffffu);
}
inline bool loss(bool active,bool graph) {
    Fixture f(active,graph); LIFETIME_CHECK(f.start()); f.driver->lose_query=true;
    LIFETIME_CHECK(f.scenario->step()==Status::device_lost && f.driver->faults==1);
    rt::DeviceHealth health=rt::make_device_health();
    LIFETIME_CHECK(f.scenario->health(0,health)==Status::ok && health.state==RTFW_DEVICE_HEALTH_LOST && health.losses==1 && health.outstanding==1);
    LIFETIME_CHECK(f.scenario->reset(0)==Status::device_lost && f.scenario->publications(0)==0);
    f.driver->fail_stream_sync=true;
    LIFETIME_CHECK(f.scenario->stop()!=Status::ok && !f.driver->clean());
    LIFETIME_CHECK(f.finish());
    return healthy(active,graph,0);
}
inline bool cleanup(bool active,bool graph) {
    for(unsigned fault=0;fault<3;++fault) {
        Fixture f(active,graph,17);
        if(fault==0) {
            f.driver->fail_registration_at=2;
            LIFETIME_CHECK(f.scenario->prepare()==Status::ok && f.scenario->start()!=Status::ok);
            LIFETIME_CHECK(f.driver->faults==1 && f.scenario->publications(0)==0);
        } else {
            LIFETIME_CHECK(f.start() && f.scenario->run()==Status::ok);
            if(fault==1) f.driver->fail_unregister=true; else f.driver->fail_destroy=true;
            LIFETIME_CHECK(f.scenario->stop()!=Status::ok && !f.driver->clean() && f.driver->faults==1);
        }
        LIFETIME_CHECK(f.finish());
    }
    return true;
}
inline bool capacity(bool active,bool graph) {
    p::Options o{{17,1,1,2},graph,active}; auto d=std::make_unique<p::SimulatedDriver>(o);
    auto hooks=p::Instrumentation{}; hooks.outstanding_capacity=0;
    auto s=std::make_unique<p::Scenario>(o,d->session(),hooks);
    const auto status=s->prepare();
    LIFETIME_CHECK(status==Status::invalid_config && d->registrations==0 && d->clean());
    return true;
}
inline bool isolated() {
    // Configure on the control thread. Only steady stepping runs concurrently;
    // this keeps the actual 512 KiB CLI process-stack contract under TSan.
    Fixture good(true,true,17,0xffffffffu),bad(false,false,1,0);
    LIFETIME_CHECK(good.start() && bad.start());
    good.driver->hold=true; bad.driver->hold=true;
    Status good_status=Status::internal_error,bad_status=Status::internal_error;
    std::thread a([&]{good_status=good.scenario->run();});
    std::thread b([&]{bad_status=bad.scenario->step();});
    const bool overlapping=await([&]{return good.driver->records==2 && bad.driver->records==1 &&
        good.driver->not_ready>0 && bad.driver->not_ready>0;});
    bad.driver->lose_query=true;
    good.driver->hold=false; bad.driver->hold=false;
    a.join(); b.join();
    LIFETIME_CHECK(overlapping && good_status==Status::ok && bad_status==Status::device_lost);
    LIFETIME_CHECK(good.scenario->completed()==2 && good.scenario->publications(0)==2 &&
        good.scenario->publications(1)==2 && good.driver->faults==0);
    LIFETIME_CHECK(bad.scenario->completed()==0 && bad.scenario->publications(0)==0 && bad.driver->faults==1);
    rt::DeviceTimelineInfo timeline;
    rt::DeviceHealth health=rt::make_device_health();
    LIFETIME_CHECK(good.scenario->timeline(0,timeline) && timeline.completed_value==2);
    LIFETIME_CHECK(bad.scenario->health(0,health)==Status::ok && health.state==RTFW_DEVICE_HEALTH_LOST && health.outstanding==1);
    LIFETIME_CHECK(good.finish() && bad.finish());
    return true;
}
inline bool suite() {
    for(bool active:{false,true}) for(bool graph:{false,true}) {
        std::cout<<"lifetime "<<(active?"active":"frame")<<' '<<(graph?"graph":"kernel")<<'\n';
        if(!healthy(active,graph) || !capacity(active,graph) || !invalid_provider(active,graph) ||
           !protocol_failure(active,graph,Protocol::Fault::queue_full,Status::device_queue_full) ||
           !protocol_failure(active,graph,Protocol::Fault::invalid_descriptor,Status::invalid_argument) ||
           !protocol_failure(active,graph,Protocol::Fault::wrong_signal,Status::device_error) ||
           !stale(active,graph) || !timeout(active,graph) ||
           !pending_stop(active,graph,false) || !pending_stop(active,graph,true) ||
           !quarantine(active,graph,false) || !quarantine(active,graph,true) ||
           !loss(active,graph) || !cleanup(active,graph)) return false;
    }
    // Repeat complete caller/Runtime lifetimes, and isolate a simultaneous fault.
    for(unsigned n=0;n<8;++n) if(!healthy(n%2==0,n%3==0,n)) return false;
    if(!isolated()) return false;
    std::cout<<"CUDA lifetime conformance PASS (portable driver / HAL protocol evidence)\n";
    return true;
}
#undef LIFETIME_CHECK
}
