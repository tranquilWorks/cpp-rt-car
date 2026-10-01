#pragma once

// Optional source kit. Every operation belongs on a non-RT host lane.
#include <rt/runtime.hpp>
#include <array>
#include <cstdint>
#include <functional>
#include <iosfwd>
#include <mutex>
#include <string>
#include <vector>

namespace rtfw_telemetry {
inline constexpr std::size_t max_events = 256;
inline constexpr std::size_t max_batches = 64;

// Caller certifies an offset-only mapping of its Runtime clock to Unix ns.
// This does not measure synchronization, clock drift or RT timing accuracy.
struct Context {
    std::string session;
    std::string clock_domain;
    std::uint64_t runtime_anchor_ns = 0;
    std::uint64_t unix_anchor_ns = 0;
    std::uint64_t uncertainty_ns = 0;
};
struct Statistics {
    std::uint64_t accepted = 0, drained = 0, full = 0, disabled = 0;
    std::uint64_t capture_failures = 0, sink_failures = 0;
    std::uint64_t discarded_batches = 0, discarded_events = 0;
    std::size_t pending = 0;
    bool closed = false;
};
struct Snapshot {
    Context context;
    std::uint64_t batch_sequence = 0;
    std::uint64_t queue_full = 0;
    rt::RuntimeMetricSnapshot metrics;
    rt::RuntimeTraceReadResult trace;
    std::array<rt::RuntimeTraceEvent, max_events> events{};
};

bool valid_context(const Context&) noexcept;
bool map_timestamp(const Context&, std::uint64_t runtime_ns, std::uint64_t& unix_ns) noexcept;
bool valid_snapshot(const Snapshot&) noexcept;
bool write_json(const Snapshot&, std::ostream&);

// One Runtime owner per queue. Caller serializes capture with ALL Runtime
// control/step calls. A separate host consumer may drain concurrently.
class Queue final {
public:
    explicit Queue(Context context, std::size_t capacity = 4);
    Queue(const Queue&) = delete;
    Queue& operator=(const Queue&) = delete;
    rt::Status capture(rt::Runtime&);
    // Sink runs under the queue lock, must not reenter, and acknowledges the
    // entire snapshot. Failure/exception retains it; partial writes can duplicate.
    bool drain_one(const std::function<bool(const Snapshot&)>& sink);
    void set_enabled(bool);
    void close(bool discard = false);
    Statistics statistics() const;
private:
    Context context_;
    mutable std::mutex mutex_;
    std::vector<Snapshot> slots_;
    std::size_t head_ = 0;
    bool enabled_ = true;
    Statistics stats_;
    rt::RuntimeMetricCursor metric_cursor_;
    rt::RuntimeTraceCursor trace_cursor_;
    std::uint64_t runtime_id_ = 0;
};
} // namespace rtfw_telemetry
