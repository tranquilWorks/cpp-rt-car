// Public-only reproducer: nested active/live-control replay must retain the
// same admission accounting as the recorded frames, as well as application state.
#include <rt/runtime.hpp>
#include <array>
#include <atomic>
#include <iostream>
#include <cstdlib>
#include <limits>
#include <new>
#if defined(_MSC_VER)
#include <malloc.h>
#endif
#include <string_view>
#include <vector>

namespace {
struct Clock final:rt::RuntimeClock {
    std::atomic<std::uint64_t> now{1000};
    std::uint64_t now_ns() noexcept override{return now.load();}
};
struct Work {
    std::array<std::byte,8> state{};
    std::size_t calls{};
    static rt::CallbackResult callback(void* opaque,const rt::CallbackContext& ctx) {
        auto& w=*static_cast<Work*>(opaque);
        if(!ctx.live_control || ctx.live_control->records.size()!=1 ||
           ctx.live_control->records[0].payload[0]!=std::byte(ctx.frame.frame_index)) return rt::CallbackResult::error;
        w.state[0]=std::byte(std::to_integer<unsigned>(w.state[0])+static_cast<unsigned>(ctx.frame.frame_index));
        ++w.calls;return rt::CallbackResult::ok;
    }
};
}
namespace {
std::atomic<bool> allocation_tracking{false};
std::atomic<std::size_t> allocations{0};
void count_allocation() noexcept {
    if (allocation_tracking.load(std::memory_order_relaxed)) {
        allocations.fetch_add(1, std::memory_order_relaxed);
    }
}
void* allocate_bytes(std::size_t bytes) {
    count_allocation();
    if (void* p = std::malloc(bytes == 0 ? 1 : bytes))
        return p;
    throw std::bad_alloc();
}
void* allocate_aligned(std::size_t bytes, std::size_t alignment) {
    count_allocation();
    bytes = bytes == 0 ? alignment : bytes;
#if defined(_MSC_VER)
    if (void* p = _aligned_malloc(bytes, alignment))
        return p;
#else
    const auto remainder = bytes % alignment;
    if (remainder != 0) {
        if (bytes > std::numeric_limits<std::size_t>::max() - (alignment - remainder))
            throw std::bad_alloc();
        bytes += alignment - remainder;
    }
    if (void* p = std::aligned_alloc(alignment, bytes))
        return p;
#endif
    throw std::bad_alloc();
}
void release_aligned(void* p) noexcept {
#if defined(_MSC_VER)
    _aligned_free(p);
#else
    std::free(p);
#endif
}
} // namespace
void* operator new(std::size_t bytes) {
    return allocate_bytes(bytes);
}
void* operator new[](std::size_t bytes) {
    return allocate_bytes(bytes);
}
void operator delete(void* p) noexcept {
    std::free(p);
}
void operator delete[](void* p) noexcept {
    std::free(p);
}
void operator delete(void* p, std::size_t) noexcept {
    std::free(p);
}
void operator delete[](void* p, std::size_t) noexcept {
    std::free(p);
}
void* operator new(std::size_t bytes, std::align_val_t alignment) {
    return allocate_aligned(bytes, static_cast<std::size_t>(alignment));
}
void* operator new[](std::size_t bytes, std::align_val_t alignment) {
    return allocate_aligned(bytes, static_cast<std::size_t>(alignment));
}
void operator delete(void* p, std::align_val_t) noexcept {
    release_aligned(p);
}
void operator delete[](void* p, std::align_val_t) noexcept {
    release_aligned(p);
}
void operator delete(void* p, std::size_t, std::align_val_t) noexcept {
    release_aligned(p);
}
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept {
    release_aligned(p);
}

int main(int argc, char** argv) {
    // Explicit v2 opt-in retains replacement payloads and rejection ownership.
    // The default consumer continues to exercise unchanged format v1.
    const bool replaced = argc == 2 && std::string_view(argv[1]) == "--replaced";
    const bool rejected = argc == 2 && std::string_view(argv[1]) == "--rejected";
    if (argc > 2 || (argc == 2 && !replaced && !rejected)) return 2;
    Clock clock;Work work;rt::Runtime runtime(clock);
    auto check=[](rt::Status status){if(status!=rt::Status::ok) throw status;};
    try {
        rt::RuntimeConfig cfg;cfg.callback_capacity=1;cfg.worker_count=1;
        cfg.executor_queue_capacity=8;cfg.task_scratch_slots=8;cfg.trace_capacity=0;
        check(runtime.configure(cfg));check(runtime.set_rate_execution_policy({8,1,1,1,32}));
        check(runtime.set_mixed_rate_closure_policy({1,128,128,65536,8,rt::MixedRateOverflowPolicy::overwrite_committed,true,true,{}}));
        rt::LiveControlPolicy policy;policy.policy_identity=1;policy.mailbox_capacity=1;policy.producer_capacity=1;
        policy.record_capacity=8;policy.payload_bytes_per_record=8;policy.total_payload_storage_bytes=64;
        check(runtime.set_live_control_policy(policy));
        rt::LiveControlMailboxRegistration mailbox;mailbox.mailbox_identity=1;mailbox.record_capacity=8;mailbox.payload_bytes_per_record=8;
        check(runtime.register_live_control_mailbox(mailbox));
        rt::LiveControlProducerRegistration producer;producer.mailbox_identity=1;producer.producer_identity=1;
        check(runtime.register_live_control_producer(producer));
        rt::LiveControlClosurePolicy closure;closure.policy_identity=1;closure.action_capacity=128;
        closure.retained_generation_capacity=8;closure.retained_record_capacity=8;closure.retained_payload_bytes=64;
        closure.replay_record_capacity=128;closure.replay_max_bytes=65536;closure.replay_enabled=true;
        check(runtime.set_live_control_closure_policy(closure));
        if (replaced || rejected) {
            rt::LiveControlReplayRetentionPolicy retention;
            retention.policy_identity = 77;
            retention.admission_capacity = 16;
            retention.payload_capacity_bytes = 128;
            check(runtime.set_live_control_replay_retention_policy(retention));
        }
        rt::PhaseHandle phase;rt::RateDomainHandle domain;
        check(runtime.register_callback({"work",Work::callback,&work},phase));
        check(runtime.register_rate_domain({"rate",100,1,100,1},domain));
        check(runtime.bind_phase_to_rate_domain(phase,domain));check(runtime.register_state({"state",1,work.state}));
        check(runtime.finalize());check(runtime.start());
        std::size_t size=0;check(runtime.checkpoint_size(size));std::vector<std::byte> checkpoint(size);
        rt::ArtifactWriteResult written;check(runtime.write_checkpoint(0,checkpoint,written));
        rt::LiveControlProducerHandle handle;check(runtime.live_control_producer_handle(1,1,handle));
        std::array<rt::ReplayInputRecord,2> inputs{};
        allocation_tracking.store(true, std::memory_order_release);
        for(std::uint64_t i=1;i<=2;++i) {
            std::array<std::byte,8> payload{};payload[0]=std::byte(i);
            rt::LiveControlUpdateRecord update;update.runtime_id=handle.runtime_id;update.configuration_generation=handle.configuration_generation;
            update.mailbox_identity=1;update.producer_identity=1;update.producer_sequence=i;update.target_frame_index=i;
            update.payload_bytes=8;update.payload_digest=rt::live_control_payload_digest(payload);
            rt::LiveControlAdmissionResult result;
            if (replaced) {
                payload[0]=std::byte(10+i);
                update.producer_sequence=2*i-1;
                update.payload_digest=rt::live_control_payload_digest(payload);
                check(runtime.stage_live_control_update(handle,update,payload,result));
                if(result!=rt::LiveControlAdmissionResult::accepted) throw rt::Status::internal_error;
                payload[0]=std::byte(i);
                update.producer_sequence=2*i;
                update.payload_digest=rt::live_control_payload_digest(payload);
            }
            if (rejected) {
                update.payload_digest ^= 1;
                check(runtime.stage_live_control_update(handle,update,payload,result));
                if(result!=rt::LiveControlAdmissionResult::invalid) throw rt::Status::internal_error;
                update.payload_digest ^= 1;
            }
            check(runtime.stage_live_control_update(handle,update,payload,result));
            if(result!=rt::LiveControlAdmissionResult::accepted) throw rt::Status::internal_error;
            inputs[i-1]={{i,std::chrono::nanoseconds{100},std::nullopt,1000+(i-1)*100},1,{}};
            check(runtime.step(inputs[i-1].frame));
        }
        allocation_tracking.store(false, std::memory_order_release);
        const auto run_allocations = allocations.load();
        std::vector<std::byte> active(65536),live(65536);
        check(runtime.write_active_replay_artifact(checkpoint,inputs,active,written));active.resize(written.bytes_written);
        check(runtime.write_live_control_replay_artifact(checkpoint,active,rt::LiveControlNestedArtifactKind::active_replay,live,written));live.resize(written.bytes_written);
        rt::LiveControlMailboxInfo before,after;
        if(!runtime.live_control_mailbox_info(1,before)) throw rt::Status::internal_error;
        work.calls=0;rt::LiveControlReplayResult result;
        allocation_tracking.store(true, std::memory_order_release);
        const auto status=runtime.replay_live_control(live,[](void*,const rt::ReplayInputView&){return rt::CallbackResult::ok;},nullptr,&result);
        allocation_tracking.store(false, std::memory_order_release);
        const auto total_allocations = allocations.load();
        if(!runtime.live_control_mailbox_info(1,after)) throw rt::Status::internal_error;
        std::cout << "status=" << static_cast<int>(status)
                  << " application_state=" << std::to_integer<unsigned>(work.state[0])
                  << " callbacks=" << work.calls
                  << " generations=" << result.generations_compared
                  << " accepted_before=" << before.accepted
                  << " accepted_after=" << after.accepted
                  << " run_allocations=" << run_allocations
                  << " total_allocations=" << total_allocations
                  << " invalid_before=" << before.invalid
                  << " invalid_after=" << after.invalid << '\n';
        check(runtime.stop());
        return status == rt::Status::ok && work.state[0] == std::byte{3} &&
                       work.calls == 2 && before.accepted == after.accepted &&
                       before.invalid == after.invalid && run_allocations == 0 &&
                       total_allocations == 0
                   ? 0
                   : 1;
    } catch(rt::Status status) {
        allocation_tracking.store(false, std::memory_order_release);
        std::cerr<<"setup status="<<static_cast<int>(status)<<'\n';
        if(runtime.state()!=rt::RuntimeState::configuring && runtime.state()!=rt::RuntimeState::stopped && runtime.stop()!=rt::Status::ok) std::terminate();
        return 2;
    }
}
