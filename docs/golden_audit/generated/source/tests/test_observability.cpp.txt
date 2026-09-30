#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <limits>
#include <ostream>
#include <sstream>
#include <streambuf>
#include <string_view>
#include <thread>
#include <vector>

#include <rt/observability_export.hpp>
#include <rt/runtime.hpp>

#include "rt/src/telemetry.hpp"

namespace {

class FakeClock final : public rt::RuntimeClock {
public:
    std::uint64_t now_ns() noexcept override {
        return now.fetch_add(10, std::memory_order_relaxed);
    }

    rt::Status sleep_until_ns(
        std::uint64_t absolute_ns) noexcept override {
        auto current = now.load(std::memory_order_relaxed);
        while (current < absolute_ns &&
               !now.compare_exchange_weak(
                   current,
                   absolute_ns,
                   std::memory_order_relaxed,
                   std::memory_order_relaxed)) {
        }
        return rt::Status::ok;
    }

    bool supports_absolute_sleep() const noexcept override {
        return true;
    }

    std::atomic<std::uint64_t> now{100};
};

class FailingBuffer final : public std::streambuf {
protected:
    std::streamsize xsputn(
        const char_type*,
        std::streamsize) override {
        return 0;
    }

    int_type overflow(int_type) override {
        return traits_type::eof();
    }
};

rt::CallbackResult count_callback(
    void* opaque,
    const rt::CallbackContext&) {
    auto& count = *static_cast<std::atomic<std::uint64_t>*>(opaque);
    count.fetch_add(1, std::memory_order_relaxed);
    return rt::CallbackResult::ok;
}

rt::RuntimeConfig test_config(std::string_view workload) {
    rt::RuntimeConfig config;
    config.callback_capacity = 2;
    config.scratch_bytes = 64;
    config.trace_capacity = 64;
    config.worker_count = 2;
    config.executor_queue_capacity = 16;
    config.task_scratch_bytes = 64;
    config.task_scratch_slots = 16;
    config.memory_budget_bytes = 1024 * 1024;
    EXPECT_EQ(
        rt::set_runtime_config_value(
            config,
            "workload_id",
            workload),
        rt::Status::ok);
    return config;
}

void prepare_runtime(
    rt::Runtime& runtime,
    const rt::RuntimeConfig& config,
    std::atomic<std::uint64_t>& count) {
    ASSERT_EQ(runtime.configure(config), rt::Status::ok);
    ASSERT_EQ(
        runtime.register_callback(
            {"observe.phase", &count_callback, &count}),
        rt::Status::ok);
    ASSERT_EQ(runtime.finalize(), rt::Status::ok);
    ASSERT_EQ(runtime.start(), rt::Status::ok);
}

void run_frames(
    rt::Runtime& runtime,
    std::uint64_t first,
    std::size_t count) {
    for (std::size_t offset = 0; offset < count; ++offset) {
        ASSERT_EQ(
            runtime.step(
                {first + offset, std::chrono::milliseconds(1), {}}),
            rt::Status::ok);
    }
}

std::uint64_t metric_value(
    const rt::RuntimeMetricSnapshot& snapshot,
    rt::RuntimeMetricId id) {
    const auto index = static_cast<std::size_t>(id);
    EXPECT_LT(index, snapshot.sample_count);
    EXPECT_EQ(snapshot.samples[index].id, id);
    return snapshot.samples[index].value;
}

} // namespace

TEST(Observability, MetadataCarriesStableProvenance) {
    FakeClock first_clock;
    FakeClock second_clock;
    std::atomic<std::uint64_t> first_count{0};
    std::atomic<std::uint64_t> second_count{0};
    auto config = test_config("physics.swarm");

    rt::Runtime first(first_clock);
    rt::Runtime second(second_clock);
    prepare_runtime(first, config, first_count);
    prepare_runtime(second, config, second_count);

    rt::ObservabilityMetadata first_metadata;
    rt::ObservabilityMetadata second_metadata;
    ASSERT_EQ(
        first.observability_metadata(first_metadata),
        rt::Status::ok);
    ASSERT_EQ(
        second.observability_metadata(second_metadata),
        rt::Status::ok);
    EXPECT_EQ(
        first_metadata.schema_version,
        rt::observability_schema_version);
    EXPECT_EQ(
        first_metadata.trace_event_size,
        sizeof(rt::RuntimeTraceEvent));
    EXPECT_EQ(
        first_metadata.metric_count,
        rt::runtime_metric_count);
    EXPECT_EQ(
        std::string_view(first_metadata.workload_id.data()),
        "physics.swarm");
    EXPECT_FALSE(
        std::string_view(first_metadata.build_id.data()).empty());
    EXPECT_EQ(
        first_metadata.config_id,
        second_metadata.config_id);
    EXPECT_NE(
        first_metadata.runtime_id,
        second_metadata.runtime_id);

    ASSERT_EQ(first.stop(), rt::Status::ok);
    ASSERT_EQ(second.stop(), rt::Status::ok);
}

TEST(Observability, IntervalWindowsPartitionCumulativeCounters) {
    FakeClock clock;
    std::atomic<std::uint64_t> count{0};
    rt::Runtime runtime(clock);
    prepare_runtime(runtime, test_config("window.partition"), count);

    rt::RuntimeMetricCursor cursor;
    rt::RuntimeMetricSnapshot initial;
    ASSERT_EQ(
        runtime.metrics_snapshot(
            rt::RuntimeMetricWindow::interval,
            &cursor,
            initial),
        rt::Status::ok);
    run_frames(runtime, 0, 2);
    rt::RuntimeMetricSnapshot first;
    ASSERT_EQ(
        runtime.metrics_snapshot(
            rt::RuntimeMetricWindow::interval,
            &cursor,
            first),
        rt::Status::ok);
    run_frames(runtime, 2, 3);
    rt::RuntimeMetricSnapshot second;
    ASSERT_EQ(
        runtime.metrics_snapshot(
            rt::RuntimeMetricWindow::interval,
            &cursor,
            second),
        rt::Status::ok);

    rt::RuntimeMetricSnapshot cumulative;
    ASSERT_EQ(
        runtime.metrics_snapshot(
            rt::RuntimeMetricWindow::cumulative,
            nullptr,
            cumulative),
        rt::Status::ok);
    EXPECT_EQ(first.window_start_ns, initial.window_end_ns);
    EXPECT_EQ(second.window_start_ns, first.window_end_ns);
    EXPECT_EQ(
        metric_value(
            cumulative,
            rt::RuntimeMetricId::frames_completed),
        5u);
    EXPECT_EQ(
        metric_value(
            cumulative,
            rt::RuntimeMetricId::callbacks_completed),
        5u);

    for (std::size_t index = 0;
         index < rt::runtime_metric_count;
         ++index) {
        rt::RuntimeMetricDefinition definition;
        ASSERT_TRUE(
            rt::runtime_metric_definition(index, definition));
        const auto total = cumulative.samples[index].value;
        if (definition.kind == rt::RuntimeMetricKind::counter) {
            EXPECT_EQ(
                initial.samples[index].value +
                    first.samples[index].value +
                    second.samples[index].value,
                total)
                << definition.name;
        } else {
            EXPECT_EQ(initial.samples[index].value, total);
            EXPECT_EQ(first.samples[index].value, total);
            EXPECT_EQ(second.samples[index].value, total);
        }
    }

    ASSERT_EQ(runtime.stop(), rt::Status::ok);
}

TEST(Observability, TraceCursorReportsOverwriteLossExactly) {
    FakeClock clock;
    std::atomic<std::uint64_t> count{0};
    auto config = test_config("trace.loss");
    config.trace_capacity = 4;
    rt::Runtime runtime(clock);
    prepare_runtime(runtime, config, count);
    run_frames(runtime, 0, 1);

    rt::RuntimeTraceCursor cursor;
    std::array<rt::RuntimeTraceEvent, 2> first_events{};
    rt::RuntimeTraceReadResult first;
    ASSERT_EQ(
        runtime.read_trace(cursor, first_events, first),
        rt::Status::ok);
    ASSERT_EQ(first.events_read, first_events.size());
    EXPECT_EQ(first.lost_events, 0u);
    EXPECT_LT(
        first_events[0].sequence,
        first_events[1].sequence);
    const auto previous_next = cursor.next_sequence;

    run_frames(runtime, 1, 2);
    const auto end_before_read =
        runtime.trace_event_count();
    EXPECT_LE(end_before_read, config.trace_capacity);

    std::array<rt::RuntimeTraceEvent, 4> later_events{};
    rt::RuntimeTraceReadResult later;
    ASSERT_EQ(
        runtime.read_trace(cursor, later_events, later),
        rt::Status::ok);
    ASSERT_GT(later.events_read, 0u);
    EXPECT_EQ(
        later.lost_events,
        later.first_sequence - previous_next);
    for (std::size_t index = 0;
         index < later.events_read;
         ++index) {
        EXPECT_EQ(
            later_events[index].schema_version,
            rt::observability_schema_version);
        EXPECT_EQ(
            later_events[index].record_size,
            sizeof(rt::RuntimeTraceEvent));
        if (index != 0) {
            EXPECT_LT(
                later_events[index - 1].sequence,
                later_events[index].sequence);
        }
    }

    ASSERT_EQ(runtime.stop(), rt::Status::ok);
}

TEST(Observability, RuntimeInstancesRejectForeignCursors) {
    FakeClock first_clock;
    FakeClock second_clock;
    std::atomic<std::uint64_t> first_count{0};
    std::atomic<std::uint64_t> second_count{0};
    rt::Runtime first(first_clock);
    rt::Runtime second(second_clock);
    const auto config = test_config("cursor.isolation");
    prepare_runtime(first, config, first_count);
    prepare_runtime(second, config, second_count);

    rt::RuntimeMetricCursor metric_cursor;
    rt::RuntimeMetricSnapshot metrics;
    ASSERT_EQ(
        first.metrics_snapshot(
            rt::RuntimeMetricWindow::interval,
            &metric_cursor,
            metrics),
        rt::Status::ok);
    EXPECT_EQ(
        second.metrics_snapshot(
            rt::RuntimeMetricWindow::interval,
            &metric_cursor,
            metrics),
        rt::Status::invalid_argument);

    rt::RuntimeTraceCursor trace_cursor;
    std::array<rt::RuntimeTraceEvent, 8> events{};
    rt::RuntimeTraceReadResult trace_result;
    ASSERT_EQ(
        first.read_trace(trace_cursor, events, trace_result),
        rt::Status::ok);
    EXPECT_EQ(
        second.read_trace(trace_cursor, events, trace_result),
        rt::Status::invalid_argument);

    ASSERT_EQ(first.stop(), rt::Status::ok);
    ASSERT_EQ(second.stop(), rt::Status::ok);
}

TEST(Observability, JsonExportIncludesSchemaAndIdentifiers) {
    FakeClock clock;
    std::atomic<std::uint64_t> count{0};
    rt::Runtime runtime(clock);
    prepare_runtime(runtime, test_config("export.json"), count);
    run_frames(runtime, 0, 1);

    std::ostringstream output;
    ASSERT_EQ(
        rt::write_observability_json(runtime, output),
        rt::Status::ok);
    const auto json = output.str();
    EXPECT_NE(
        json.find("\"schema_version\":2"),
        std::string::npos);
    EXPECT_NE(
        json.find("\"trace_event_size\":64"),
        std::string::npos);
    EXPECT_NE(
        json.find("\"metric_count\":32"),
        std::string::npos);
    EXPECT_NE(json.find("\"build_id\":"), std::string::npos);
    EXPECT_NE(json.find("\"config_id\":"), std::string::npos);
    EXPECT_NE(json.find("\"runtime_id\":"), std::string::npos);
    EXPECT_NE(
        json.find("\"workload_id\":\"export.json\""),
        std::string::npos);
    EXPECT_NE(
        json.find("\"runtime.frames_completed\""),
        std::string::npos);
    EXPECT_NE(
        json.find("\"frame.end\""),
        std::string::npos);

    ASSERT_EQ(runtime.stop(), rt::Status::ok);
}

TEST(Observability, RejectsMalformedFreshCursors) {
    FakeClock clock;
    std::atomic<std::uint64_t> count{0};
    rt::Runtime runtime(clock);
    prepare_runtime(runtime, test_config("cursor.validation"), count);

    rt::RuntimeMetricSnapshot metrics;
    rt::RuntimeMetricCursor metric_cursor;
    metric_cursor.window_end_ns = 1;
    EXPECT_EQ(
        runtime.metrics_snapshot(
            rt::RuntimeMetricWindow::interval,
            &metric_cursor,
            metrics),
        rt::Status::invalid_argument);

    metric_cursor = {};
    metric_cursor.counters[static_cast<std::size_t>(
        rt::RuntimeMetricId::frames_started)] = 1;
    EXPECT_EQ(
        runtime.metrics_snapshot(
            rt::RuntimeMetricWindow::interval,
            &metric_cursor,
            metrics),
        rt::Status::invalid_argument);

    metric_cursor = {};
    ASSERT_EQ(
        runtime.metrics_snapshot(
            rt::RuntimeMetricWindow::interval,
            &metric_cursor,
            metrics),
        rt::Status::ok);
    metric_cursor.window_end_ns =
        std::numeric_limits<std::uint64_t>::max();
    EXPECT_EQ(
        runtime.metrics_snapshot(
            rt::RuntimeMetricWindow::interval,
            &metric_cursor,
            metrics),
        rt::Status::invalid_argument);

    rt::RuntimeTraceCursor trace_cursor;
    trace_cursor.next_sequence = 1;
    std::array<rt::RuntimeTraceEvent, 4> events{};
    rt::RuntimeTraceReadResult trace_result;
    EXPECT_EQ(
        runtime.read_trace(
            trace_cursor,
            events,
            trace_result),
        rt::Status::invalid_argument);

    ASSERT_EQ(runtime.stop(), rt::Status::ok);
}

TEST(Observability, JsonExportCommitsCursorsOnlyAfterSuccess) {
    FakeClock clock;
    std::atomic<std::uint64_t> count{0};
    rt::Runtime runtime(clock);
    prepare_runtime(runtime, test_config("export.transaction"), count);
    run_frames(runtime, 0, 1);

    std::ostringstream missing_cursor_output;
    EXPECT_EQ(
        rt::write_observability_json(
            runtime,
            missing_cursor_output,
            rt::RuntimeMetricWindow::interval),
        rt::Status::invalid_argument);

    rt::RuntimeMetricCursor metric_cursor;
    rt::RuntimeTraceCursor trace_cursor;
    FailingBuffer buffer;
    std::ostream failing_output(&buffer);
    EXPECT_EQ(
        rt::write_observability_json(
            runtime,
            failing_output,
            rt::RuntimeMetricWindow::interval,
            &metric_cursor,
            &trace_cursor),
        rt::Status::internal_error);
    EXPECT_EQ(metric_cursor.runtime_id, 0u);
    EXPECT_EQ(metric_cursor.window_end_ns, 0u);
    EXPECT_EQ(trace_cursor.runtime_id, 0u);
    EXPECT_EQ(trace_cursor.next_sequence, 0u);

    std::ostringstream successful_output;
    ASSERT_EQ(
        rt::write_observability_json(
            runtime,
            successful_output,
            rt::RuntimeMetricWindow::interval,
            &metric_cursor,
            &trace_cursor),
        rt::Status::ok);
    EXPECT_NE(metric_cursor.runtime_id, 0u);
    EXPECT_NE(trace_cursor.runtime_id, 0u);
    EXPECT_NE(
        successful_output.str().find("\"runtime_id\":"),
        std::string::npos);

    ASSERT_EQ(runtime.stop(), rt::Status::ok);
}

TEST(Observability, ContendedRingDropsInsteadOfWaiting) {
    constexpr std::size_t thread_count = 8;
    constexpr std::size_t events_per_thread = 2'000;
    rt::detail::TelemetryRing ring(1);
    std::vector<std::thread> threads;
    threads.reserve(thread_count);
    for (std::size_t thread = 0;
         thread < thread_count;
         ++thread) {
        threads.emplace_back([&, thread] {
            for (std::size_t event_index = 0;
                 event_index < events_per_thread;
                 ++event_index) {
                rt::RuntimeTraceEvent event;
                event.type =
                    rt::RuntimeTraceEventType::callback_begin;
                event.producer =
                    rt::RuntimeTraceProducer::worker;
                event.worker_index =
                    static_cast<std::uint32_t>(thread);
                (void)ring.emit(event);
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }

    const auto attempts =
        static_cast<std::uint64_t>(
            thread_count * events_per_thread);
    EXPECT_EQ(ring.next_sequence(), attempts);
    EXPECT_EQ(ring.emitted() + ring.dropped(), attempts);
    EXPECT_LE(ring.retained_count(), 1u);
    EXPECT_LE(ring.overwritten(), ring.emitted());
}
