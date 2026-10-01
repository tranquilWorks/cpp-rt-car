#include "etw.hpp"
#ifdef _WIN32
#include <TraceLoggingProvider.h>
#include <stdexcept>

namespace rtfw_telemetry {
namespace {
TRACELOGGING_DEFINE_PROVIDER(provider, "RTFW.Telemetry",
    (0x7744f8d4, 0xbb73, 0x4f12, 0x9b, 0xe6, 0x49, 0xe1, 0x59, 0x81, 0x13, 0x70));
const GUID guid{0x7744f8d4, 0xbb73, 0x4f12, {0x9b, 0xe6, 0x49, 0xe1, 0x59, 0x81, 0x13, 0x70}};
std::mutex provider_mutex;
std::size_t references = 0;
}
Etw::Etw() {
    const std::lock_guard lock(provider_mutex);
    if (references == 0 && TraceLoggingRegister(provider) != ERROR_SUCCESS)
        throw std::runtime_error("ETW registration failed");
    ++references;
}
Etw::~Etw() {
    const std::lock_guard lock(provider_mutex);
    if (--references == 0) TraceLoggingUnregister(provider);
}
const GUID& Etw::provider_id() noexcept { return guid; }
bool Etw::write(const Snapshot& s) {
    if (!valid_snapshot(s)) return false;
    const std::lock_guard lock(provider_mutex);
    if (!TraceLoggingProviderEnabled(provider, 4, 1)) return false;
    const auto& c = s.context;
    const auto& m = s.metrics.metadata;
    TraceLoggingWrite(provider, "Snapshot", TraceLoggingLevel(4), TraceLoggingKeyword(1),
        TraceLoggingString(c.session.c_str(), "Session"),
        TraceLoggingUInt64(m.runtime_id, "RuntimeId"), TraceLoggingUInt64(m.config_id, "ConfigId"),
        TraceLoggingUInt64(s.batch_sequence, "BatchSequence"),
        TraceLoggingString(m.build_id.data(), "BuildId"), TraceLoggingString(m.workload_id.data(), "WorkloadId"),
        TraceLoggingString(c.clock_domain.c_str(), "ClockDomain"),
        TraceLoggingUInt64(c.runtime_anchor_ns, "RuntimeAnchorNs"),
        TraceLoggingUInt64(c.unix_anchor_ns, "UnixAnchorNs"),
        TraceLoggingUInt64(c.uncertainty_ns, "UncertaintyNs"),
        TraceLoggingUInt64(s.queue_full, "QueueFull"), TraceLoggingUInt64(s.trace.lost_events, "RuntimeLost"),
        TraceLoggingUInt64(s.trace.remaining_sequence_count, "RemainingSequences"),
        TraceLoggingUInt64(s.trace.first_sequence, "FirstSequence"),
        TraceLoggingUInt64(s.trace.next_sequence, "NextSequence"),
        TraceLoggingUInt64(s.metrics.snapshot_sequence, "MetricSequence"),
        TraceLoggingUInt64(s.metrics.window_start_ns, "WindowStartNs"),
        TraceLoggingUInt64(s.metrics.window_end_ns, "WindowEndNs"),
        TraceLoggingUInt32(m.schema_version, "RuntimeSchema"),
        TraceLoggingUInt32(m.runtime_version_major, "VersionMajor"),
        TraceLoggingUInt32(m.runtime_version_minor, "VersionMinor"),
        TraceLoggingUInt32(m.runtime_version_patch, "VersionPatch"),
        TraceLoggingUInt64(m.trace_capacity, "TraceCapacity"),
        TraceLoggingUInt8(1, "MetricWindow"));
    for (std::size_t i = 0; i < s.trace.events_read; ++i) {
        const auto& e = s.events[i];
        std::uint64_t unix_ns = 0;
        if (!map_timestamp(c, e.timestamp_ns, unix_ns)) return false;
        TraceLoggingWrite(provider, "RuntimeEvent", TraceLoggingLevel(4), TraceLoggingKeyword(1),
            TraceLoggingString(c.session.c_str(), "Session"),
            TraceLoggingUInt64(m.runtime_id, "RuntimeId"),
            TraceLoggingUInt64(s.batch_sequence, "BatchSequence"),
            TraceLoggingString(rt::runtime_trace_event_name(e.type), "Name"),
            TraceLoggingUInt16(static_cast<std::uint16_t>(e.type), "Type"),
            TraceLoggingUInt64(e.sequence, "Sequence"), TraceLoggingUInt64(e.timestamp_ns, "RuntimeTimestampNs"),
            TraceLoggingUInt64(unix_ns, "UnixTimestampNs"), TraceLoggingUInt64(e.frame_index, "Frame"),
            TraceLoggingUInt32(e.callback_index, "Callback"), TraceLoggingUInt32(e.worker_index, "Worker"),
            TraceLoggingUInt16(static_cast<std::uint16_t>(e.producer), "Producer"),
            TraceLoggingInt32(static_cast<std::int32_t>(e.status), "Status"), TraceLoggingUInt64(e.value, "Value"));
    }
    for (std::size_t i = 0; i < s.metrics.sample_count; ++i) {
        rt::RuntimeMetricDefinition d;
        if (!rt::runtime_metric_definition(i, d)) return false;
        const std::string metric_name(d.name); // Keep a terminated copy alive through emission.
        TraceLoggingWrite(provider, "RuntimeMetric", TraceLoggingLevel(4), TraceLoggingKeyword(1),
            TraceLoggingString(c.session.c_str(), "Session"),
            TraceLoggingUInt64(m.runtime_id, "RuntimeId"),
            TraceLoggingUInt64(s.batch_sequence, "BatchSequence"),
            TraceLoggingString(metric_name.c_str(), "Name"), TraceLoggingUInt16(static_cast<std::uint16_t>(d.id), "Id"),
            TraceLoggingUInt8(static_cast<std::uint8_t>(d.kind), "Kind"),
            TraceLoggingUInt64(s.metrics.samples[i].value, "Value"));
    }
    return true; // emission attempted; collector delivery remains unacknowledged
}
} // namespace rtfw_telemetry
#endif
