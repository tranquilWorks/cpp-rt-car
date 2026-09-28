#pragma once
#include "jobs.hpp"
#include "memory.hpp"
#include <rt/sdk.hpp>
#include <algorithm>
#include <limits>
#include <optional>

namespace host_example {
class Clock final : public rt::RuntimeClock {
public:
    std::atomic<bool> fail{false};
    std::uint64_t now_ns() noexcept override {
        if (fail.load(std::memory_order_relaxed)) return 0; // Explicit fixture only.
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    }
};
struct World {
    std::array<std::int64_t, 16> values{};
    std::int64_t multiplier = 1, checksum = 0;
    bool fail_callback = false;
    struct Row { World* world; std::size_t row; };
    static rt::TaskResult columns(void* opaque, const rt::TaskContext& ctx, const rt::TaskRange& range) noexcept {
        auto& row = *static_cast<Row*>(opaque);
        if (ctx.scratch().size() != 64) return rt::TaskResult::error;
        std::fill(ctx.scratch().begin(), ctx.scratch().end(), std::byte{0x2a});
        for (auto col = range.begin; col < range.end; ++col) {
            const auto i = row.row * 4 + col;
            row.world->values[i] += row.world->multiplier * static_cast<std::int64_t>(i + 1);
        }
        return rt::TaskResult::ok;
    }
    static rt::TaskResult rows(void* opaque, const rt::TaskContext& ctx, const rt::TaskRange& range) noexcept {
        auto& world = *static_cast<World*>(opaque);
        if (ctx.scratch().size() != 64) return rt::TaskResult::error;
        std::fill(ctx.scratch().begin(), ctx.scratch().end(), std::byte{0x51});
        for (auto i = range.begin; i < range.end; ++i) {
            Row row{&world, i};
            if (ctx.parallel_for(4, 1, &columns, &row) != rt::Status::ok) return rt::TaskResult::error;
        }
        return std::all_of(ctx.scratch().begin(), ctx.scratch().end(), [](auto b) { return b == std::byte{0x51}; })
            ? rt::TaskResult::ok : rt::TaskResult::error;
    }
    rt::CallbackResult integrate(const rt::CallbackContext& ctx) noexcept {
        if (fail_callback || ctx.frame.delta != std::chrono::milliseconds(1)) return rt::CallbackResult::error;
        return ctx.tasks.parallel_for(4, 1, &rows, this) == rt::Status::ok ? rt::CallbackResult::ok : rt::CallbackResult::error;
    }
    rt::CallbackResult summarize(const rt::CallbackContext&) noexcept {
        checksum = 0; for (const auto v : values) checksum += v;
        return rt::CallbackResult::ok;
    }
    bool matches(std::uint64_t frames) const noexcept {
        for (std::size_t i = 0; i < values.size(); ++i)
            if (values[i] != static_cast<std::int64_t>(frames) * multiplier * static_cast<std::int64_t>(i + 1)) return false;
        return checksum == static_cast<std::int64_t>(frames) * multiplier * 136;
    }
};
struct Telemetry {
    rt::RuntimeTraceCursor cursor{};
    std::uint64_t runtime_id = 0, next = 0, events = 0, lost = 0, begins = 0, ends = 0, callbacks = 0;
    rt::Status drain(rt::Runtime& runtime, bool allow_loss = false) noexcept {
        std::array<rt::RuntimeTraceEvent, 32> buffer{};
        // Capacity is at most256. No callback executes concurrently with drain.
        for (unsigned batch = 0; batch < 9; ++batch) {
            rt::RuntimeTraceReadResult read;
            auto s = runtime.read_trace(cursor, buffer, read); if (s != rt::Status::ok) return s;
            if (!runtime_id) runtime_id = read.metadata.runtime_id;
            if (!runtime_id || runtime_id != read.metadata.runtime_id || read.events_read > buffer.size()) return rt::Status::internal_error;
            lost += read.lost_events;
            if (read.lost_events && !allow_loss) return rt::Status::resource_exhausted;
            if (read.lost_events) next = read.first_sequence;
            for (std::size_t i = 0; i < read.events_read; ++i) {
                const auto& e = buffer[i];
                if (!next) next = e.sequence;
                if (e.sequence != next++ || e.schema_version != rt::observability_schema_version || e.record_size != sizeof(e)) return rt::Status::internal_error;
                ++events;
                if (e.type == rt::RuntimeTraceEventType::step_begin) ++begins;
                if (e.type == rt::RuntimeTraceEventType::step_end) ++ends;
                if (e.type == rt::RuntimeTraceEventType::callback_end) ++callbacks;
            }
            if (!read.remaining_sequence_count) return rt::Status::ok;
        }
        return rt::Status::resource_exhausted;
    }
    bool metrics(rt::Runtime& runtime, std::uint64_t frames) noexcept {
        rt::RuntimeMetricSnapshot m;
        if (runtime.metrics_snapshot(rt::RuntimeMetricWindow::cumulative, nullptr, m) != rt::Status::ok ||
            m.metadata.runtime_id != runtime_id || m.sample_count != rt::runtime_metric_count) return false;
        bool completed = false, failures = false, calls = false;
        for (std::size_t i = 0; i < m.sample_count; ++i) {
            if (m.samples[i].id == rt::RuntimeMetricId::frames_completed) completed = m.samples[i].value == frames;
            if (m.samples[i].id == rt::RuntimeMetricId::frames_failed) failures = m.samples[i].value == 0;
            if (m.samples[i].id == rt::RuntimeMetricId::callbacks_completed) calls = m.samples[i].value == frames * 2;
        }
        return completed && failures && calls;
    }
};
// Borrowed clock, job system and memory owner precede and outlive this session.
// Lifecycle methods are serialized on the host frame thread, after frame callers
// join. Checked close can be retried; a failed close never clears ownership.
class Session {
    Clock& clock_;
    Jobs* jobs_;
    Memory& memory_;
    bool attached_ = false, closed_ = false, ready_ = false;
    std::uint64_t last_clock_ = 0;
public:
    World world;
    std::optional<rt::Runtime> runtime;
    Telemetry telemetry;
    std::uint64_t frames = 0;
    Session(Clock& clock, Jobs* jobs, Memory& memory, std::int64_t multiplier)
        : clock_(clock), jobs_(jobs), memory_(memory), runtime(std::in_place, clock) { world.multiplier = multiplier; }
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    ~Session() { if (close() != rt::Status::ok) std::terminate(); }
    rt::Status prepare(std::size_t trace_capacity = 256, bool wrong_adapter_capacity = false) noexcept {
        if (closed_ || ready_) return rt::Status::invalid_state;
        rt::RuntimeConfig c; c.worker_count = Jobs::workers; c.executor_queue_capacity = Jobs::capacity;
        c.callback_capacity = 2; c.scratch_bytes = 64; c.task_scratch_bytes = 64; c.task_scratch_slots = 64;
        c.trace_capacity = trace_capacity;
        if (jobs_) c.executor_policy = rt::ExecutorPolicy::host_adapter;
        auto s = runtime->configure(c); if (s != rt::Status::ok) return s;
        if (jobs_) {
            s = jobs_->attach(); if (s != rt::Status::ok) return s; attached_ = true;
            auto table = jobs_->adapter(); if (wrong_adapter_capacity) ++table.queue_capacity;
            s = runtime->set_host_executor(table); if (s != rt::Status::ok) return s;
        }
        s = runtime->set_memory_provider(memory_.table()); if (s != rt::Status::ok) return s;
        s = runtime->set_cpu_memory_policy(Memory::policy()); if (s != rt::Status::ok) return s;
        rt::sdk::GraphBuilder graph(*runtime); rt::PhaseHandle integrate, summary;
        s = graph.phase<&World::integrate>("host.integrate", world, integrate); if (s != rt::Status::ok) return s;
        s = graph.phase<&World::summarize>("host.summary", world, summary); if (s != rt::Status::ok) return s;
        s = graph.depends_on(summary, integrate); if (s != rt::Status::ok) return s;
        s = graph.finalize(); if (s != rt::Status::ok) return s;
        rt::MemoryPlan plan;
        if (!runtime->memory_plan(plan) || plan.phase_scratch_total_bytes > Memory::region_capacity ||
            plan.task_scratch_total_bytes > Memory::region_capacity || plan.trace_storage_bytes > Memory::region_capacity ||
            memory_.live_count() != 3) return rt::Status::internal_error;
        s = runtime->start(); if (s != rt::Status::ok) return s;
        ready_ = true; return telemetry.drain(*runtime);
    }
    rt::Status step(bool drain = true) noexcept {
        if (!ready_ || closed_ || frames >= 1024) return rt::Status::invalid_state;
        const auto now = clock_.now_ns();
        if (!now || now < last_clock_) return rt::Status::clock_failure;
        last_clock_ = now;
        auto s = runtime->step({frames, std::chrono::milliseconds(1)});
        if (s != rt::Status::ok) { ready_ = false; return s; }
        ++frames;
        if (!world.matches(frames)) return rt::Status::internal_error;
        return drain ? telemetry.drain(*runtime) : rt::Status::ok;
    }
    rt::Status close() noexcept {
        if (closed_) return rt::Status::ok;
        ready_ = false; // Stop frame admission before quiescence, retain borrowed owners on failure.
        if (attached_) { auto s = jobs_->quiesce(); if (s != rt::Status::ok) return s; }
        if (runtime->state() != rt::RuntimeState::configuring) {
            auto s = runtime->stop(); if (s != rt::Status::ok) return s;
        }
        if (memory_.live_count() || memory_.violations) return rt::Status::internal_error;
        runtime.reset(); // Destroy the borrowed adapter table before detaching its host owner.
        if (attached_) { auto s = jobs_->detach(); if (s != rt::Status::ok) return s; attached_ = false; }
        closed_ = true; return rt::Status::ok;
    }
};
struct Host {
    Clock clock;
    Jobs jobs;
    std::array<Memory, 2> memory;
};
} // namespace host_example
