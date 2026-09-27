#pragma once
#include "../model.hpp"
#include <rt/cuda_backend.hpp>
#include <rt/runtime.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>

namespace rtfw::cuda_physics::pipeline {
struct Options : cuda_physics::Options { bool graph = true; bool active = false; };
inline constexpr std::uint64_t period_ns = 10'000'000'000ULL;
inline constexpr std::uint64_t completion_ns = 5'000'000'000ULL;
inline std::uint32_t lanes(const Options& o) noexcept { return o.count == 1 ? 1u : 2u; }
inline std::uint32_t count(const Options& o, std::size_t lane) noexcept {
    return o.count/2 + (lane == 0 ? o.count%2 : 0);
}
struct Session {
    rt::CudaDriverApi driver{};
    rt::CudaContext context{};
    std::array<rt::CudaStream,2> streams{};
    rt::CudaFunction function{};
    std::array<rt::CudaGraphExec,2> graphs{};
    std::array<rt::CudaDeviceAddress,4> buffers{};
    std::array<std::uint64_t,4> bytes{};
};
// Optional configuring-time instrumentation for the installed conformance kit.
// Copied callbacks borrow their owner through checked stop; defaults do nothing.
struct Instrumentation {
    void* owner{};
    rt::HalV2BackendRegistration (*registration)(void*,std::size_t,rt::HalV2BackendRegistration) noexcept{};
    void (*batch)(void*,std::size_t,rt::DeviceCommandBatch&) noexcept{};
    std::size_t outstanding_capacity=2;
};
// Caller-owned session resources must outlive checked stop and this owner.
class Scenario {
    struct Lane {
        Scenario* owner{};
        std::size_t index{};
        std::uint32_t size{};
        std::array<Particle,max_particles/2> initial{}, staging{}, output{};
        rt::DeviceCommandBatch batch{};
        rt::DeviceTimelineHandle timeline{};
        std::atomic<std::uint64_t> prepared{}, submitted{}, published{};
    };
    static constexpr std::array<std::string_view,4> names{
        "pipeline.0.stage","pipeline.0.work","pipeline.1.stage","pipeline.1.work"};
    Options options_;
    Session session_;
    Instrumentation instrumentation_;
    std::array<Lane,2> lanes_{};
    std::array<std::unique_ptr<rt::CudaDeviceBackend>,2> backends_{};
    rt::Runtime runtime_;
    std::array<rt::DeviceBackendHandle,2> backend_handles_{};
    std::uint64_t completed_{},epoch_{};
    bool failed_{}, reset_{};
    rt::CudaBackendConfig config(std::size_t i) const noexcept {
        rt::CudaBackendConfig c;
        c.context=session_.context;
        c.streams=std::span(session_.streams.data()+i,1);
        c.queue_capacity=2; c.buffer_capacity=2; c.kernel_capacity=1;
        c.allocate_device_mirrors=false;
        return c;
    }
    bool valid_session() const noexcept {
        if (!options_.valid() || !session_.context || !session_.function) return false;
        for (std::size_t i=0;i<lanes(options_);++i) {
            if (!session_.streams[i] || (options_.graph && !session_.graphs[i])) return false;
            if (i && session_.streams[0]==session_.streams[1]) return false;
            for (std::size_t j=2*i;j<2*i+2;++j) {
                if (!session_.buffers[j] || session_.bytes[j] != count(options_,i)*sizeof(Particle)) return false;
                if (session_.buffers[j] > UINT64_MAX-session_.bytes[j]) return false;
                for (std::size_t k=0;k<j;++k)
                    if (session_.buffers[j] < session_.buffers[k]+session_.bytes[k] &&
                        session_.buffers[k] < session_.buffers[j]+session_.bytes[j]) return false;
            }
        }
        return true;
    }
    template<class Context> static bool identity(Lane& lane,const Context& c) noexcept {
        const auto n=lane.owner->completed_;
        return c.frame.frame_index==n+1 && (lane.owner->options_.active ?
            c.rate_release && c.rate_release->domain_release_sequence==n &&
            c.rate_release->substep_ordinal==0 && c.rate_release->logical_release_ns==n*period_ns :
            c.rate_release==nullptr);
    }
    static rt::CallbackResult prepare_frame(void* p,const rt::CallbackContext& c) noexcept {
        auto& lane=*static_cast<Lane*>(p);
        if (!identity(lane,c) || lane.published.load()!=lane.owner->completed_) return rt::CallbackResult::error;
        std::copy_n(lane.output.begin(),lane.size,lane.staging.begin());
        std::fill_n(lane.output.begin(),lane.size,Particle{});
        ++lane.prepared; return rt::CallbackResult::ok;
    }
    static rt::CallbackResult provide(void* p,const rt::DeviceCallbackContext& c,rt::DeviceCommandBatch& b) noexcept {
        auto& lane=*static_cast<Lane*>(p);
        if (!identity(lane,c)) return rt::CallbackResult::error;
        b=lane.batch; b.timeout_ns=completion_ns;
        b.signals[0].value=lane.owner->completed_+1;
        if (lane.owner->instrumentation_.batch)
            lane.owner->instrumentation_.batch(lane.owner->instrumentation_.owner,lane.index,b);
        ++lane.submitted; return rt::CallbackResult::ok;
    }
    static rt::CallbackResult validate(void* p,const rt::CallbackContext& c) noexcept {
        auto& lane=*static_cast<Lane*>(p);
        if (!identity(lane,c)) return rt::CallbackResult::error;
        for (std::size_t i=0;i<lane.size;++i)
            if (!matches_oracle(lane.output[i],lane.initial[i],static_cast<std::uint32_t>(c.frame.frame_index)))
                return rt::CallbackResult::error;
        ++lane.published; return rt::CallbackResult::ok;
    }
public:
    Scenario(const Options& o,const Session& s,Instrumentation hooks={}): options_(o),session_(s),instrumentation_(hooks) {}
    Scenario(const Scenario&)=delete;
    Scenario& operator=(const Scenario&)=delete;
    ~Scenario() { if (stop()!=rt::Status::ok) std::terminate(); }
    rt::Status prepare() {
        if (!valid_session()) return rt::Status::invalid_argument;
        rt::RuntimeConfig c;
        c.worker_count=options_.workers; c.callback_capacity=6;
        c.device_backend_capacity=2; c.device_buffer_capacity=4;
        c.device_outstanding_capacity=instrumentation_.outstanding_capacity; c.device_completion_batch=instrumentation_.outstanding_capacity;
        c.memory_budget_bytes=128*1024*1024; c.scratch_bytes=4096;
        c.trace_capacity=128; c.executor_queue_capacity=16;
        c.task_scratch_bytes=256; c.task_scratch_slots=16;
        auto s=runtime_.configure(c);
        if (s!=rt::Status::ok) return s;
        std::array<std::uint64_t,2> kernels{};
        std::array<rt::DeviceMemoryDomainHandle,2> memories{};
        for (std::size_t i=0;i<lanes(options_);++i) {
            backends_[i]=std::make_unique<rt::CudaDeviceBackend>(session_.driver,config(i));
            auto& backend=*backends_[i];
            if (backend.register_kernel(session_.function,kernels[i])!=RTFW_DEVICE_STATUS_OK) return rt::Status::invalid_argument;
            for (std::size_t j=2*i;j<2*i+2;++j)
                if (backend.bind_device_buffer(names[j],session_.buffers[j],session_.bytes[j])!=RTFW_DEVICE_STATUS_OK)
                    return rt::Status::invalid_argument;
            if (options_.graph) {
                const std::array bindings{rt::CudaGraphBufferBinding{names[2*i+1],RTFW_DEVICE_ACCESS_READ_WRITE}};
                if (backend.register_graph(static_cast<std::uint16_t>(i+1),session_.graphs[i],bindings)!=RTFW_DEVICE_STATUS_OK)
                    return rt::Status::invalid_argument;
            }
            auto registration=backend.hal_v2_registration(names[2*i]);
            if (instrumentation_.registration)
                registration=instrumentation_.registration(instrumentation_.owner,i,registration);
            s=runtime_.register_device_backend(registration,backend_handles_[i]);
            if (s!=rt::Status::ok) return s;
            rt::HalV2MemoryDomain descriptor;
            if (!runtime_.device_memory_domain_at(backend_handles_[i],1,memories[i],descriptor)) return rt::Status::device_error;
        }
        rt::RateDomainHandle rate;
        if (options_.active) {
            s=runtime_.set_rate_execution_policy({16,2402,1,1,128});
            if (s!=rt::Status::ok) return s;
            s=runtime_.register_rate_domain({"pipeline.rate",period_ns,1,period_ns,1'000'000'000ULL},rate);
            if (s!=rt::Status::ok) return s;
        }
        auto seed=options_.seed;
        for (std::size_t i=0;i<lanes(options_);++i) {
            auto& lane=lanes_[i]; lane.owner=this; lane.index=i; lane.size=count(options_,i);
            for (std::size_t n=0;n<lane.size;++n) lane.initial[n]=lane.output[n]=initial_particle(seed);
            std::array<rt::HalV2BufferReference,2> refs{};
            for (std::size_t j=0;j<2;++j) {
                auto bytes=std::as_writable_bytes(std::span(j==0?lane.staging.data():lane.output.data(),lane.size));
                rt::DeviceBufferHandle buffer;
                s=runtime_.register_device_buffer({names[2*i+j],backend_handles_[i],memories[i],bytes,{},bytes.size(),
                    rt::HalV2MemoryOwnership::borrowed_host,RTFW_DEVICE_BUFFER_HOST_READ|RTFW_DEVICE_BUFFER_HOST_WRITE|
                    RTFW_DEVICE_BUFFER_DEVICE_READ|RTFW_DEVICE_BUFFER_DEVICE_WRITE,rt::HalV2MemoryCoherency::staged_copy,
                    rt::hal_v2_memory_sync_copy_to_device|rt::hal_v2_memory_sync_copy_from_device},buffer);
                if (s!=rt::Status::ok) return s;
                refs[j].buffer_token=buffer.value; refs[j].bytes=bytes.size();
            }
            s=runtime_.register_device_timeline({names[2*i],backend_handles_[i],0},lane.timeline);
            if (s!=rt::Status::ok) return s;
            auto& b=lane.batch; b.command_count=5; b.signal_count=1;
            b.signals[0].timeline_handle=lane.timeline.value;
            auto& up=b.commands[0]; up.kind=static_cast<std::uint32_t>(rt::HalV2CommandKind::copy);
            up.operation=static_cast<std::uint32_t>(rt::HalV2MemoryOperation::copy_to_device);
            up.source=up.destination=refs[0]; up.source.access=RTFW_DEVICE_ACCESS_READ; up.destination.access=RTFW_DEVICE_ACCESS_WRITE;
            auto& initialize=b.commands[1]; initialize=up;
            initialize.source=initialize.destination=refs[1];
            initialize.source.access=RTFW_DEVICE_ACCESS_READ; initialize.destination.access=RTFW_DEVICE_ACCESS_WRITE;
            auto& copy=b.commands[2]; copy.kind=static_cast<std::uint32_t>(rt::HalV2CommandKind::dispatch);
            copy.opcode=rt::cuda_device_opcode_copy_device_to_device; copy.buffer_count=2;
            copy.buffers[0]=refs[0]; copy.buffers[0].access=RTFW_DEVICE_ACCESS_READ;
            copy.buffers[1]=refs[1]; copy.buffers[1].access=RTFW_DEVICE_ACCESS_WRITE;
            auto& launch=b.commands[3]; launch.kind=static_cast<std::uint32_t>(rt::HalV2CommandKind::dispatch);
            launch.buffer_count=1; launch.buffers[0]=refs[1]; launch.buffers[0].access=RTFW_DEVICE_ACCESS_READ_WRITE;
            if (options_.graph) launch.opcode=rt::cuda_device_opcode_graph(static_cast<std::uint16_t>(i+1));
            else {
                launch.opcode=rt::cuda_device_opcode_launch_kernel;
                rt::CudaKernelLaunch args; args.kernel_token=kernels[i]; args.block_x=128; args.grid_x=(lane.size+127)/128;
                if (!rt::cuda_kernel_add_buffer_argument(args,0) || !rt::cuda_kernel_add_scalar_argument(args,lane.size)) return rt::Status::internal_error;
                launch.payload_size=sizeof(args); std::memcpy(launch.payload.data(),&args,sizeof(args));
            }
            auto& down=b.commands[4]; down=up;
            down.operation=static_cast<std::uint32_t>(rt::HalV2MemoryOperation::copy_from_device);
            down.source=down.destination=refs[1]; down.source.access=RTFW_DEVICE_ACCESS_READ; down.destination.access=RTFW_DEVICE_ACCESS_WRITE;
        }
        // Active dispatch follows the compiled phase order. Declare both
        // independent submissions before either dependent validation barrier.
        std::array<rt::PhaseHandle,2> prep{},device{},verify{};
        for (std::size_t i=0;i<lanes(options_);++i) {
            s=runtime_.register_callback({names[2*i],prepare_frame,&lanes_[i]},prep[i]); if (s!=rt::Status::ok) return s;
        }
        for (std::size_t i=0;i<lanes(options_);++i) {
            s=runtime_.register_device_batch_phase({names[2*i+1],backend_handles_[i],provide,&lanes_[i],lanes_[i].batch},device[i]);
            if (s!=rt::Status::ok) return s;
        }
        for (std::size_t i=0;i<lanes(options_);++i) {
            s=runtime_.register_callback({i==0?"pipeline.0.verify":"pipeline.1.verify",validate,&lanes_[i]},verify[i]); if (s!=rt::Status::ok) return s;
            s=runtime_.add_dependency(prep[i],device[i]); if (s!=rt::Status::ok) return s;
            s=runtime_.add_dependency(device[i],verify[i]); if (s!=rt::Status::ok) return s;
            rt::ResourceHandle resource;
            s=runtime_.register_resource(names[2*i],resource); if (s!=rt::Status::ok) return s;
            for (const auto phase:{prep[i],device[i],verify[i]}) {
                s=runtime_.declare_resource_access(phase,resource,rt::ResourceAccess::write); if (s!=rt::Status::ok) return s;
            }
            if (options_.active) {
                s=runtime_.bind_phase_to_rate_domain(prep[i],rate); if (s!=rt::Status::ok) return s;
                s=runtime_.bind_phase_to_rate_domain(verify[i],rate); if (s!=rt::Status::ok) return s;
                using Role=rt::DeviceRatePayloadRole;
                const std::array roles{Role::input,Role::output,Role::input,Role::output,Role::input,Role::output,Role::input_output,Role::input,Role::output};
                s=runtime_.bind_device_phase_to_rate_domain({device[i],rate,completion_ns,1,roles}); if (s!=rt::Status::ok) return s;
            }
        }
        return runtime_.finalize();
    }
    rt::Status start() noexcept {
        const auto s=runtime_.start();
        if (s==rt::Status::ok) epoch_=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
        return s;
    }
    rt::Status step() noexcept {
        if (failed_) return rt::Status::invalid_state;
        if (completed_>=options_.steps) return rt::Status::invalid_argument;
        const auto duration=options_.active?period_ns:3'906'250ULL;
        auto s=runtime_.step({completed_+1,std::chrono::nanoseconds(duration),std::nullopt,
            options_.active?std::optional<std::uint64_t>(epoch_+completed_*period_ns):std::nullopt});
        reset_=s==rt::Status::device_reset_required;
        if (s==rt::Status::ok) for (std::size_t i=0;i<lanes(options_);++i) {
            rt::DeviceTimelineInfo info;
            if (!runtime_.device_timeline_at(backend_handles_[i],0,info) || info.completed_value!=completed_+1 ||
                info.last_accepted_value!=completed_+1 || lanes_[i].published.load()!=completed_+1) s=rt::Status::internal_error;
        }
        failed_=s!=rt::Status::ok;
        if (!failed_) ++completed_;
        return s;
    }
    rt::Status run() noexcept { while (completed_<options_.steps) { const auto s=step(); if (s!=rt::Status::ok) return s; } return rt::Status::ok; }
    rt::Status stop() noexcept {
        if (reset_) {
            for (std::size_t i=0;i<lanes(options_);++i) {
                const auto s=runtime_.reset_device(backend_handles_[i]); if (s!=rt::Status::ok) return s;
            }
            reset_=false;
        }
        return runtime_.state()==rt::RuntimeState::configuring?rt::Status::ok:runtime_.stop();
    }
    // May request cancellation during a step. Join that step before stop()/reset
    // or reading ordinary state; no vendor preemption is promised.
    rt::Status request_stop() noexcept { return runtime_.stop(); }
    rt::Status reset(std::size_t i) noexcept { return runtime_.reset_device(backend_handles_.at(i)); }
    rt::Status health(std::size_t i,rt::DeviceHealth& value) noexcept {
        return runtime_.device_health(backend_handles_.at(i),value);
    }
    bool timeline(std::size_t i,rt::DeviceTimelineInfo& value) const noexcept {
        return runtime_.device_timeline_at(backend_handles_.at(i),0,value);
    }
    std::string_view error() const noexcept { return runtime_.last_error(); }
    std::uint64_t completed() const noexcept { return completed_; }
    std::uint64_t publications(std::size_t i) const noexcept { return lanes_[i].published.load(); }
    std::uint64_t preparations(std::size_t i) const noexcept { return lanes_[i].prepared.load(); }
    std::uint64_t submissions(std::size_t i) const noexcept { return lanes_[i].submitted.load(); }
    const Particle& particle(std::size_t i) const noexcept {
        const auto first=count(options_,0); return i<first?lanes_[0].output[i]:lanes_[1].output[i-first];
    }
    bool active_plan() const noexcept {
        if (!runtime_.rate_execution_enabled() || runtime_.device_rate_phase_count()!=lanes(options_)) return false;
        for (std::size_t i=0;i<lanes(options_);++i) {
            rt::CompiledDeviceRatePhase p;
            if (!runtime_.compiled_device_rate_phase_at(i,p) || p.maximum_in_flight!=1 || p.command_count!=5 || p.completion_budget_ns!=completion_ns) return false;
        }
        return true;
    }
};
static_assert(sizeof(Scenario)<32*1024*1024);
}
