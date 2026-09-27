#pragma once
#include "scenario.hpp"
#include <algorithm>

namespace rtfw::cuda_physics::pipeline {
class SimulatedDriver {
    using R=rt::CudaDriverResult;
    static constexpr auto ok=R::success, bad=R::invalid_value;
    static constexpr rt::CudaContext context=0xca02;
    static constexpr rt::CudaFunction function=0xfa02;
    static constexpr std::array<rt::CudaStream,2> streams{0x5101,0x5102};
    Options options_;
    std::array<std::array<Particle,max_particles/2>,4> device_{};
    std::array<void*,4> host_{};
    std::array<std::uint64_t,4> bytes_{};
    std::array<bool,4> registered_{};
    std::array<unsigned,2> phase_{};
    std::array<int,2> stream_lane_{-1,-1};
    std::array<std::atomic<bool>,2> live_{};
    struct Event { std::atomic<bool> allocated{},recorded{}; int lane=-1; };
    std::array<Event,8> events_{};
    inline static thread_local const SimulatedDriver* current_{};
    bool closed_{};
    rt::CudaDeviceAddress address(std::size_t i) const noexcept {
        return static_cast<rt::CudaDeviceAddress>(reinterpret_cast<std::uintptr_t>(device_[i].data()));
    }
    int buffer(rt::CudaDeviceAddress a,std::uint64_t bytes) const noexcept {
        for (std::size_t i=0;i<2*lanes(options_);++i)
            if (a==address(i) && bytes==bytes_[i]) return static_cast<int>(i);
        return -1;
    }
    int stream(rt::CudaStream s) const noexcept {
        for (std::size_t i=0;i<lanes(options_);++i) if (s==streams[i]) return static_cast<int>(i);
        return -1;
    }
    Event* event(rt::CudaEvent e) noexcept {
        return e && e<=events_.size() && events_[e-1].allocated.load()?&events_[e-1]:nullptr;
    }
    bool current() noexcept {
        if (current_==this && !closed_) return true;
        protocol_ok=false; return false;
    }
    R integrate(std::size_t lane,rt::CudaStream st) noexcept {
        const auto index=stream(st);
        if (!current() || index<0 || stream_lane_[static_cast<std::size_t>(index)]!=static_cast<int>(lane) || phase_[lane]!=3) return bad;
        for (std::size_t i=0;i<count(options_,lane);++i) for (unsigned axis=0;axis<3;++axis) {
            auto& p=device_[2*lane+1][i]; p.velocity[axis]+=p.acceleration[axis]; p.position[axis]+=p.velocity[axis];
        }
        phase_[lane]=4; return ok;
    }
public:
    std::atomic<bool> hold{},fail_graph{},corrupt_d2d{},corrupt_output{},fail_unregister{},protocol_ok{true};
    std::atomic<std::uint64_t> uploads{},copies{},downloads{},kernels{},graphs{},upload_bytes{},copy_bytes{},download_bytes{},
        records{},not_ready{},registrations{},unregistrations{},backend_allocations{},backend_frees{},stream_mask{},stream_syncs{};
    explicit SimulatedDriver(const Options& o):options_(o) {
        for (std::size_t i=0;i<4;++i) bytes_[i]=count(o,i/2)*sizeof(Particle);
    }
    ~SimulatedDriver() { if (!close()) std::terminate(); }
    bool clean() const noexcept {
        for (const auto& e:events_) if (e.allocated.load()) return false;
        for (bool r:registered_) if (r) return false;
        for (const auto& v:live_) if (v.load()) return false;
        return true;
    }
    bool close() noexcept { if (!clean()) return false; closed_=true; return true; }
    Session session() noexcept {
        rt::CudaDriverApi a;
        a.api_version=rt::cuda_driver_api_version_2; a.struct_size=rt::cuda_driver_api_v2_struct_size; a.user_data=this;
        a.push_context=[](void* p,rt::CudaContext c) noexcept {
            auto* s=static_cast<SimulatedDriver*>(p);
            if (c!=context || current_ || s->closed_) return bad;
            current_=s; return ok;
        };
        a.pop_context=[](void* p,rt::CudaContext* c) noexcept {
            auto& s=*static_cast<SimulatedDriver*>(p);
            if (!c || !s.current()) return bad;
            *c=context; current_=nullptr; return ok;
        };
        a.event_create=[](void* p,rt::CudaEvent* out) noexcept {
            auto& s=*static_cast<SimulatedDriver*>(p); if (!s.current() || !out) return bad;
            for (std::size_t i=0;i<s.events_.size();++i) if (!s.events_[i].allocated.exchange(true)) {
                s.events_[i].recorded=false; *out=i+1; return ok;
            }
            return R::out_of_memory;
        };
        a.event_destroy=[](void* p,rt::CudaEvent e) noexcept {
            auto& s=*static_cast<SimulatedDriver*>(p); auto* v=s.event(e);
            if (!s.current() || !v) return bad;
            v->recorded=false; v->allocated=false; return ok;
        };
        a.event_record=[](void* p,rt::CudaEvent e,rt::CudaStream st) noexcept {
            auto& s=*static_cast<SimulatedDriver*>(p); auto* v=s.event(e); const auto stream=s.stream(st);
            if (!s.current() || !v || stream<0) return bad;
            const auto lane=s.stream_lane_[static_cast<std::size_t>(stream)];
            if (lane<0) return bad;
            v->lane=lane; s.live_[static_cast<std::size_t>(lane)]=true;
            v->recorded=true; ++s.records; return ok;
        };
        a.event_query=[](void* p,rt::CudaEvent e) noexcept {
            auto& s=*static_cast<SimulatedDriver*>(p); auto* v=s.event(e);
            if (!s.current() || !v || !v->recorded.load()) return bad;
            if (s.hold.load()) { ++s.not_ready; return R::not_ready; }
            s.live_[static_cast<std::size_t>(v->lane)]=false; return ok;
        };
        a.event_synchronize=[](void* p,rt::CudaEvent e) noexcept {
            auto& s=*static_cast<SimulatedDriver*>(p); auto* v=s.event(e);
            if (!s.current() || !v) return bad;
            if (v->recorded.load()) s.live_[static_cast<std::size_t>(v->lane)]=false;
            return ok;
        };
        a.stream_synchronize=[](void* p,rt::CudaStream st) noexcept {
            auto& s=*static_cast<SimulatedDriver*>(p); const auto index=s.stream(st);
            if (!s.current() || index<0) return bad;
            const auto lane=s.stream_lane_[static_cast<std::size_t>(index)];
            if (lane>=0) s.live_[static_cast<std::size_t>(lane)]=false;
            ++s.stream_syncs; return ok;
        };
        a.mem_alloc=[](void* p,std::uint64_t,rt::CudaDeviceAddress*) noexcept {
            ++static_cast<SimulatedDriver*>(p)->backend_allocations; return bad;
        };
        a.mem_free=[](void* p,rt::CudaDeviceAddress) noexcept {
            ++static_cast<SimulatedDriver*>(p)->backend_frees; return bad;
        };
        a.host_register=[](void* p,void* host,std::uint64_t bytes) noexcept {
            auto& s=*static_cast<SimulatedDriver*>(p); if (!s.current() || !host) return bad;
            for (std::size_t i=0;i<2*lanes(s.options_);++i) if (!s.registered_[i]) {
                if (bytes!=s.bytes_[i]) return bad;
                s.host_[i]=host; s.registered_[i]=true; ++s.registrations; return ok;
            }
            return bad;
        };
        a.host_unregister=[](void* p,void* host) noexcept {
            auto& s=*static_cast<SimulatedDriver*>(p); if (!s.current()) return bad;
            if (s.fail_unregister.exchange(false)) return R::error;
            for (std::size_t i=0;i<4;++i) if (s.registered_[i] && s.host_[i]==host) {
                s.registered_[i]=false; ++s.unregistrations; return ok;
            }
            return bad;
        };
        a.memcpy_host_to_device_async=[](void* p,rt::CudaDeviceAddress dst,const void* host,std::uint64_t bytes,rt::CudaStream st) noexcept {
            auto& s=*static_cast<SimulatedDriver*>(p); const auto b=s.buffer(dst,bytes), stream=s.stream(st);
            if (!s.current() || b<0 || stream<0) return bad;
            const auto i=static_cast<std::size_t>(b), lane=i/2;
            if (host!=s.host_[i] || !s.registered_[i] || s.phase_[lane]!=(i%2==0?0u:1u) || s.live_[lane].load()) return bad;
            s.stream_lane_[static_cast<std::size_t>(stream)]=static_cast<int>(lane);
            s.stream_mask.fetch_or(1ULL<<static_cast<unsigned>(stream));
            std::memcpy(s.device_[i].data(),host,static_cast<std::size_t>(bytes));
            s.phase_[lane]=i%2==0?1u:2u; ++s.uploads; s.upload_bytes+=bytes; return ok;
        };
        a.memcpy_device_to_device_async=[](void* p,rt::CudaDeviceAddress dst,rt::CudaDeviceAddress src,std::uint64_t bytes,rt::CudaStream st) noexcept {
            auto& s=*static_cast<SimulatedDriver*>(p); const auto from=s.buffer(src,bytes),to=s.buffer(dst,bytes),stream=s.stream(st);
            if (!s.current() || from<0 || from%2 || to!=from+1 || stream<0) return bad;
            const auto i=static_cast<std::size_t>(from),lane=i/2;
            if (s.phase_[lane]!=2 || s.stream_lane_[static_cast<std::size_t>(stream)]!=static_cast<int>(lane)) return bad;
            std::memcpy(s.device_[i+1].data(),s.device_[i].data(),static_cast<std::size_t>(bytes));
            if (s.corrupt_d2d.exchange(false)) ++s.device_[i+1][0].position[0];
            s.phase_[lane]=3; ++s.copies; s.copy_bytes+=bytes; return ok;
        };
        a.memcpy_device_to_host_async=[](void* p,void* host,rt::CudaDeviceAddress src,std::uint64_t bytes,rt::CudaStream st) noexcept {
            auto& s=*static_cast<SimulatedDriver*>(p); const auto b=s.buffer(src,bytes),stream=s.stream(st);
            if (!s.current() || b<0 || b%2!=1 || stream<0) return bad;
            const auto i=static_cast<std::size_t>(b),lane=i/2;
            if (host!=s.host_[i] || !s.registered_[i] || s.phase_[lane]!=4 || s.stream_lane_[static_cast<std::size_t>(stream)]!=static_cast<int>(lane)) return bad;
            std::memcpy(host,s.device_[i].data(),static_cast<std::size_t>(bytes));
            if (s.corrupt_output.exchange(false)) ++static_cast<Particle*>(host)[0].position[0];
            s.phase_[lane]=0; ++s.downloads; s.download_bytes+=bytes; return ok;
        };
        a.memset_d8_async=[](void*,rt::CudaDeviceAddress,std::uint8_t,std::uint64_t,rt::CudaStream) noexcept { return bad; };
        a.launch_kernel=[](void* p,rt::CudaFunction f,std::uint32_t gx,std::uint32_t gy,std::uint32_t gz,
            std::uint32_t bx,std::uint32_t by,std::uint32_t bz,std::uint32_t shared,rt::CudaStream st,void* const* args) noexcept {
            auto& s=*static_cast<SimulatedDriver*>(p);
            if (f!=function || !args || !args[0] || !args[1] || gy!=1 || gz!=1 || bx!=128 || by!=1 || bz!=1 || shared) return bad;
            rt::CudaDeviceAddress address{}; std::uint32_t n{};
            std::memcpy(&address,args[0],sizeof(address)); std::memcpy(&n,args[1],sizeof(n));
            const auto b=s.buffer(address,n*sizeof(Particle));
            if (b<0 || b%2!=1 || gx!=(n+127)/128) return bad;
            const auto result=s.integrate(static_cast<std::size_t>(b)/2,st);
            if (result==ok) ++s.kernels;
            return result;
        };
        a.graph_launch=[](void* p,rt::CudaGraphExec graph,rt::CudaStream st) noexcept {
            auto& s=*static_cast<SimulatedDriver*>(p);
            if (graph<0x7001 || graph>=0x7001+lanes(s.options_)) return bad;
            if (s.fail_graph.exchange(false)) return R::launch_failure;
            const auto result=s.integrate(static_cast<std::size_t>(graph-0x7001),st);
            if (result==ok) ++s.graphs;
            return result;
        };
        a.monotonic_time_ns=[](void*) noexcept ->std::uint64_t {
            return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        };
        Session result; result.driver=a; result.context=context; result.streams=streams; result.function=function;
        for (std::size_t i=0;i<lanes(options_);++i) result.graphs[i]=0x7001+i;
        for (std::size_t i=0;i<2*lanes(options_);++i) { result.buffers[i]=address(i); result.bytes[i]=bytes_[i]; }
        return result;
    }
};
static_assert(sizeof(SimulatedDriver)+sizeof(Scenario)<32*1024*1024);
}
