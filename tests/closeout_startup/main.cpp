// Ordered public-API controls; the original fixture and its 8ms/2s bounds stay intact.
#define RTFW_SAMPLED_SIMULATION_NO_MAIN
#include "../package_consumer/sampled_io_simulation_consumer.cpp"
#include "../golden_cuda/allocation.hpp"
#include <cstdlib>

namespace {
using rt::Status;
using Fixture = sampled_simulation::Fixture;
namespace allocation = rtfw_physics_allocation;
void require(bool good, const char* what) {
    if (!good) { std::fprintf(stderr, "FAIL %s\n", what); std::exit(1); }
}
void ok(Status status) { require(status == Status::ok, "setup or checked cleanup"); }
template<class Predicate> bool await(Predicate ready) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!ready()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::yield();
    }
    return true;
}
struct Driver {
    Fixture& f;
    rt::XdmaDriverApi original;
    std::atomic<bool> release{false}, entered{false}, expired{false}, terminal{false};
    std::atomic<unsigned> stop_calls{0};
    bool held = true, original_two_seconds = false;
    explicit Driver(Fixture& fixture) : f(fixture), original(f.backend.card.api()) {}
    static Driver& self(void* p) { return *static_cast<Driver*>(p); }
    rt::XdmaDriverApi api() {
        auto a = original; a.user_data = this;
        a.initialize = [](void* p) noexcept { auto& s=self(p); return s.original.initialize(s.original.user_data); };
        a.shutdown = [](void* p) noexcept { auto& s=self(p); return s.original.shutdown(s.original.user_data); };
        a.reset = [](void* p) noexcept { auto& s=self(p); return s.original.reset(s.original.user_data); };
        a.monotonic_time_ns = [](void* p) noexcept { auto& s=self(p); return s.original.monotonic_time_ns(s.original.user_data); };
        a.transfer = [](void* p, rt::XdmaDirection d, std::uint32_t ch, std::uint64_t off, void* host, std::uint64_t bytes) noexcept {
            auto& s=self(p); return s.original.transfer(s.original.user_data,d,ch,off,host,bytes);
        };
        a.control_read32 = [](void* p,std::uint32_t off) noexcept { auto& s=self(p); return s.original.control_read32(s.original.user_data,off); };
        a.control_write32 = [](void* p,std::uint32_t off,std::uint32_t value) noexcept { auto& s=self(p); return s.original.control_write32(s.original.user_data,off,value); };
        a.request_stop = [](void* p) noexcept {
            auto& s=self(p); ++s.stop_calls;
            // A request is not a promise that borrowed native work has ended.
            return rt::XdmaDriverResult::success;
        };
        a.wait_user_event = [](void* p,std::uint32_t index,std::uint64_t timeout) noexcept {
            auto& s=self(p); s.f.backend.card.event_timeout=timeout; s.entered=true;
            if (s.held && !s.original_two_seconds && !await([&] { return s.release.load(); })) {
                s.expired=true;
                return rt::XdmaUserEventResult{rt::XdmaDriverResult::timeout,0};
            }
            return s.original.wait_user_event(s.original.user_data,index,timeout);
        };
        return a;
    }
    void install() {
        rt::XdmaBackendConfig c;
        c.queue_capacity=4; c.buffer_capacity=1; c.worker_count=1;
        c.h2c_channel_count=c.c2h_channel_count=1;
        c.max_buffer_bytes=4096; c.max_transfer_bytes=128;
        c.transfer_alignment=1; c.control_aperture_bytes=4; c.user_event_count=1;
        f.backend.native=std::make_unique<rt::XdmaDeviceBackend>(api(),c);
    }
};
struct Commands {
    Driver& driver;
    rt::HalV2CommandTimelineExtension original{};
    std::atomic<unsigned> attempts{0};
    std::atomic<bool> entered{false}, release{false}, expired{false};
    std::atomic<rt::HalV2Status> returned{rt::HalV2Status::internal_error};
    bool hold=false;
    explicit Commands(Driver& d) : driver(d) {}
    static Commands& self(void* p) { return *static_cast<Commands*>(p); }
    void install() {
        auto& f=driver.f; original=f.backend.original_commands;
        auto e=original; e.instance=this;
        e.get_capabilities=[](void* p,rt::HalV2CommandTimelineCapabilities* out) { auto& s=self(p); return s.original.get_capabilities(s.original.instance,out); };
        e.submit=[](void* p,const rt::DeviceCommandBatch* batch) {
            auto& s=self(p); ++s.attempts; s.entered=true;
            if (s.hold && !await([&] { return s.release.load(); })) s.expired=true;
            const auto result=s.original.submit(s.original.instance,batch); s.returned=result; return result;
        };
        e.poll=[](void* p,rt::HalV2BatchCompletion* out,std::uint64_t capacity,std::uint64_t* count) {
            auto& s=self(p); auto result=s.original.poll(s.original.instance,out,capacity,count);
            if (*count) s.driver.terminal=true;
            return result;
        };
        e.cancel=[](void* p,std::uint64_t id) { auto& s=self(p); return s.original.cancel(s.original.instance,id); };
        e.request_stop=[](void* p) { auto& s=self(p); const auto result=s.original.request_stop(s.original.instance); s.release=true; return result; };
        f.backend.original_commands=e;
    }
};
void setup(Fixture& f, Driver& d, Commands& c, bool native=false, bool simulation=true) {
    if (!simulation) f.simulation.reset();
    f.backend.deterministic=!native;
    d.install(); ok(f.configure()); c.install(); ok(f.runtime.finalize());
}
void channel(Fixture& f, Status expected, rt::SampledIoSafetyState safety) {
    rt::SampledIoChannelStatus s;
    require(f.runtime.sampled_io_channel_status(f.output,s),"channel exists");
    require(s.last_status==expected && s.safety_state==safety,"terminal channel status/safety");
}
void clean(Fixture& f, Driver& d, bool measured=true) {
    d.release=true; f.backend.card.hold=false;
    if (measured) allocation::begin();
    Status status=Status::invalid_state;
    const bool ended=await([&] { status=f.runtime.stop(); return status==Status::ok; });
    const auto again=f.runtime.stop();
    const auto n=measured?allocation::end():0;
    require(ended && again==Status::ok && n==0,"checked stop/repeated stop/allocation");
    require(f.runtime.state()==rt::RuntimeState::stopped && !f.backend.card.live,"final stopped ownership");
    require(f.backend.card.shutdowns==1,"exact native shutdown count");
    rt::RuntimeMetricSnapshot snapshot;
    ok(f.runtime.metrics_snapshot(rt::RuntimeMetricWindow::cumulative,nullptr,snapshot));
    require(snapshot.samples[static_cast<std::size_t>(rt::RuntimeMetricId::device_outstanding)].value==0,
            "zero Runtime outstanding work after successful checked stop");
}
void success(bool measured=true);
void pending(bool native, bool simulator, bool measured=true, bool peer=false) {
    Fixture f; Driver d(f); Commands c(d);
    if (simulator) f.simulation=rt::DeviceRateSimulationPolicy{100'000'000};
    setup(f,d,c,native,simulator);
    const auto original_state=f.state;
    std::atomic<bool> observed{false};
    std::thread clock_owner([&] {
        observed=await([&] { return d.entered.load(); });
        // Host watchdog expires outstanding work; logical time stays frozen.
    });
    const auto start=f.runtime.start(); clock_owner.join();
    const auto first=f.runtime.stop(), second=f.runtime.stop();
    const auto restart=f.runtime.start(), step=f.step(0);
    const bool retained=f.backend.card.live;
    channel(f,Status::device_timeout,rt::SampledIoSafetyState::unknown);
    const bool unpublished=f.state==original_state && f.backend.card.acks==0 && f.backend.card.downloads==0;
    if (peer) {
        success(false);
        require(f.backend.card.live && !d.release,"another owner cannot release pending work");
        channel(f,Status::device_timeout,rt::SampledIoSafetyState::unknown);
    }
    clean(f,d,measured);
    require(observed && !d.expired && !c.expired,"ordered native entry/hold");
    require(start==Status::invalid_state && first==Status::invalid_state && second==Status::invalid_state,"pending cleanup status precedence");
    require(restart==Status::invalid_state && step==Status::invalid_state && retained && unpublished,"pending ownership and execution refusal");
    require(f.backend.card.event_timeout==8'000'000,"original native 8ms argument");
    require(f.state==original_state,"late completion was not application publication");
    std::printf("pending native=%d simulator=%d start=%d stop=%d repeated=%d cleanup=0 live=0\n",native,simulator,int(start),int(first),int(second));
}
void settled_timeout() {
    Fixture f; Driver d(f); Commands c(d); d.held=false;
    f.backend.card.missing_ack=true; setup(f,d,c);
    const auto start=f.runtime.start();
    channel(f,Status::device_timeout,rt::SampledIoSafetyState::unknown);
    require(d.entered && d.terminal && !d.expired,"returned native timeout completion");
    require(start==Status::device_timeout && !f.backend.card.live,"completed cleanup preserves original timeout");
    clean(f,d);
    require(f.backend.card.acks==0 && f.backend.card.downloads==0,"missing ACK never copied");
    std::printf("settled start=%d channel=-18 cleanup=0 live=0\n",int(start));
}

void logical_expiry() {
    Fixture f; Driver d(f); Commands c(d); setup(f,d,c);
    const auto initial=f.state;
    std::atomic<bool> observed{false};
    std::thread clock_owner([&] {
        observed=await([&] { return d.entered.load(); });
        if (observed) f.clock.now=8'001'001;
        d.release=true;
    });
    const auto start=f.runtime.start(); clock_owner.join();
    channel(f,Status::device_timeout,rt::SampledIoSafetyState::unknown);
    rt::DeviceTimelineInfo timeline;
    require(f.runtime.device_timeline_at(f.device,0,timeline),"timeline still inspectable");
    require(timeline.completed_value==0,"late native success cannot publish Runtime timeline");
    clean(f,d);
    require(observed && !d.expired && start==Status::device_timeout,"logical expiry on returned completion");
    require(f.backend.card.acks==1 && f.backend.card.downloads==1 && f.state==initial,
            "native readback is not application publication after logical expiry");
    std::puts("logical expiry: native ACK/readback=1, application/timeline publication=0, cleanup=0");
}
void failed_shutdown() {
    Fixture f; Driver d(f); Commands c(d); d.held=false;
    f.backend.card.missing_ack=true; f.backend.card.fail_shutdown=true;
    setup(f,d,c); const auto start=f.runtime.start();
    channel(f,Status::device_timeout,rt::SampledIoSafetyState::unknown);
    require(f.backend.card.live && f.backend.card.shutdowns==0,"failed shutdown retains borrowed owner");
    clean(f,d);
    require(start==Status::device_error,"cleanup error takes precedence over original timeout");
    std::puts("shutdown error: original channel=-18, startup=-19, retry releases ownership");
}

void before_acceptance() {
    Fixture f; Driver d(f); Commands c(d); c.hold=true;
    f.simulation=rt::DeviceRateSimulationPolicy{100'000'000};
    setup(f,d,c);
    std::atomic<bool> observed{false};
    std::thread clock_owner([&] { observed=await([&] { return c.entered.load(); }); });
    const auto start=f.runtime.start(); clock_owner.join();
    channel(f,Status::device_timeout,rt::SampledIoSafetyState::unknown);
    clean(f,d);

    require(observed && !c.expired && c.attempts==1,"one ordered submission attempt");
    require(start==Status::device_timeout && c.returned!=rt::HalV2Status::ok,"no accepted work after stop request");
    require(!d.entered && f.backend.card.acks==0 && f.backend.card.downloads==0,"no native event or invalid output");
    std::puts("before acceptance start=-18 accepted=0 cleanup=0 live=0");
}
void success(bool measured) {
    Fixture f; Driver d(f); Commands c(d); d.held=false;
    setup(f,d,c); ok(f.runtime.start());
    channel(f,Status::ok,rt::SampledIoSafetyState::startup_acknowledged);
    require(f.backend.card.input==f.safe && f.backend.card.event_timeout==8'000'000,"exact startup-safe frame and native deadline");
    if (measured) allocation::begin();
    const auto status=f.step(0);
    const auto n=measured?allocation::end():0;
    ok(status); require(n==0 && f.valid(0),"valid regular publication and no frame allocation");
    clean(f,d,measured); channel(f,Status::ok,rt::SampledIoSafetyState::shutdown_acknowledged);
    std::puts("success startup/regular/shutdown acknowledged live=0");
}
void two_seconds() {
    Fixture f; Driver d(f); Commands c(d); d.original_two_seconds=true;
    f.backend.card.hold=true; setup(f,d,c);
    const auto begin=std::chrono::steady_clock::now();
    const auto start=f.runtime.start();
    const auto elapsed=std::chrono::steady_clock::now()-begin;
    channel(f,Status::device_timeout,rt::SampledIoSafetyState::unknown); clean(f,d);
    require(start==Status::device_timeout && f.backend.card.event_timeout==8'000'000,"original driver wait timeout outcome");
    require(elapsed>=std::chrono::seconds(2) && f.clock.now==1000,"original two-second driver bound, frozen logical time");
    require(f.backend.card.acks==0 && f.backend.card.downloads==0,"driver timeout never acknowledged or copied");
    std::printf("original driver bound start=-18 elapsed-ms=%lld logical=1000 live=0; historical Windows cause unproven\n",static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()));
}
}
int main() {
    allocation::begin();
    void* positive=::operator new(32);
    const auto counted=allocation::end();
    ::operator delete(positive);
    require(counted!=0,"allocation observer positive control");
    success(); settled_timeout(); logical_expiry(); failed_shutdown(); before_acceptance();
    pending(false,true); pending(false,false); pending(true,false);
    pending(false,true,true,true);
    two_seconds();
    std::thread a([] { pending(false,true,false); });
    std::thread b([] { success(false); });
    a.join(); b.join();
    std::puts("PASS ordered sampled startup/cleanup ownership controls");
}
