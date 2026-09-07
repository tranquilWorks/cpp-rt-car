#include "cpu_provider.hpp"
#include <rt/runtime.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <limits>
#include <new>
#include <stdexcept>

namespace rtfw::benchmark::cpu {
namespace {
constexpr Case catalog[]{
    {"compile-chain-32", "compile", Kind::compile, 32, 64, 16, 1, Policy::static_workers, true, false, 0, 512, 0, 64, 1, Memory::normal},
    {"compile-chain-4", "compile", Kind::compile, 4, 64, 16, 1, Policy::static_workers, true, false, 0, 512, 0, 64, 1, Memory::normal},
    {"compile-cycle-32", "compile", Kind::compile, 32, 64, 16, 1, Policy::static_workers, true, true, 0, 512, 0, 64, 1, Memory::normal},
    {"compile-cycle-4", "compile", Kind::compile, 4, 64, 16, 1, Policy::static_workers, true, true, 0, 512, 0, 64, 1, Memory::normal},
    {"compile-wide-32", "compile", Kind::compile, 32, 64, 16, 1, Policy::static_workers, false, false, 0, 512, 0, 64, 1, Memory::normal},
    {"compile-wide-4", "compile", Kind::compile, 4, 64, 16, 1, Policy::static_workers, false, false, 0, 512, 0, 64, 1, Memory::normal},
    {"depth-32", "graph_shape", Kind::graph, 32, 64, 16, 1, Policy::static_workers, true, false, 0, 512, 0, 64, 1, Memory::normal},
    {"depth-4", "graph_shape", Kind::graph, 4, 64, 16, 1, Policy::static_workers, true, false, 0, 512, 0, 64, 1, Memory::normal},
    {"empty-1", "empty_dispatch", Kind::graph, 1, 0, 16, 1, Policy::static_workers, false, false, 0, 512, 0, 64, 1, Memory::normal},
    {"empty-32", "empty_dispatch", Kind::graph, 32, 0, 16, 1, Policy::static_workers, false, false, 0, 512, 0, 64, 1, Memory::normal},
    {"entities-1024", "entities", Kind::range, 1, 1024, 64, 1, Policy::static_workers, false, false, 0, 512, 0, 64, 1, Memory::normal},
    {"entities-4096", "entities", Kind::range, 1, 4096, 64, 1, Policy::static_workers, false, false, 0, 512, 0, 64, 1, Memory::normal},
    {"entities-64", "entities", Kind::range, 1, 64, 64, 1, Policy::static_workers, false, false, 0, 512, 0, 64, 1, Memory::normal},
    {"grain-1", "granularity", Kind::range, 1, 128, 1, 1, Policy::static_workers, false, false, 0, 512, 0, 64, 1, Memory::normal},
    {"grain-16", "granularity", Kind::range, 1, 128, 16, 1, Policy::static_workers, false, false, 0, 512, 0, 64, 1, Memory::normal},
    {"grain-64", "granularity", Kind::range, 1, 128, 64, 1, Policy::static_workers, false, false, 0, 512, 0, 64, 1, Memory::normal},
    {"host-adapter-1024", "host_adapter", Kind::range, 1, 1024, 16, 1, Policy::host, false, false, 0, 512, 0, 64, 1, Memory::normal},
    {"host-adapter-64", "host_adapter", Kind::range, 1, 64, 64, 1, Policy::host, false, false, 0, 512, 0, 64, 1, Memory::normal},
    {"lifecycle-1", "lifecycle", Kind::lifecycle, 1, 64, 16, 1, Policy::static_workers, false, false, 0, 512, 0, 64, 1, Memory::normal},
    {"lifecycle-4", "lifecycle", Kind::lifecycle, 1, 64, 16, 1, Policy::static_workers, false, false, 0, 512, 0, 64, 4, Memory::normal},
    {"memory-acquire_failure-256", "memory", Kind::memory, 1, 64, 16, 1, Policy::static_workers, false, false, 0, 512, 256, 256, 1, Memory::acquire_failure},
    {"memory-acquire_failure-64", "memory", Kind::memory, 1, 64, 16, 1, Policy::static_workers, false, false, 0, 512, 256, 64, 1, Memory::acquire_failure},
    {"memory-apply_failure-256", "memory", Kind::memory, 1, 64, 16, 1, Policy::static_workers, false, false, 0, 512, 256, 256, 1, Memory::apply_failure},
    {"memory-apply_failure-64", "memory", Kind::memory, 1, 64, 16, 1, Policy::static_workers, false, false, 0, 512, 256, 64, 1, Memory::apply_failure},
    {"memory-inactive", "memory", Kind::memory, 1, 64, 16, 1, Policy::static_workers, false, false, 0, 512, 0, 0, 1, Memory::simulated},
    {"memory-normal-256", "memory", Kind::memory, 1, 64, 16, 1, Policy::static_workers, false, false, 0, 512, 256, 256, 1, Memory::normal},
    {"memory-normal-64", "memory", Kind::memory, 1, 64, 16, 1, Policy::static_workers, false, false, 0, 512, 256, 64, 1, Memory::normal},
    {"memory-rollback_retry-256", "memory", Kind::memory, 1, 64, 16, 1, Policy::static_workers, false, false, 0, 512, 256, 256, 1, Memory::rollback_retry},
    {"memory-rollback_retry-64", "memory", Kind::memory, 1, 64, 16, 1, Policy::static_workers, false, false, 0, 512, 256, 64, 1, Memory::rollback_retry},
    {"memory-simulated-256", "memory", Kind::memory, 1, 64, 16, 1, Policy::static_workers, false, false, 0, 512, 256, 256, 1, Memory::simulated},
    {"memory-simulated-64", "memory", Kind::memory, 1, 64, 16, 1, Policy::static_workers, false, false, 0, 512, 256, 64, 1, Memory::simulated},
    {"multiple-1024", "multiple", Kind::multiple, 1, 1024, 16, 1, Policy::static_workers, false, false, 0, 512, 0, 64, 1, Memory::normal},
    {"multiple-64", "multiple", Kind::multiple, 1, 64, 16, 1, Policy::static_workers, false, false, 0, 512, 0, 64, 1, Memory::normal},
    {"native-residency-4096", "residency", Kind::memory, 1, 64, 16, 1, Policy::static_workers, false, false, 0, 512, 0, 4096, 1, Memory::native},
    {"native-residency-8192", "residency", Kind::memory, 1, 64, 16, 1, Policy::static_workers, false, false, 0, 512, 0, 8192, 1, Memory::native},
    {"nested-for-1", "nested", Kind::nested_for, 1, 64, 16, 1, Policy::static_workers, false, false, 1, 512, 0, 64, 1, Memory::normal},
    {"nested-for-2", "nested", Kind::nested_for, 1, 64, 16, 1, Policy::static_workers, false, false, 2, 512, 0, 64, 1, Memory::normal},
    {"nested-reduce-1", "nested", Kind::nested_reduce, 1, 64, 16, 1, Policy::static_workers, false, false, 1, 512, 0, 64, 1, Memory::normal},
    {"nested-reduce-2", "nested", Kind::nested_reduce, 1, 64, 16, 1, Policy::static_workers, false, false, 2, 512, 0, 64, 1, Memory::normal},
    {"queue-2", "queue_pressure", Kind::pressure, 1, 16, 1, 1, Policy::host, false, false, 0, 2, 0, 64, 1, Memory::normal},
    {"queue-8", "queue_pressure", Kind::pressure, 1, 16, 1, 1, Policy::host, false, false, 0, 8, 0, 64, 1, Memory::normal},
    {"trace-0", "trace", Kind::graph, 4, 64, 16, 1, Policy::static_workers, false, false, 0, 512, 0, 64, 1, Memory::normal},
    {"trace-256", "trace", Kind::graph, 4, 64, 16, 1, Policy::static_workers, false, false, 0, 512, 256, 64, 1, Memory::normal},
    {"width-32", "graph_shape", Kind::graph, 32, 64, 16, 1, Policy::static_workers, false, false, 0, 512, 0, 64, 1, Memory::normal},
    {"width-4", "graph_shape", Kind::graph, 4, 64, 16, 1, Policy::static_workers, false, false, 0, 512, 0, 64, 1, Memory::normal},
    {"workers-static_workers-1", "workers", Kind::range, 1, 1024, 16, 1, Policy::static_workers, false, false, 0, 512, 0, 64, 1, Memory::normal},
    {"workers-static_workers-2", "workers", Kind::range, 1, 1024, 16, 2, Policy::static_workers, false, false, 0, 512, 0, 64, 1, Memory::normal},
    {"workers-static_workers-4", "workers", Kind::range, 1, 1024, 16, 4, Policy::static_workers, false, false, 0, 512, 0, 64, 1, Memory::normal},
    {"workers-throughput-1", "workers", Kind::range, 1, 1024, 16, 1, Policy::throughput, false, false, 0, 512, 0, 64, 1, Memory::normal},
    {"workers-throughput-2", "workers", Kind::range, 1, 1024, 16, 2, Policy::throughput, false, false, 0, 512, 0, 64, 1, Memory::normal},
    {"workers-throughput-4", "workers", Kind::range, 1, 1024, 16, 4, Policy::throughput, false, false, 0, 512, 0, 64, 1, Memory::normal},

};
constexpr std::size_t entity_limit = 4096, task_limit = 256;
constexpr std::uint64_t value(std::size_t i) noexcept {
    return static_cast<std::uint64_t>(i) * 3U + 1U;
}
std::uint64_t oracle(std::size_t n) noexcept {
    const auto count = static_cast<std::uint64_t>(n);
    return count == 0 ? 0 : count * (3U * count - 1U) / 2U;
}
bool lifecycle(const Case& c) noexcept {
    return c.kind == Kind::compile || c.kind == Kind::memory || c.kind == Kind::lifecycle;
}
bool deterministic_counters(const Case& c) noexcept { return c.policy == Policy::host; }

// Explicit caller-thread, bounded FIFO host fixture; never a claimed engine pool.
struct HostQueue {
    std::array<rt::HostExecutorJob, 512> jobs{};
    std::size_t head{}, count{}, capacity{512};
    static rt::Status submit(void* opaque, const rt::HostExecutorJob& job) noexcept {
        auto& q = *static_cast<HostQueue*>(opaque);
        if (q.count == q.capacity) return rt::Status::queue_full;
        q.jobs[(q.head + q.count) % q.jobs.size()] = job;
        ++q.count;
        return rt::Status::ok;
    }
    static bool help(void* opaque) noexcept {
        auto& q = *static_cast<HostQueue*>(opaque);
        if (q.count == 0) return false;
        const auto job = q.jobs[q.head];
        q.head = (q.head + 1) % q.jobs.size();
        --q.count;
        job.execute(job.execution_context, job.completion_context, job.completion_token, 0);
        return true;
    }
};

struct MemoryCounts { std::uint64_t acquired{}, applied{}, observed{}, rolled_back{}, released{}; };
struct SimulatedMemory {
    struct Slot {
        std::byte* bytes{};
        std::size_t size{};
        bool live{}, applied{};
        ~Slot() { if (bytes) ::operator delete(bytes, std::align_val_t{64}); }
    };
    std::array<Slot, 3> slots{};
    MemoryCounts counts{};
    Memory mode{Memory::simulated};
    bool retry_consumed{}, valid{true};
    rt::MemoryProvider table() noexcept {
        rt::MemoryProvider t;
        t.capabilities = rt::memory_provider_capability_bit(rt::MemoryProviderCapability::policy_operations) |
            rt::memory_provider_capability_bit(rt::MemoryProviderCapability::independent_observation);
        t.user_data = this; t.acquire = acquire; t.apply = apply; t.observe = observe;
        t.rollback = rollback; t.release = release;
        return t;
    }
    static rt::Status acquire(void* opaque, const rt::MemoryProviderAcquireRequest& req,
                              rt::MemoryProviderAllocation& out) noexcept {
        auto& self = *static_cast<SimulatedMemory*>(opaque);
        if (req.region.value < 4 || req.region.value > 6 || req.required_alignment > 64 ||
            req.logical_bytes > 256U * 1024U || req.page_rounding != rt::PageRounding::none)
            return rt::Status::invalid_argument;
        const auto index = req.region.value - 4U;
        if (self.mode == Memory::acquire_failure && index == 1) return rt::Status::resource_exhausted;
        auto& s = self.slots[index];
        if (s.live || s.bytes) { self.valid = false; return rt::Status::internal_error; }
        s.bytes = static_cast<std::byte*>(::operator new(req.logical_bytes, std::align_val_t{64}, std::nothrow));
        if (!s.bytes) return rt::Status::resource_exhausted;
        s.size = req.logical_bytes; s.live = true;
        ++self.counts.acquired;
        out.token = &s; out.allocation_base = s.bytes; out.allocation_bytes = s.size;
        out.usable_data = s.bytes; out.usable_bytes = s.size; out.committed_bytes = s.size;
        out.alignment = 64; out.actual_page_bytes = 4096;
        return rt::Status::ok;
    }
    static void observation(Slot& s, rt::MemoryProviderObservation& out) noexcept {
        // These observations are intentionally simulated, never OS readback.
        out.resident_bytes = s.size; out.prefaulted = true;
        out.caller_first_touched = true; out.independently_observed = true;
    }
    static rt::Status apply(void* opaque, void* token, const rt::MemoryPolicy&,
                            rt::MemoryProviderObservation& out) noexcept {
        auto& self = *static_cast<SimulatedMemory*>(opaque); auto& s = *static_cast<Slot*>(token);
        if (!s.live || s.applied) { self.valid = false; return rt::Status::internal_error; }
        s.applied = true; ++self.counts.applied;
        if (self.mode == Memory::apply_failure && &s == &self.slots[1]) return rt::Status::internal_error;
        std::fill_n(s.bytes, s.size, std::byte{}); observation(s, out);
        return rt::Status::ok;
    }
    static rt::Status observe(void* opaque, void* token, const rt::MemoryPolicy&,
                              rt::MemoryProviderObservation& out) noexcept {
        auto& self = *static_cast<SimulatedMemory*>(opaque); auto& s = *static_cast<Slot*>(token);
        if (!s.live || !s.applied) { self.valid = false; return rt::Status::internal_error; }
        ++self.counts.observed; observation(s, out); return rt::Status::ok;
    }
    static rt::Status rollback(void* opaque, void* token, const rt::MemoryPolicy&,
                               const rt::MemoryProviderObservation&) noexcept {
        auto& self = *static_cast<SimulatedMemory*>(opaque); auto& s = *static_cast<Slot*>(token);
        if (!s.live || !s.applied) { self.valid = false; return rt::Status::internal_error; }
        if (self.mode == Memory::rollback_retry && !self.retry_consumed) {
            self.retry_consumed = true; return rt::Status::internal_error;
        }
        s.applied = false; ++self.counts.rolled_back; return rt::Status::ok;
    }
    static void release(void* opaque, void* token, rt::RollbackIntent) noexcept {
        auto& self = *static_cast<SimulatedMemory*>(opaque); auto& s = *static_cast<Slot*>(token);
        if (!s.live || s.applied) self.valid = false;
        s.live = false; ++self.counts.released;
        ::operator delete(s.bytes, std::align_val_t{64}); s.bytes = nullptr;
    }
    bool settled() const noexcept {
        return valid && counts.acquired == counts.released && counts.applied == counts.rolled_back &&
            std::all_of(slots.begin(), slots.end(), [](const auto& s) { return !s.live && !s.applied; });
    }
};

struct Work {
    const Case* spec{};
    std::uint64_t frame{};
    std::array<std::atomic<unsigned>, entity_limit> visits{};
    std::array<std::uint64_t, entity_limit> output{};
    std::array<std::uint64_t, task_limit> partials{};
    std::array<std::atomic<unsigned>, 64> phase_visits{};
    std::array<std::uint64_t, 64> phase_sums{};
    std::atomic<std::uint64_t> operations{}, range_calls{}, phase_calls{};
    std::uint64_t reduction{};
    rt::Status pressure_status{rt::Status::ok}, recovery_status{rt::Status::ok};
    std::uint64_t recovery_calls{};
    void reset(std::uint64_t ordinal) noexcept {
        frame = ordinal;
        for (auto& v : visits) v.store(0, std::memory_order_relaxed);
        for (auto& v : phase_visits) v.store(0, std::memory_order_relaxed);
        output.fill(0); partials.fill(0); phase_sums.fill(0);
        operations = 0; range_calls = 0; phase_calls = 0; reduction = 0; recovery_calls = 0;
        pressure_status = rt::Status::ok; recovery_status = rt::Status::ok;
    }
    static rt::TaskResult leaf(void* opaque, const rt::TaskContext&, const rt::TaskRange& range) {
        auto& w = *static_cast<Work*>(opaque);
        if (range.end > w.spec->entities || range.begin >= range.end || range.task_index >= task_limit)
            return rt::TaskResult::error;
        ++w.range_calls;
        std::uint64_t sum = 0;
        for (auto i = range.begin; i < range.end; ++i) {
            if (w.visits[i].fetch_add(1, std::memory_order_relaxed) != 0) return rt::TaskResult::error;
            w.output[i] = value(i); sum += w.output[i]; ++w.operations;
        }
        w.partials[range.task_index] = sum;
        return rt::TaskResult::ok;
    }
    static rt::TaskResult combine(void* opaque, const rt::TaskContext&, std::size_t left, std::size_t right) {
        auto& w = *static_cast<Work*>(opaque);
        if (left >= task_limit || right >= task_limit || left >= right) return rt::TaskResult::error;
        w.partials[left] += w.partials[right]; return rt::TaskResult::ok;
    }
    struct Nest { Work* work; std::size_t remaining; };
    static rt::TaskResult nested(void* opaque, const rt::TaskContext& context, const rt::TaskRange&) {
        const auto& nest = *static_cast<Nest*>(opaque); auto& w = *nest.work;
        rt::Status status;
        if (nest.remaining != 0) {
            Nest child{&w, nest.remaining - 1};
            status = context.parallel_for(1, 1, nested, &child);
        } else if (w.spec->kind == Kind::nested_reduce) {
            status = context.parallel_reduce(w.spec->entities, w.spec->grain, leaf, combine, &w);
            w.reduction = w.partials[0];
        } else status = context.parallel_for(w.spec->entities, w.spec->grain, leaf, &w);
        return status == rt::Status::ok ? rt::TaskResult::ok : rt::TaskResult::error;
    }
    static rt::TaskResult recover(void* opaque, const rt::TaskContext&, const rt::TaskRange&) {
        ++static_cast<Work*>(opaque)->recovery_calls; return rt::TaskResult::ok;
    }
    struct Phase { Work* work{}; std::size_t index{}; };
    static rt::CallbackResult phase(void* opaque, const rt::CallbackContext& context) {
        const auto& p = *static_cast<Phase*>(opaque); auto& w = *p.work; const auto& c = *w.spec;
        if (context.frame.frame_index != w.frame || context.tasks.worker_index() >= c.workers ||
            w.phase_visits[p.index].fetch_add(1, std::memory_order_relaxed) != 0)
            return rt::CallbackResult::error;
        if (c.chain && p.index && w.phase_visits[p.index - 1].load(std::memory_order_acquire) != 1)
            return rt::CallbackResult::error;
        ++w.phase_calls;
        rt::Status status = rt::Status::ok;
        if (c.kind == Kind::nested_for || c.kind == Kind::nested_reduce) {
            Nest nest{&w, c.depth - 1};
            status = context.tasks.parallel_for(1, 1, nested, &nest);
        } else if (c.kind == Kind::range || c.kind == Kind::pressure) {
            status = context.tasks.parallel_for(c.entities, c.grain, leaf, &w);
            if (c.kind == Kind::pressure) {
                w.pressure_status = status;
                w.recovery_status = context.tasks.parallel_for(1, 1, recover, &w);
                status = w.pressure_status == rt::Status::queue_full && w.recovery_status == rt::Status::ok
                    ? rt::Status::ok : rt::Status::internal_error;
            }
        } else {
            std::uint64_t sum = 0;
            for (std::size_t i = 0; i < c.entities; ++i) { sum += value(i); ++w.operations; }
            w.phase_sums[p.index] = sum;
        }
        if (!context.scratch.empty()) context.scratch[0] = std::byte{0x5a};
        return status == rt::Status::ok ? rt::CallbackResult::ok : rt::CallbackResult::error;
    }
    bool check() const noexcept {
        const auto& c = *spec;
        if (phase_calls != c.phases) return false;
        for (std::size_t i = 0; i < c.phases; ++i) if (phase_visits[i] != 1) return false;
        if (c.kind == Kind::range || c.kind == Kind::nested_for || c.kind == Kind::nested_reduce ||
            c.kind == Kind::pressure) {
            const auto expected = c.kind == Kind::pressure ? c.queue : c.entities;
            if (operations != expected) return false;
            std::uint64_t sum = 0;
            for (std::size_t i = 0; i < c.entities; ++i) {
                if (visits[i] != (i < expected ? 1U : 0U) || output[i] != (i < expected ? value(i) : 0U))
                    return false;
                sum += output[i];
            }
            if (sum != oracle(expected)) return false;
            if (c.kind == Kind::nested_reduce && reduction != sum) return false;
            if (c.kind == Kind::pressure && (recovery_calls != 1 || pressure_status != rt::Status::queue_full ||
                                            recovery_status != rt::Status::ok)) return false;
            return range_calls == (expected + c.grain - 1) / c.grain;
        }
        if (operations != c.phases * c.entities) return false;
        for (std::size_t i = 0; i < c.phases; ++i) if (phase_sums[i] != oracle(c.entities)) return false;
        return true;
    }
};

struct Totals {
    std::uint64_t operations{}, phases{}, ranges{}, submitted{}, rejected{}, workers{}, planned{};
    MemoryCounts memory{};
    std::uint64_t resident{}, emitted{}, overwritten{}, dropped{}, checksum{};
    void add(const Totals& t) noexcept {
        operations += t.operations; phases += t.phases; ranges += t.ranges;
        submitted += t.submitted; rejected += t.rejected; workers += t.workers; planned += t.planned;
        memory.acquired += t.memory.acquired; memory.applied += t.memory.applied;
        memory.observed += t.memory.observed; memory.rolled_back += t.memory.rolled_back;
        memory.released += t.memory.released; resident += t.resident;
        emitted += t.emitted; overwritten += t.overwritten; dropped += t.dropped; checksum += t.checksum;
    }
};

// Field order is intentional: Runtime dies before every borrowed owner.
struct Fixture {
    Case spec;
    HostQueue host;
    SimulatedMemory memory;
    Work work;
    std::array<Work::Phase, 64> phase_data{};
    std::array<rt::PhaseHandle, 64> handles{};
    rt::Runtime runtime;
    bool finalized{}, started{}, stopped{};
    explicit Fixture(const Case& c) : spec(c) { work.spec = &spec; memory.mode = c.memory; }
    ~Fixture() { if (!close()) std::terminate(); }
    bool close() noexcept {
        if (stopped) return true;
        // Failed transactional finalization leaves a configuring object;
        // stop is not a legal lifecycle operation there. Verify rollback.
        if (runtime.state() == rt::RuntimeState::configuring && memory.settled() && host.count == 0) {
            stopped = true; return true;
        }
        auto status = runtime.stop();
        if (status != rt::Status::ok && spec.memory == Memory::rollback_retry && memory.retry_consumed)
            status = runtime.stop(); // one deliberate injected failure, no unbounded retry
        if (status != rt::Status::ok) return false;
        stopped = true;
        return host.count == 0 && memory.settled();
    }
    rt::Status setup() {
        rt::RuntimeConfig config;
        config.callback_capacity = spec.phases; config.worker_count = spec.workers;
        config.executor_policy = spec.policy == Policy::host ? rt::ExecutorPolicy::host_adapter :
            (spec.policy == Policy::throughput ? rt::ExecutorPolicy::bounded_throughput : rt::ExecutorPolicy::static_deterministic);
        config.executor_queue_capacity = spec.queue; config.task_scratch_slots = 512;
        config.scratch_bytes = spec.scratch; config.task_scratch_bytes = spec.scratch;
        config.trace_capacity = spec.trace;
        auto s = runtime.configure(config); if (s != rt::Status::ok) return s;
        if (spec.policy == Policy::host) {
            host.capacity = spec.queue;
            s = runtime.set_host_executor({&host, 1, spec.queue, HostQueue::submit, HostQueue::help});
            if (s != rt::Status::ok) return s;
        }
        if (spec.memory != Memory::normal) {
            rt::CpuMemoryPolicy policy;
            for (const auto region : {rt::memory_region_phase_scratch, rt::memory_region_task_scratch,
                                      rt::memory_region_trace_storage}) {
                if ((region == rt::memory_region_trace_storage && spec.trace == 0) ||
                    (region != rt::memory_region_trace_storage && spec.scratch == 0)) continue;
                auto& req = policy.memory_policies[policy.memory_policy_count++]; req.region = region;
                auto& p = req.policy; p.requirement = rt::PolicyRequirement::strict;
                p.provider = spec.memory == Memory::native ? rt::MemoryProviderOwnership::runtime : rt::MemoryProviderOwnership::host;
                p.page_rounding = spec.memory == Memory::native ? rt::PageRounding::base_page : rt::PageRounding::none;
                p.prefault = rt::PolicyToggle::enabled; p.first_touch = rt::FirstTouchPolicy::caller;
                p.residency_verification = rt::PolicyToggle::enabled; p.rollback = rt::RollbackIntent::release;
                p.locking = rt::PolicyToggle::disabled; p.pinning = rt::PolicyToggle::disabled;
                p.huge_pages = rt::HugePagePreference::disabled;
            }
            if (spec.memory != Memory::native) {
                s = runtime.set_memory_provider(memory.table()); if (s != rt::Status::ok) return s;
            }
            s = runtime.set_cpu_memory_policy(policy); if (s != rt::Status::ok) return s;
        }
        for (std::size_t i = 0; i < spec.phases; ++i) {
            phase_data[i] = {&work, i};
            s = runtime.register_callback({"cpu-phase-" + std::to_string(i), Work::phase, &phase_data[i]}, handles[i]);
            if (s != rt::Status::ok) return s;
            if (spec.chain && i) {
                s = runtime.add_dependency(handles[i - 1], handles[i]); if (s != rt::Status::ok) return s;
            }
        }
        if (spec.invalid_graph) {
            s = runtime.add_dependency(handles[spec.phases - 1], handles[0]); if (s != rt::Status::ok) return s;
        }
        s = runtime.finalize(); finalized = s == rt::Status::ok;
        return s;
    }
    rt::Status start() noexcept {
        const auto s = runtime.start(); started = s == rt::Status::ok; return s;
    }
    bool accounting(Totals& t) const noexcept {
        if (!finalized) return true;
        rt::MemoryPlan m;
        if (!runtime.memory_plan(m)) return false;
        if (m.planned_bytes != m.runtime_control_bytes + m.executor_control_bytes + m.device_control_bytes +
            m.phase_scratch_total_bytes + m.task_scratch_total_bytes + m.trace_storage_bytes ||
            m.phase_count != spec.phases) return false;
        t.planned = m.planned_bytes;
        if (spec.memory != Memory::normal && started) {
            rt::CpuMemoryPolicyReport report;
            if (!runtime.cpu_memory_policy_report(report)) return false;
            std::uint64_t accounted = 0;
            for (std::size_t i = 0; i < report.memory_count; ++i) {
                const auto& row = report.memory[i];
                if (row.region.value >= 1 && row.region.value <= 6) accounted += row.accounted_bytes;
                if (row.region.value >= 4 && row.region.value <= 6 && row.accounted_bytes) {
                    if (row.verified != rt::PolicyOperationState::succeeded || row.resident_bytes != row.committed_bytes)
                        return false;
                    t.resident += row.resident_bytes;
                }
            }
            if (accounted != m.planned_bytes) return false;
        }
        return true;
    }
    bool step(std::uint64_t frame, Totals& t) {
        work.reset(frame);
        const auto before = runtime.executor_stats();
        rt::RuntimeMetricSnapshot metrics_before, metrics_after;
        if (runtime.metrics_snapshot(rt::RuntimeMetricWindow::cumulative, nullptr, metrics_before) != rt::Status::ok)
            return false;
        const auto s = runtime.step({frame, std::chrono::nanoseconds{1}, std::nullopt});
        const auto after = runtime.executor_stats();
        // The public counter increments at worker-loop entry, after thread
        // creation. A late first entry is not a thread created by this step.
        if (s != rt::Status::ok || after.worker_starts > (spec.policy == Policy::host ? 0 : spec.workers) ||
            !work.check() || !accounting(t) ||
            runtime.metrics_snapshot(rt::RuntimeMetricWindow::cumulative, nullptr, metrics_after) != rt::Status::ok)
            return false;
        t.operations = work.operations; t.phases = work.phase_calls; t.ranges = work.range_calls;
        t.submitted = after.submitted_tasks - before.submitted_tasks;
        t.rejected = after.queue_full_rejections - before.queue_full_rejections;
        t.workers = after.worker_starts; // retained gauge, never a per-step creation claim
        if (after.local_executions - before.local_executions +
            after.successful_steals - before.successful_steals != t.submitted) return false;
        if (spec.kind == Kind::pressure && (t.rejected != 1 || t.submitted != t.ranges + 2)) return false;
        if (spec.kind != Kind::pressure && t.rejected != 0) return false;
        t.checksum = spec.kind == Kind::pressure ? oracle(spec.queue) : oracle(spec.entities) * spec.phases;
        const auto metric = [&](rt::RuntimeMetricId id) {
            const auto index = static_cast<std::size_t>(id);
            return metrics_after.samples[index].value - metrics_before.samples[index].value;
        };
        t.emitted = metric(rt::RuntimeMetricId::trace_events_emitted);
        t.overwritten = metric(rt::RuntimeMetricId::trace_events_overwritten);
        t.dropped = metric(rt::RuntimeMetricId::trace_events_dropped);
        return true;
    }
};

bool run_lifecycle(const Case& c, std::uint64_t frame, Totals& total) {
    for (std::size_t cycle = 0; cycle < c.cycles; ++cycle) {
        auto f = std::make_unique<Fixture>(c);
        auto s = f->setup(); Totals t;
        if (c.invalid_graph) {
            if (s != rt::Status::graph_cycle || f->work.phase_calls != 0 || f->runtime.executor_stats().worker_starts != 0)
                return false;
        } else if (c.memory == Memory::acquire_failure) {
            if (s != rt::Status::resource_exhausted || f->work.phase_calls != 0) return false;
        } else {
            if (s != rt::Status::ok || !f->accounting(t)) return false;
            if (c.kind == Kind::compile) {
                if (f->runtime.callback_count() != c.phases || f->runtime.executor_stats().worker_starts != 0) return false;
                for (std::size_t i = 0; i < c.phases; ++i) {
                    rt::PhaseHandle phase;
                    if (!f->runtime.compiled_phase_at(i, phase) || phase != f->handles[i]) return false;
                }
                rt::PhaseHandle past_end;
                if (f->runtime.compiled_phase_at(c.phases, past_end) ||
                    f->runtime.dependency_count() != (c.chain ? c.phases - 1 : 0)) return false;
                t.operations = f->runtime.callback_count(); t.checksum = t.operations;
            } else {
                s = f->start();
                if (c.memory == Memory::apply_failure) {
                    if (s != rt::Status::internal_error || f->work.phase_calls != 0) return false;
                } else if (s != rt::Status::ok || !f->step(frame, t)) return false;
            }
        }
        if (!f->close()) return false;
        t.memory = f->memory.counts;
        if (c.memory != Memory::normal && c.memory != Memory::native && c.scratch != 0) {
            if (t.memory.acquired == 0 || t.memory.released != t.memory.acquired) return false;
            if (c.memory != Memory::acquire_failure && t.memory.applied == 0) return false;
        }
        if (c.scratch == 0 && c.trace == 0 && t.memory.acquired != 0) return false;
        if (c.memory == Memory::rollback_retry && !f->memory.retry_consumed) return false;
        total.add(t);
    }
    return true;
}
} // namespace

struct Provider::State {
    const Case* selected{};
    std::unique_ptr<Fixture> first, second;
    Status preparation{Status::invalid};
    std::uint64_t calls{};
};
std::span<const Case> cases() noexcept { return catalog; }
Provider::Provider() : state_(std::make_unique<State>()) {}
Provider::~Provider() { if (finish() != Status::ok) std::terminate(); }
bool Provider::prepared() const noexcept { return state_->selected != nullptr; }
std::uint64_t Provider::invocations() const noexcept { return state_->calls; }
ProviderV1 Provider::table() noexcept {
    ProviderV1 t; t.id = "rtfw.cpu"; t.case_count = cases().size(); t.user = this;
    t.describe = describe; t.invoke = invoke; return t;
}
Status Provider::describe(void*, std::size_t index, Descriptor& d) {
    if (index >= cases().size()) return Status::not_found;
    const auto& c = catalog[index]; d = {};
    d.case_id = c.id; d.subsystem = c.family; d.implementation = "public-runtime-cpu-v1";
    d.configuration = lifecycle(c) ? "lifecycle-setup-inspect-cleanup" : "step-and-observation";
    d.workload_kind = "integer-oracle";
    d.parameters = {{"phases", c.phases, 1, 64}, {"entities", c.entities, 0, entity_limit},
        {"grain", c.grain, 1, 64}, {"workers", c.workers, 1, 4}, {"depth", c.depth, 0, 2},
        {"queue", c.queue, 2, 512}, {"trace", c.trace, 0, 256}, {"scratch", c.scratch, 0, 8192},
        {"cycles", c.cycles, 1, 4}, {"policy", static_cast<std::uint64_t>(c.policy), 0, 2},
        {"memory", static_cast<std::uint64_t>(c.memory), 0, 5},
        {"chain", c.chain ? 1U : 0U, 0, 1}, {"invalid_graph", c.invalid_graph ? 1U : 0U, 0, 1}};
    std::string workload = "cpu-v1:" + std::string(c.id);
    for (const auto& p : d.parameters) workload += ":" + p.name + "=" + std::to_string(p.value);
    d.workload_sha256 = sha256(workload);
    d.counters = {{"operations", c.kind == Kind::compile ? "phases" : "entities"}, {"phase_calls"}, {"range_calls"}, {"submitted"}, {"rejected"}};
    if (!deterministic_counters(c)) {
        for (const auto* name : {"worker_starts", "planned_bytes", "acquired", "applied", "observed",
                                "rolled_back", "released", "resident_bytes", "trace_emitted", "trace_overwritten", "trace_dropped"})
            d.counters.push_back({name, std::string_view(name).ends_with("bytes") ? "bytes" : "count"});
    }
    return validate(d);
}
Status Provider::finish() noexcept {
    auto& s = *state_;
    if (s.second && !s.second->close()) return Status::provider_error;
    if (s.second && s.first && s.preparation == Status::ok && s.calls == 7 && !s.first->stopped) {
        Totals continuation;
        if (!s.first->step(7, continuation)) return Status::provider_error;
    }
    if (s.first && !s.first->close()) return Status::provider_error;
    s.second.reset(); s.first.reset(); s.selected = nullptr; s.preparation = Status::invalid;
    return Status::ok;
}
Status Provider::prepare(std::string_view id) {
    auto& s = *state_;
    if (s.selected) return Status::busy;
    const auto found = std::find_if(cases().begin(), cases().end(), [&](const auto& c) { return c.id == id; });
    if (found == cases().end()) return Status::not_found;
    s.selected = &*found; s.calls = 0; s.preparation = Status::provider_error;
    const auto& c = *s.selected;
    if (c.memory == Memory::native) {
#if defined(__linux__)
        // Probe only availability before warm-up. Never turn a workload,
        // accounting or cleanup failure into NOT RUN.
        auto probe = std::make_unique<Fixture>(c);
        auto status = probe->setup();
        bool unavailable = status == rt::Status::resource_exhausted;
        bool valid = status == rt::Status::ok;
        if (valid) {
            status = probe->start();
            valid = status == rt::Status::ok;
            if (!valid) {
                rt::CpuMemoryPolicyReport report;
                if (probe->runtime.cpu_memory_policy_report(report)) {
                    for (std::size_t i = 0; i < report.memory_count; ++i) {
                        const auto& row = report.memory[i];
                        unavailable = unavailable || (row.accounted_bytes != 0 &&
                            (row.verified == rt::PolicyOperationState::failed ||
                             row.verified == rt::PolicyOperationState::mismatched));
                    }
                }
            }
        }
        Totals accounting;
        if (valid) valid = probe->accounting(accounting);
        if (!probe->close()) return s.preparation;
        if (unavailable) { s.preparation = Status::not_run; return s.preparation; }
        if (!valid) return s.preparation;
#else
        s.preparation = Status::not_run; return s.preparation;
#endif
    }
    if (!lifecycle(c)) {
        s.first = std::make_unique<Fixture>(c);
        if (s.first->setup() != rt::Status::ok || s.first->start() != rt::Status::ok) return s.preparation;
        if (c.kind == Kind::multiple) {
            s.second = std::make_unique<Fixture>(c);
            if (s.second->setup() != rt::Status::ok || s.second->start() != rt::Status::ok) return s.preparation;
        }
    }
    s.preparation = Status::ok; return s.preparation;
}
Status Provider::invoke(void* opaque, std::string_view id, std::uint64_t ordinal, Observation& out) {
    auto& provider = *static_cast<Provider*>(opaque); auto& s = *provider.state_;
    if (!s.selected || s.selected->id != id || ordinal != s.calls) return Status::provider_error;
    ++s.calls;
    if (s.preparation != Status::ok) return s.preparation;
    const auto& c = *s.selected; Totals t;
    if (lifecycle(c)) {
        if (!run_lifecycle(c, ordinal, t)) return Status::provider_error;
    } else {
        if (!s.first->step(ordinal, t)) return Status::provider_error;
        if (s.second) {
            Totals second;
            if (!s.second->step(ordinal, second)) return Status::provider_error;
            t.add(second);
        }
    }
    out.counters = {t.operations, t.phases, t.ranges, t.submitted, t.rejected};
    if (!deterministic_counters(c)) {
        const std::array rest{t.workers, t.planned, t.memory.acquired, t.memory.applied, t.memory.observed,
            t.memory.rolled_back, t.memory.released, t.resident, t.emitted, t.overwritten, t.dropped};
        out.counters.insert(out.counters.end(), rest.begin(), rest.end());
    }
    out.checksum = t.checksum; out.correct = true;
    return Status::ok;
}
} // namespace rtfw::benchmark::cpu
