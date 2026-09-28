#include "provider.hpp"

namespace rtfw::cuda_physics::benchmark {
namespace {
constexpr Case catalog[]{
#define CASES(n,w) {"kernel-frame-" #n "-w" #w,n,w,false,false}, {"graph-frame-" #n "-w" #w,n,w,true,false}, \
 {"kernel-active-" #n "-w" #w,n,w,false,true}, {"graph-active-" #n "-w" #w,n,w,true,true}
    CASES(1,1), CASES(17,1), CASES(17,2), CASES(4096,1), CASES(4096,2)
#undef CASES
};
std::uint64_t now() noexcept {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
}
std::span<const Case> cases() noexcept {return catalog;}
struct Provider::State {
    const Case* selected{};
    p::Options options{};
    std::array<Correlation,record_capacity> records{};
    std::array<std::uint64_t,record_capacity> handles{};
    std::atomic<bool> invalid{};
    std::uint64_t calls{};
    std::size_t capacity{}, retained{};
    bool failed{}, settled{};
    std::unique_ptr<p::SimulatedDriver> driver;
    std::unique_ptr<p::Scenario> scenario;
    static void batch(void* owner,std::size_t lane,rt::DeviceCommandBatch& value) noexcept {
        auto& s=*static_cast<State*>(owner);
        const auto index=s.calls*p::lanes(s.options)+lane;
        if(index>=s.capacity || lane>=p::lanes(s.options) || value.signal_count!=1) {
            s.invalid=true; return;
        }
        auto& r=s.records[index];
        if(r.id!=0) {s.invalid=true;return;}
        r={index+1,s.calls,lane,value.signals[0].value,value.command_count,now(),0};
        s.handles[index]=value.signals[0].timeline_handle;
    }
};
Provider::Provider():state_(std::make_unique<State>()) {}
Provider::~Provider() {if(finish()!=b::Status::ok) std::terminate();}
b::ProviderV1 Provider::table() noexcept {
    b::ProviderV1 table;table.id="rtfw.cuda-physics";table.case_count=std::size(catalog);
    table.user=this;table.describe=describe;table.invoke=invoke;return table;
}
b::Status Provider::describe(void*,std::size_t index,b::Descriptor& out) {
    if(index>=std::size(catalog)) return b::Status::not_found;
    const auto& c=catalog[index];out={};out.case_id=c.id;out.subsystem="cuda-physics";
    out.implementation="runtime-simulated-cuda-particle-v1";
    out.configuration="step-including-copies-oracle-host-correlation";
    out.workload_kind=c.active?"active-rate-particle-step":"frame-particle-step";
    out.workload_sha256=b::sha256(std::string("particle-v1-seed1-seven-steps-")+c.id);
    out.parameters={{"entities",c.entities,c.entities,c.entities},{"workers",c.workers,c.workers,c.workers},
        {"graph",c.graph,0,1},{"active",c.active,0,1},{"seed",1,1,1}};
    const std::uint64_t n=c.entities, lanes=n==1?1:2, bytes=n*sizeof(Particle);
    out.counters={{"entities","count",n,n},{"upload_bytes","bytes",2*bytes,2*bytes},
        {"copy_bytes","bytes",bytes,bytes},{"download_bytes","bytes",bytes,bytes},
        {"kernels","count",c.graph?0:lanes,c.graph?0:lanes},
        {"graphs","count",c.graph?lanes:0,c.graph?lanes:0},{"publications","count",lanes,lanes},
        {"correlations","records",lanes,lanes},{"commands","count",5*lanes,5*lanes}};
    return b::Status::ok;
}
b::Status Provider::prepare(std::string_view id,std::size_t capacity) {
    auto& s=*state_;if(s.scenario) return b::Status::busy;
    const auto found=std::find_if(std::begin(catalog),std::end(catalog),[&](const Case& c){return id==c.id;});
    if(found==std::end(catalog)) return b::Status::not_found;
    const auto lanes=found->entities==1?1u:2u;
    if(capacity<invocation_count*lanes || capacity>record_capacity) return b::Status::capacity;
    s.selected=found;s.options={{found->entities,invocation_count,1,found->workers},found->graph,found->active};
    s.records={};s.handles={};s.invalid=false;s.calls=0;s.capacity=capacity;s.retained=0;s.failed=false;s.settled=false;
    s.driver=std::make_unique<p::SimulatedDriver>(s.options);
    p::Instrumentation hooks;hooks.owner=&s;hooks.batch=State::batch;
    s.scenario=std::make_unique<p::Scenario>(s.options,s.driver->session(),hooks);
    if(s.scenario->prepare()!=rt::Status::ok || s.scenario->active_plan()!=s.options.active ||
       s.scenario->start()!=rt::Status::ok) {s.failed=true;return b::Status::provider_error;}
    return b::Status::ok;
}
b::Status Provider::invoke(void* user,std::string_view id,std::uint64_t ordinal,b::Observation& out) {
    auto& s=*static_cast<Provider*>(user)->state_;
    if(!s.scenario || !s.selected || id!=s.selected->id || s.failed || ordinal!=s.calls || ordinal>=invocation_count)
        return b::Status::invalid;
    s.failed=true;out.correct=false;
    const auto& d=*s.driver;
    const auto up=d.upload_bytes.load(),copy=d.copy_bytes.load(),down=d.download_bytes.load();
    const auto kernels=d.kernels.load(),graphs=d.graphs.load();
    if(s.scenario->step()!=rt::Status::ok || s.invalid.load()) return b::Status::invariant_failed;
    const auto end=now();const auto lanes=p::lanes(s.options);
    for(std::size_t lane=0;lane<lanes;++lane) {
        const auto index=ordinal*lanes+lane;auto& r=s.records[index];rt::DeviceTimelineInfo info;
        if(!s.scenario->timeline(lane,info) || info.timeline.value!=s.handles[index] ||
            info.completed_value!=ordinal+1 || info.last_accepted_value!=ordinal+1 ||
            r.id!=index+1 || r.invocation!=ordinal || r.lane!=lane || r.timeline_value!=ordinal+1 ||
            r.commands!=5 || r.host_submit_ns>end || s.scenario->publications(lane)!=ordinal+1)
            return b::Status::invariant_failed;
        r.host_complete_ns=end;
    }
    if(d.stream_syncs!=0 || d.event_syncs!=0 || d.backend_allocations!=0 || !d.protocol_ok.load())
        return b::Status::invariant_failed;
    // Exact oracle is checked by every dependent CPU callback. This stable token
    // additionally lets consumers compare identical outputs across dispatch modes.
    std::uint64_t checksum=0;
    for(std::size_t i=0;i<s.options.count;++i) {
        const auto& particle=s.scenario->particle(i);
        for(unsigned axis=0;axis<3;++axis) for(auto v:{particle.position[axis],particle.velocity[axis],particle.acceleration[axis]})
            checksum=(checksum*131+static_cast<std::uint32_t>(v))&b::max_integer;
    }
    out.counters={s.options.count,d.upload_bytes.load()-up,d.copy_bytes.load()-copy,d.download_bytes.load()-down,
        d.kernels.load()-kernels,d.graphs.load()-graphs,lanes,lanes,5*lanes};
    out.checksum=checksum;out.correct=true;++s.calls;s.retained=static_cast<std::size_t>(s.calls*lanes);s.failed=false;
    return b::Status::ok;
}
b::Status Provider::finish() noexcept {
    auto& s=*state_;if(!s.scenario) return b::Status::ok;
    s.failed=true;
    if(s.scenario->stop()!=rt::Status::ok) return b::Status::provider_error;
    if(!s.driver->clean() || s.driver->registrations!=s.driver->unregistrations.load() ||
        s.driver->event_creates!=s.driver->event_destroys.load() || s.driver->backend_allocations!=0 ||
        s.driver->backend_frees!=0 || !s.driver->close()) return b::Status::invariant_failed;
    s.scenario.reset();s.driver.reset();s.settled=true;return b::Status::ok;
}
std::span<const Correlation> Provider::correlations() const noexcept {
    const auto& s=*state_;return s.settled?std::span(s.records.data(),s.retained):std::span<const Correlation>{};
}
void Provider::corrupt_next_output() noexcept {if(state_->driver) state_->driver->corrupt_output=true;}
}
