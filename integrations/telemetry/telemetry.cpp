#include "telemetry.hpp"
#include <algorithm>
#include <limits>
#include <locale>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace rtfw_telemetry {
namespace {
bool token(std::string_view s) noexcept {
    if (s.empty() || s.size() > 63) return false;
    for (const auto c : s) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '_' || c == ':' ||
              c == '/' || c == '@' || c == '-')) return false;
    }
    return true;
}
template<std::size_t N> std::string_view identifier(const std::array<char, N>& s) noexcept {
    const auto end = std::find(s.begin(), s.end(), '\0');
    if (end == s.end()) return {};
    return {s.data(), static_cast<std::size_t>(end - s.begin())};
}
void bump(std::uint64_t& value) noexcept {
    if (value != std::numeric_limits<std::uint64_t>::max()) ++value;
}
}
bool valid_context(const Context& c) noexcept {
    return token(c.session) && token(c.clock_domain) && c.unix_anchor_ns != 0;
}
bool map_timestamp(const Context& c, std::uint64_t t, std::uint64_t& out) noexcept {
    if (t >= c.runtime_anchor_ns) {
        const auto delta = t - c.runtime_anchor_ns;
        if (delta > std::numeric_limits<std::uint64_t>::max() - c.unix_anchor_ns) return false;
        out = c.unix_anchor_ns + delta;
    } else {
        const auto delta = c.runtime_anchor_ns - t;
        if (delta > c.unix_anchor_ns) return false;
        out = c.unix_anchor_ns - delta;
    }
    return out != 0;
}
bool valid_snapshot(const Snapshot& s) noexcept {
    const auto& m = s.metrics.metadata;
    if (!valid_context(s.context) || s.batch_sequence == 0 || m.runtime_id == 0 ||
        m.schema_version != 2 || m.trace_event_size != 64 || m.metric_sample_size != 16 ||
        m.metric_count != rt::runtime_metric_count || s.metrics.sample_count != rt::runtime_metric_count ||
        s.metrics.window != rt::RuntimeMetricWindow::interval ||
        s.trace.events_read > max_events || s.trace.metadata.runtime_id != m.runtime_id ||
        s.trace.metadata.config_id != m.config_id || s.trace.metadata.schema_version != 2 ||
        !token(identifier(m.build_id)) || !token(identifier(m.workload_id)) ||
        s.metrics.window_start_ns > s.metrics.window_end_ns ||
        s.trace.first_sequence > s.trace.next_sequence) return false;
    std::uint64_t mapped = 0;
    if (!map_timestamp(s.context, s.metrics.window_start_ns, mapped) ||
        !map_timestamp(s.context, s.metrics.window_end_ns, mapped)) return false;
    for (std::size_t i = 0; i < s.metrics.sample_count; ++i) {
        rt::RuntimeMetricDefinition d;
        const auto& value = s.metrics.samples[i];
        if (!rt::runtime_metric_definition(i, d) || value.id != d.id || value.kind != d.kind ||
            value.reserved0 != 0 || value.reserved1 != 0) return false;
    }
    std::uint64_t previous = 0;
    for (std::size_t i = 0; i < s.trace.events_read; ++i) {
        const auto& e = s.events[i];
        const auto type = static_cast<unsigned>(e.type);
        const auto status = static_cast<std::int32_t>(e.status);
        if (e.schema_version != 2 || e.record_size != 64 || e.reserved0 != 0 || e.reserved1 != 0 ||
            type < 1 || type > 14 || static_cast<unsigned>(e.producer) > 2 || status > 0 || status < -23 ||
            e.sequence < s.trace.first_sequence || e.sequence >= s.trace.next_sequence ||
            (i != 0 && e.sequence <= previous) || !map_timestamp(s.context, e.timestamp_ns, mapped)) return false;
        previous = e.sequence;
    }
    return true;
}
Queue::Queue(Context context, std::size_t capacity) : context_(std::move(context)) {
    if (!valid_context(context_) || capacity == 0 || capacity > max_batches)
        throw std::invalid_argument("invalid telemetry context/capacity");
    slots_.resize(capacity);
    // All variable-size identity storage is bounded and copied before capture.
    for (auto& slot : slots_) slot.context = context_;
}
rt::Status Queue::capture(rt::Runtime& runtime) {
    const std::lock_guard lock(mutex_);
    if (stats_.closed) return rt::Status::invalid_state;
    if (!enabled_) { bump(stats_.disabled); return rt::Status::invalid_state; }
    if (stats_.pending == slots_.size()) { bump(stats_.full); return rt::Status::queue_full; }
    if (stats_.accepted == std::numeric_limits<std::uint64_t>::max()) return rt::Status::capacity_exceeded;
    auto& s = slots_[(head_ + stats_.pending) % slots_.size()];
    auto metric = metric_cursor_;
    auto trace = trace_cursor_;
    auto status = runtime.metrics_snapshot(rt::RuntimeMetricWindow::interval, &metric, s.metrics);
    if (status == rt::Status::ok && runtime_id_ != 0 && s.metrics.metadata.runtime_id != runtime_id_)
        status = rt::Status::invalid_argument;
    if (status == rt::Status::ok) status = runtime.read_trace(trace, s.events, s.trace);
    s.batch_sequence = stats_.accepted + 1;
    s.queue_full = stats_.full;
    if (status == rt::Status::ok && !valid_snapshot(s)) status = rt::Status::invalid_artifact;
    if (status != rt::Status::ok) { bump(stats_.capture_failures); return status; }
    metric_cursor_ = metric;
    trace_cursor_ = trace;
    runtime_id_ = s.metrics.metadata.runtime_id;
    ++stats_.accepted;
    ++stats_.pending;
    return rt::Status::ok;
}
bool Queue::drain_one(const std::function<bool(const Snapshot&)>& sink) {
    const std::lock_guard lock(mutex_);
    if (stats_.pending == 0) return false;
    try {
        if (!sink(slots_[head_])) { bump(stats_.sink_failures); return false; }
    } catch (...) { bump(stats_.sink_failures); throw; }
    head_ = (head_ + 1) % slots_.size();
    --stats_.pending;
    bump(stats_.drained);
    return true;
}
void Queue::set_enabled(bool enabled) { const std::lock_guard lock(mutex_); enabled_ = enabled; }
void Queue::close(bool discard) {
    const std::lock_guard lock(mutex_);
    stats_.closed = true;
    if (discard) {
        while (stats_.pending != 0) {
            bump(stats_.discarded_batches);
            for (std::size_t i = 0; i < slots_[head_].trace.events_read; ++i) bump(stats_.discarded_events);
            head_ = (head_ + 1) % slots_.size();
            --stats_.pending;
        }
    }
}
Statistics Queue::statistics() const { const std::lock_guard lock(mutex_); return stats_; }

bool write_json(const Snapshot& s, std::ostream& destination) {
    if (!valid_snapshot(s)) return false;
    // Isolate the schema from caller locale/hex/boolalpha/precision flags.
    std::ostringstream o;
    o.imbue(std::locale::classic());
    const auto& c = s.context;
    const auto& m = s.metrics.metadata;
    o << "{\"spool_schema\":1,\"session\":\"" << c.session << "\",\"clock_domain\":\"" << c.clock_domain
      << "\",\"runtime_anchor_ns\":" << c.runtime_anchor_ns << ",\"unix_anchor_ns\":" << c.unix_anchor_ns
      << ",\"uncertainty_ns\":" << c.uncertainty_ns << ",\"batch_sequence\":" << s.batch_sequence
      << ",\"queue_full\":" << s.queue_full << ",\"schema_version\":" << m.schema_version
      << ",\"runtime_id\":" << m.runtime_id << ",\"config_id\":" << m.config_id
      << ",\"build_id\":\"" << identifier(m.build_id) << "\",\"workload_id\":\"" << identifier(m.workload_id)
      << "\",\"runtime_version\":[" << m.runtime_version_major << ',' << m.runtime_version_minor << ',' << m.runtime_version_patch
      << "],\"trace_capacity\":" << m.trace_capacity
      << ",\"metrics\":{\"sequence\":" << s.metrics.snapshot_sequence << ",\"start_ns\":" << s.metrics.window_start_ns
      << ",\"end_ns\":" << s.metrics.window_end_ns << ",\"window\":\"interval\",\"samples\":[";
    for (std::size_t i = 0; i < s.metrics.sample_count; ++i) {
        if (i != 0) o << ',';
        rt::RuntimeMetricDefinition d;
        if (!rt::runtime_metric_definition(i, d)) return false;
        o << "{\"id\":" << i << ",\"name\":\"" << d.name << "\",\"kind\":" << static_cast<unsigned>(d.kind)
          << ",\"value\":" << s.metrics.samples[i].value << '}';
    }
    o << "]},\"trace\":{\"first_sequence\":" << s.trace.first_sequence << ",\"next_sequence\":" << s.trace.next_sequence
      << ",\"lost_events\":" << s.trace.lost_events << ",\"remaining_sequence_count\":" << s.trace.remaining_sequence_count
      << ",\"events\":[";
    for (std::size_t i = 0; i < s.trace.events_read; ++i) {
        if (i != 0) o << ',';
        const auto& e = s.events[i];
        o << "{\"type\":" << static_cast<unsigned>(e.type) << ",\"name\":\"" << rt::runtime_trace_event_name(e.type)
          << "\",\"status\":" << static_cast<std::int32_t>(e.status) << ",\"producer\":" << static_cast<unsigned>(e.producer)
          << ",\"sequence\":" << e.sequence << ",\"timestamp_ns\":" << e.timestamp_ns << ",\"frame\":" << e.frame_index
          << ",\"callback\":" << e.callback_index << ",\"worker\":" << e.worker_index << ",\"value\":" << e.value << '}';
    }
    o << "]}}\n";
    const auto bytes = o.str();
    destination.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return destination.good();
}
} // namespace rtfw_telemetry
