#include "../samples/cuda_physics/pipeline/simulated_driver.hpp"
#include "cuda_physics/allocation_guard.hpp"
#include <future>
#include <iostream>
#include <thread>
namespace p=rtfw::cuda_physics::pipeline;
using rtfw::cuda_physics::Particle;
#define CHECK(x) do { if (!(x)) { std::cerr<<__func__<<':'<<__LINE__<<" failed: " #x <<'\n'; return false; } } while(false)

bool run_case(p::Options o,Particle* output=nullptr,bool measure=true) {
    auto d=std::make_unique<p::SimulatedDriver>(o);
    auto s=std::make_unique<p::Scenario>(o,d->session());
    const auto prepared=s->prepare();
    if (prepared!=rt::Status::ok) std::cerr<<s->error()<<'\n';
    CHECK(prepared==rt::Status::ok);
    CHECK(s->active_plan()==o.active);
    CHECK(s->start()==rt::Status::ok);
    CHECK(!d->close()); // Registered host spans/events keep the caller owner live.
    if (measure) rtfw_physics_allocation::begin();
    const auto ran=s->run(); const auto allocated=measure?rtfw_physics_allocation::end():0;
    if (ran!=rt::Status::ok) std::cerr<<s->error()<<'\n';
    CHECK(ran==rt::Status::ok && allocated==0);
    CHECK(s->completed()==o.steps && s->step()==rt::Status::invalid_argument);
    for (std::size_t i=0;i<p::lanes(o);++i)
        CHECK(s->publications(i)==o.steps && s->preparations(i)==o.steps && s->submissions(i)==o.steps);
    const auto calls=static_cast<std::uint64_t>(p::lanes(o))*o.steps;
    CHECK(d->uploads==2*calls && d->copies==calls && d->downloads==calls && d->records==calls);
    CHECK(d->kernels==(o.graph?0:calls) && d->graphs==(o.graph?calls:0));
    CHECK(d->upload_bytes==2*static_cast<std::uint64_t>(o.count)*o.steps*sizeof(Particle));
    CHECK(2*d->copy_bytes==d->upload_bytes.load() && d->download_bytes==d->copy_bytes.load());
    if (output) for (std::size_t i=0;i<o.count;++i) output[i]=s->particle(i);
    CHECK(s->stop()==rt::Status::ok && s->stop()==rt::Status::ok && d->clean());
    CHECK(d->registrations==2*p::lanes(o) && d->unregistrations==d->registrations.load());
    CHECK(d->backend_allocations==0 && d->backend_frees==0 && d->protocol_ok.load());
    CHECK(d->close()); return true;
}

bool parity() {
    auto reference=std::make_unique<std::array<Particle,4096>>();
    auto actual=std::make_unique<std::array<Particle,4096>>();
    for (auto base:{rtfw::cuda_physics::Options{1,1,1,1},{17,7,0,2},{256,64,1,1},
                    {1024,7,0xffffffffu,2},{4095,64,1,2},{4096,1024,0xffffffffu,2}}) {
        CHECK(run_case({base,false,false},reference->data()));
        for (bool active:{false,true}) for (bool graph:{false,true}) {
            CHECK(run_case({base,graph,active},actual->data()));
            CHECK(std::memcmp(reference->data(),actual->data(),base.count*sizeof(Particle))==0);
        }
        if (base.seed==0 && base.steps==7) {
            CHECK((*actual)[0].position[0]==844 && (*actual)[0].position[1]==1065 && (*actual)[0].position[2]==350);
        }
    }
    return true;
}
bool overlap(bool active,bool graph) {
    p::Options o{{17,2,0,2},graph,active};
    auto d=std::make_unique<p::SimulatedDriver>(o);
    auto s=std::make_unique<p::Scenario>(o,d->session());
    CHECK(s->prepare()==rt::Status::ok && s->start()==rt::Status::ok);
    d->hold=true; rt::Status result=rt::Status::internal_error;
    std::thread worker([&]{result=s->step();});
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
    while ((d->records<2 || d->not_ready==0) && std::chrono::steady_clock::now()<deadline) std::this_thread::yield();
    const bool two=d->records==2 && d->stream_mask==3 && d->not_ready>0;
    const bool unpublished=s->publications(0)==0 && s->publications(1)==0;
    d->hold=false; worker.join();
    if (result!=rt::Status::ok) std::cerr<<s->error()<<'\n';
    CHECK(two && unpublished && result==rt::Status::ok);
    CHECK(s->step()==rt::Status::ok); // Both staging slots can be reused after terminal completion.
    CHECK(s->publications(0)==2 && s->publications(1)==2);
    CHECK(s->stop()==rt::Status::ok && d->clean()); return true;
}
bool failures() {
    p::Options o{{1,1,0,1},true,false};
    using Flag=std::atomic<bool> p::SimulatedDriver::*;
    for (Flag flag:{&p::SimulatedDriver::fail_graph,&p::SimulatedDriver::corrupt_d2d,&p::SimulatedDriver::corrupt_output}) {
        auto d=std::make_unique<p::SimulatedDriver>(o); auto s=std::make_unique<p::Scenario>(o,d->session());
        CHECK(s->prepare()==rt::Status::ok && s->start()==rt::Status::ok);
        (d.get()->*flag)=true;
        CHECK(s->step()!=rt::Status::ok && s->completed()==0 && s->publications(0)==0);
        CHECK(s->step()==rt::Status::invalid_state);
        CHECK(s->stop()==rt::Status::ok && d->clean());
        CHECK(d->backend_frees==0);
    }
    o.count=17;
    auto d=std::make_unique<p::SimulatedDriver>(o); auto s=std::make_unique<p::Scenario>(o,d->session());
    CHECK(s->prepare()==rt::Status::ok && s->start()==rt::Status::ok && s->run()==rt::Status::ok);
    d->fail_unregister=true;
    CHECK(s->stop()!=rt::Status::ok && !d->close());
    CHECK(s->stop()==rt::Status::ok && d->clean() && d->close());
    CHECK(d->registrations==4 && d->unregistrations==4 && d->backend_frees==0);
    return true;
}
bool malformed() {
    p::Options o{{17,1,0,1},true,false}; auto d=std::make_unique<p::SimulatedDriver>(o);
    for (unsigned which=0;which<7;++which) {
        auto session=d->session(); auto options=o;
        switch(which) {
        case 0:session.context=0;break;
        case 1:session.function=0;break;
        case 2:session.graphs[1]=0;break;
        case 3:session.streams[1]=session.streams[0];break;
        case 4:session.buffers[1]=session.buffers[0];break;
        case 5:--session.bytes[2];break;
        default:options.count=4097;break;
        }
        try {
            auto s=std::make_unique<p::Scenario>(options,session);
            CHECK(s->prepare()==rt::Status::invalid_argument && d->registrations==0 && d->clean());
        } catch (const std::invalid_argument&) { CHECK(d->registrations==0 && d->clean()); }
    }
    return true;
}
int main() {
    if (!malformed() || !failures()) return 1;
    for (bool active:{false,true}) for (bool graph:{false,true}) if (!overlap(active,graph)) return 1;
    if (!parity()) return 1;
    auto a=std::async(std::launch::async,[]{return run_case({{256,64,0,1},true,true},nullptr,false);});
    auto b=std::async(std::launch::async,[]{return run_case({{256,64,0xffffffffu,2},false,false},nullptr,false);});
    if (!a.get() || !b.get()) return 1;
    std::cout<<"CUDA pipeline: four-mode parity, observed overlap, active releases, ownership and zero-allocation PASS\n";
}
