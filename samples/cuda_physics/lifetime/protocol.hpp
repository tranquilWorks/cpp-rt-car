#pragma once
#include "../pipeline/scenario.hpp"

namespace rtfw::cuda_physics::lifetime {
// Explicit HAL protocol fault adapter, not a simulated CUDA capability. All
// accepted work and normal completions still use the actual CUDA backend.
class Protocol {
    struct Lane {
        Protocol* owner{};
        rt::HalV2CommandTimelineExtension original{},wrapped{};
        rt::HalV2BatchCompletion retained{};
        bool history{};
    };
    std::array<Lane,2> lanes_{};
public:
    enum class Fault { none, queue_full, invalid_descriptor, stale_batch, wrong_signal };
    Fault fault=Fault::none; // Configure before start.
    std::atomic<std::uint64_t> injected{},submits{},polls{},cancels{},unsupported_cancels{},stops{};
    pipeline::Instrumentation hooks() noexcept {
        pipeline::Instrumentation hooks; hooks.owner=this; hooks.outstanding_capacity=2;
        hooks.registration=[](void* owner,std::size_t i,rt::HalV2BackendRegistration r) noexcept {
            auto& self=*static_cast<Protocol*>(owner); auto& lane=self.lanes_[i];
            lane.owner=&self; lane.original=*r.command_timeline; lane.wrapped=lane.original;
            auto& w=lane.wrapped; w.instance=&lane;
            w.get_capabilities=[](void* p,rt::HalV2CommandTimelineCapabilities* c) {
                auto& l=*static_cast<Lane*>(p); return l.original.get_capabilities(l.original.instance,c);
            };
            w.submit=[](void* p,const rt::DeviceCommandBatch* b) {
                auto& l=*static_cast<Lane*>(p); auto& s=*l.owner; ++s.submits;
                if (s.fault==Fault::queue_full) { ++s.injected; return rt::HalV2Status::queue_full; }
                if (s.fault==Fault::invalid_descriptor) {
                    auto invalid=*b; invalid.command_count=rt::hal_v2_command_capacity+1;
                    ++s.injected; return l.original.submit(l.original.instance,&invalid);
                }
                return l.original.submit(l.original.instance,b);
            };
            w.poll=[](void* p,rt::HalV2BatchCompletion* c,std::uint64_t capacity,std::uint64_t* n) {
                auto& l=*static_cast<Lane*>(p); auto& s=*l.owner; ++s.polls;
                const auto status=l.original.poll(l.original.instance,c,capacity,n);
                if (status==rt::HalV2Status::ok && *n && s.injected.load()==0) {
                    if (s.fault==Fault::stale_batch) {
                        if (!l.history) { l.retained=c[0]; l.history=true; }
                        else { c[0]=l.retained; ++s.injected; }
                    } else if (s.fault==Fault::wrong_signal) {
                        ++c[0].signals[0].value; ++s.injected;
                    }
                }
                return status;
            };
            w.cancel=[](void* p,std::uint64_t id) {
                auto& l=*static_cast<Lane*>(p); auto& s=*l.owner; ++s.cancels;
                const auto status=l.original.cancel(l.original.instance,id);
                if (status==rt::HalV2Status::unsupported) ++s.unsupported_cancels;
                return status;
            };
            w.request_stop=[](void* p) {
                auto& l=*static_cast<Lane*>(p); ++l.owner->stops;
                return l.original.request_stop(l.original.instance);
            };
            r.command_timeline=&w; return r;
        };
        return hooks;
    }
};
}
