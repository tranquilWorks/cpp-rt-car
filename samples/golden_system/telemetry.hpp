#pragma once
#include "model.hpp"
#include <rt/runtime.hpp>
namespace golden {
// Derived off-lane trace reader from the unchanged M25-05 kit.
struct Telemetry {
  rt::RuntimeTraceCursor cursor{};
  std::uint64_t runtime_id = 0, next = 0, events = 0, lost = 0, begins = 0,
                ends = 0, callbacks = 0;
  rt::Status drain(rt::Runtime &runtime, bool allow_loss = false) noexcept {
    std::array<rt::RuntimeTraceEvent, 32> buffer{};
    // Capacity is at most16384. No callback executes concurrently with drain.
    for (unsigned batch = 0; batch < 513; ++batch) {
      rt::RuntimeTraceReadResult read;
      auto s = runtime.read_trace(cursor, buffer, read);
      if (s != rt::Status::ok)
        return s;
      if (!runtime_id)
        runtime_id = read.metadata.runtime_id;
      if (!runtime_id || runtime_id != read.metadata.runtime_id ||
          read.events_read > buffer.size())
        return rt::Status::internal_error;
      lost += read.lost_events;
      if (read.lost_events && !allow_loss)
        return rt::Status::resource_exhausted;
      if (read.lost_events)
        next = read.first_sequence;
      for (std::size_t i = 0; i < read.events_read; ++i) {
        const auto &e = buffer[i];
        if (!next)
          next = e.sequence;
        if (e.sequence != next++ ||
            e.schema_version != rt::observability_schema_version ||
            e.record_size != sizeof(e))
          return rt::Status::internal_error;
        ++events;
        if (e.type == rt::RuntimeTraceEventType::step_begin)
          ++begins;
        if (e.type == rt::RuntimeTraceEventType::step_end)
          ++ends;
        if (e.type == rt::RuntimeTraceEventType::callback_end)
          ++callbacks;
      }
      if (!read.remaining_sequence_count)
        return rt::Status::ok;
    }
    return rt::Status::resource_exhausted;
  }
  bool metrics(rt::Runtime &runtime, std::uint64_t frames,
               std::uint64_t expected_callbacks) noexcept {
    rt::RuntimeMetricSnapshot m;
    if (runtime.metrics_snapshot(rt::RuntimeMetricWindow::cumulative, nullptr,
                                 m) != rt::Status::ok ||
        m.metadata.runtime_id != runtime_id ||
        m.sample_count != rt::runtime_metric_count)
      return false;
    bool completed = false, failures = false, calls = false;
    for (std::size_t i = 0; i < m.sample_count; ++i) {
      if (m.samples[i].id == rt::RuntimeMetricId::frames_completed)
        completed = m.samples[i].value == frames;
      if (m.samples[i].id == rt::RuntimeMetricId::frames_failed)
        failures = m.samples[i].value == 0;
      if (m.samples[i].id == rt::RuntimeMetricId::callbacks_completed)
        calls = m.samples[i].value == expected_callbacks;
    }
    return completed && failures && calls;
  }
};
} // namespace golden
