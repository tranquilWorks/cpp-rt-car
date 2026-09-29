#pragma once
#include "../golden_system/model.hpp"
#include <rt/runtime.hpp>
#include <type_traits>
namespace golden::cuda {
// Derived off-lane trace reader from the unchanged M25-05 kit.
struct Telemetry {
  rt::RuntimeTraceCursor cursor{};
  std::uint64_t runtime_id = 0, next = 0, events = 0, lost = 0, begins = 0,
                ends = 0, callbacks = 0;
  rt::RateTelemetryCursor rate_cursor{};
  rt::MixedRateActionCursor mixed_cursor{};
  rt::LiveControlActionCursor control_cursor{};
  std::array<std::uint64_t, 3> action_records{}, action_next{};
  std::uint64_t action_gaps = 0, deadline_failures = 0;
  std::size_t last_stream = 0;
  rt::Status expected_device_failure = rt::Status::ok;
  std::uint64_t device_failures = 0, device_terminals = 0;
  bool resume(rt::Runtime &runtime, const Telemetry &previous) noexcept {
    rt::RateTelemetryMetadata rate;
    rt::MixedRateActionMetadata mixed;
    rt::LiveControlActionMetadata control;
    if (runtime.rate_telemetry_metadata(rate) != rt::Status::ok ||
        runtime.mixed_rate_action_metadata(mixed) != rt::Status::ok ||
        runtime.live_control_action_metadata(control) != rt::Status::ok)
      return false;
    // Checkpoints restore sequence positions, not historical action bytes.
    // The old owner has already drained the prefix and failed attempt. Start
    // the new owner's cursors at its actual restored boundary, before any step.
    if (previous.action_gaps || rate.runtime_id == previous.runtime_id ||
        rate.runtime_id != mixed.runtime_id ||
        rate.runtime_id != control.runtime_id ||
        rate.next_sequence > previous.rate_cursor.next_sequence ||
        mixed.next_sequence > previous.mixed_cursor.next_sequence ||
        control.next_sequence > previous.control_cursor.next_sequence)
      return false;
    rate_cursor.runtime_id = rate.runtime_id;
    rate_cursor.next_sequence = rate.next_sequence;
    mixed_cursor.runtime_id = mixed.runtime_id;
    mixed_cursor.next_sequence = mixed.next_sequence;
    control_cursor.runtime_id = control.runtime_id;
    control_cursor.configuration_generation = control.configuration_generation;
    control_cursor.next_sequence = control.next_sequence;
    action_next = {rate.next_sequence, mixed.next_sequence,
                   control.next_sequence};
    return true;
  }
  template <typename Record, typename Cursor, typename Result, auto Read>
  rt::Status actions(rt::Runtime &runtime, Cursor &position,
                     std::size_t index) noexcept {
    last_stream = index;
    std::array<Record, 32> records{};
    for (std::size_t batch = 0; batch < 513; ++batch) {
      Result read;
      auto status = (runtime.*Read)(position, records, read);
      if (status != rt::Status::ok)
        return status;
      action_gaps += read.lost_records;
      if (read.lost_records || read.records_read > records.size())
        return rt::Status::resource_exhausted;
      for (std::size_t i = 0; i < read.records_read; ++i) {
        const auto &record = records[i];
        if (!action_next[index])
          action_next[index] = record.sequence;
        if (record.sequence != action_next[index]++ ||
            record.schema_version != 1 || record.record_size != sizeof(Record))
          return rt::Status::internal_error;
        if constexpr (std::is_same_v<Record, rt::RateActionRecord>) {
          if (record.status != static_cast<std::int32_t>(rt::Status::ok) &&
              record.status !=
                  static_cast<std::int32_t>(rt::Status::callback_failed)) {
            if (expected_device_failure == rt::Status::ok ||
                record.status !=
                    static_cast<std::int32_t>(expected_device_failure) ||
                record.action != rt::RateActionId::execute_on_time ||
                record.reason != rt::RateActionReason::callback_failure ||
                record.late || record.frame_index != 6 ||
                record.logical_release_ns != 6 * fixed::tick_ns)
              return rt::Status::internal_error;
            ++device_failures;
          }
          if (record.action == rt::RateActionId::fail) {
            if (!record.late ||
                record.reason != rt::RateActionReason::deadline_late ||
                record.frame_index != 6 ||
                record.logical_release_ns != 6 * fixed::tick_ns ||
                record.status !=
                    static_cast<std::int32_t>(rt::Status::callback_failed))
              return rt::Status::internal_error;
            ++deadline_failures;
          }
        }
        if constexpr (std::is_same_v<Record, rt::MixedRateActionRecord>) {
          if (record.action == rt::MixedRateActionId::device_terminal) {
            if (record.phase_index != 1)
              return rt::Status::internal_error;
            if (record.terminal_status !=
                static_cast<std::int32_t>(rt::Status::ok)) {
              const bool lost_device =
                  expected_device_failure == rt::Status::device_lost;
              if (expected_device_failure == rt::Status::ok ||
                  record.frame_index != 6 ||
                  record.terminal_status !=
                      static_cast<std::int32_t>(expected_device_failure) ||
                  record.reason !=
                      (lost_device
                           ? rt::MixedRateActionReason::lost
                           : rt::MixedRateActionReason::completion_error) ||
                  record.stage != (lost_device
                                       ? rt::MixedRateActionStage::quarantined
                                       : rt::MixedRateActionStage::terminal))
                return rt::Status::internal_error;
            }
            ++device_terminals;
          }
        }
        ++action_records[index];
      }
      if (!read.remaining_sequence_count)
        return rt::Status::ok;
    }
    return rt::Status::resource_exhausted;
  }
  rt::Status drain(rt::Runtime &runtime, bool allow_loss = false) noexcept {
    auto action_status =
        actions<rt::RateActionRecord, rt::RateTelemetryCursor,
                rt::RateTelemetryReadResult, &rt::Runtime::read_rate_actions>(
            runtime, rate_cursor, 0);
    if (action_status != rt::Status::ok)
      return action_status;
    action_status =
        actions<rt::MixedRateActionRecord, rt::MixedRateActionCursor,
                rt::MixedRateActionReadResult,
                &rt::Runtime::read_mixed_rate_actions>(runtime, mixed_cursor,
                                                       1);
    if (action_status != rt::Status::ok)
      return action_status;
    action_status =
        actions<rt::LiveControlActionRecord, rt::LiveControlActionCursor,
                rt::LiveControlActionReadResult,
                &rt::Runtime::read_live_control_actions>(runtime,
                                                         control_cursor, 2);
    if (action_status != rt::Status::ok)
      return action_status;
    last_stream = 3;
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
} // namespace golden::cuda
