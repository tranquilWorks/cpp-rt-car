#define RTFW_COMBINED_SAMPLE_NO_MAIN 1
#include "../samples/cpu_gpu_fpga_cpu.cpp"

#include <iostream>

namespace {

int failures = 0;

#define CHECK_TRUE(expression)                                                \
    do {                                                                      \
        if (!(expression)) {                                                  \
            std::cerr << __func__ << ':' << __LINE__                         \
                      << " check failed: " #expression << '\n';             \
            ++failures;                                                       \
            return false;                                                     \
        }                                                                     \
    } while (false)

using rtfw_combined_sample::FailureMode;
using rtfw_combined_sample::Scenario;

bool configure_and_start(Scenario& scenario) {
    CHECK_TRUE(scenario.configure() == rt::Status::ok);
    CHECK_TRUE(scenario.runtime.state() == rt::RuntimeState::configuring);
    CHECK_TRUE(scenario.finalize() == rt::Status::ok);
    CHECK_TRUE(scenario.runtime.state() == rt::RuntimeState::finalized);
    CHECK_TRUE(scenario.start() == rt::Status::ok);
    CHECK_TRUE(scenario.runtime.state() == rt::RuntimeState::running);
    return true;
}

bool wait_for_flag(const std::atomic<bool>& flag) {
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(2);
    while (!flag.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    return flag.load(std::memory_order_acquire);
}

bool success_and_fixed_resources() {
    Scenario scenario;
    CHECK_TRUE(configure_and_start(scenario));
    CHECK_TRUE(scenario.run(rtfw_combined_sample::frame_count) ==
               rt::Status::ok);
    CHECK_TRUE(scenario.success_contract());
    CHECK_TRUE(scenario.measured_allocations == 0);
    CHECK_TRUE(scenario.cuda_driver.host_registrations.load() == 1);
    CHECK_TRUE(scenario.cuda_driver.host_unregistrations.load() == 0);
    CHECK_TRUE(scenario.cuda_driver.frees.load() == 0);
    CHECK_TRUE(scenario.stop() == rt::Status::ok);
    CHECK_TRUE(scenario.runtime.state() == rt::RuntimeState::stopped);
    CHECK_TRUE(scenario.cuda_driver.host_unregistrations.load() == 1);
    CHECK_TRUE(scenario.cuda_driver.frees.load() == 1);
    CHECK_TRUE(scenario.xdma_driver.shutdown_calls.load() == 1);
    return true;
}

bool cuda_failure_suppresses_every_downstream_stage_and_recovers() {
    {
        Scenario failed(FailureMode::cuda_graph, 2);
        CHECK_TRUE(configure_and_start(failed));
        CHECK_TRUE(failed.run(1) == rt::Status::device_reset_required);
        CHECK_TRUE(failed.measured_allocations == 0);
        CHECK_TRUE(failed.completed_frames == 0);
        CHECK_TRUE(failed.prepare_calls.load() == 1);
        CHECK_TRUE(failed.cuda_provider_calls.load() == 1);
        CHECK_TRUE(failed.cuda_driver.uploads.load() == 1);
        CHECK_TRUE(failed.cuda_driver.graph_launches.load() == 1);
        CHECK_TRUE(failed.cuda_driver.downloads.load() == 0);
        CHECK_TRUE(failed.cuda_driver.event_records.load() == 0);
        CHECK_TRUE(failed.bridge_calls.load() == 0);
        CHECK_TRUE(failed.xdma_provider_calls.load() == 0);
        CHECK_TRUE(failed.xdma_driver.h2c_calls.load() == 0);
        CHECK_TRUE(failed.xdma_driver.control_writes.load() == 0);
        CHECK_TRUE(failed.xdma_driver.event_waits.load() == 0);
        CHECK_TRUE(failed.xdma_driver.c2h_calls.load() == 0);
        CHECK_TRUE(failed.validate_calls.load() == 0);
        CHECK_TRUE(failed.timeline_observations[0].cuda_accepted == 1);
        CHECK_TRUE(failed.timeline_observations[0].cuda_completed == 0);
        CHECK_TRUE(failed.timeline_observations[0].xdma_accepted == 0);
        CHECK_TRUE(failed.timeline_observations[0].xdma_completed == 0);
        constexpr std::array expected{'P', 'U', 'H', 'G'};
        CHECK_TRUE(failed.operations.matches(expected));
        CHECK_TRUE(failed.runtime.reset_device(failed.cuda_backend_handle) ==
                   rt::Status::ok);
        CHECK_TRUE(failed.cuda_driver.stream_synchronizations.load() == 1);
        CHECK_TRUE(failed.stop() == rt::Status::ok);
        CHECK_TRUE(failed.cuda_driver.stream_synchronizations.load() == 1);
        CHECK_TRUE(failed.cuda_driver.host_unregistrations.load() == 1);
        CHECK_TRUE(failed.cuda_driver.frees.load() == 1);
    }
    Scenario recovered(FailureMode::none, 3);
    CHECK_TRUE(configure_and_start(recovered));
    CHECK_TRUE(recovered.run(rtfw_combined_sample::frame_count) ==
               rt::Status::ok);
    CHECK_TRUE(recovered.success_contract());
    CHECK_TRUE(recovered.stop() == rt::Status::ok);
    return true;
}

bool isolated_event_timeout_has_no_xdma_completion_and_other_fixture_runs() {
    Scenario failed(FailureMode::xdma_event_timeout, 4);
    Scenario independent(FailureMode::none, 5);
    CHECK_TRUE(failed.configure() == rt::Status::ok);
    CHECK_TRUE(independent.configure() == rt::Status::ok);
    CHECK_TRUE(failed.cuda_backend_handle != independent.cuda_backend_handle);
    CHECK_TRUE(failed.xdma_backend_handle != independent.xdma_backend_handle);
    CHECK_TRUE(failed.cuda_timeline != independent.cuda_timeline);
    CHECK_TRUE(failed.cuda_driver.graph != independent.cuda_driver.graph);
    static_assert(rtfw_combined_sample::graph_id == 7);
    CHECK_TRUE(failed.cuda_stage.data() != independent.cuda_stage.data());
    CHECK_TRUE(failed.xdma_stage.data() != independent.xdma_stage.data());
    rt::DeviceTimelineHandle foreign_timeline;
    CHECK_TRUE(failed.runtime.register_device_timeline(
                   {"combined.foreign.timeline",
                    independent.cuda_backend_handle, 0},
                   foreign_timeline) == rt::Status::invalid_handle);
    CHECK_TRUE(!foreign_timeline.valid());
    rt::DeviceTimelineInfo foreign_info;
    CHECK_TRUE(!failed.runtime.device_timeline_at(
        independent.cuda_backend_handle, 0, foreign_info));

    CHECK_TRUE(failed.finalize() == rt::Status::ok);
    CHECK_TRUE(failed.start() == rt::Status::ok);
    CHECK_TRUE(failed.run(1) == rt::Status::device_timeout);
    CHECK_TRUE(failed.measured_allocations == 0);
    CHECK_TRUE(failed.completed_frames == 0);
    CHECK_TRUE(failed.prepare_calls.load() == 1);
    CHECK_TRUE(failed.cuda_provider_calls.load() == 1);
    CHECK_TRUE(failed.cuda_driver.uploads.load() == 1);
    CHECK_TRUE(failed.cuda_driver.graph_launches.load() == 1);
    CHECK_TRUE(failed.cuda_driver.downloads.load() == 1);
    CHECK_TRUE(failed.cuda_driver.event_records.load() == 1);
    CHECK_TRUE(failed.bridge_calls.load() == 1);
    CHECK_TRUE(failed.xdma_provider_calls.load() == 1);
    CHECK_TRUE(failed.xdma_driver.h2c_calls.load() == 1);
    CHECK_TRUE(failed.xdma_driver.control_writes.load() == 1);
    CHECK_TRUE(failed.xdma_driver.event_waits.load() == 1);
    CHECK_TRUE(failed.xdma_driver.event_entered.load());
    CHECK_TRUE(failed.xdma_driver.stop_requests.load() >= 1);
    CHECK_TRUE(wait_for_flag(failed.xdma_driver.event_exited));
    CHECK_TRUE(failed.xdma_driver.c2h_calls.load() == 0);
    CHECK_TRUE(failed.validate_calls.load() == 0);
    CHECK_TRUE(failed.timeline_observations[0].cuda_accepted == 1);
    CHECK_TRUE(failed.timeline_observations[0].cuda_completed == 1);
    CHECK_TRUE(failed.timeline_observations[0].xdma_accepted == 1);
    CHECK_TRUE(failed.timeline_observations[0].xdma_completed == 0);
    constexpr std::array expected{
        'P', 'U', 'H', 'G', 'D', 'E', 'B', 'X', 'h', 'w', 'e'};
    CHECK_TRUE(failed.operations.matches(expected));

    CHECK_TRUE(independent.prepare_calls.load() == 0);
    CHECK_TRUE(independent.cuda_driver.uploads.load() == 0);
    CHECK_TRUE(independent.xdma_driver.h2c_calls.load() == 0);
    CHECK_TRUE(independent.operations.count.load() == 0);
    rt::DeviceTimelineInfo independent_cuda;
    rt::DeviceTimelineInfo independent_xdma;
    CHECK_TRUE(independent.runtime.device_timeline_at(
        independent.cuda_backend_handle, 0, independent_cuda));
    CHECK_TRUE(independent.runtime.device_timeline_at(
        independent.xdma_backend_handle, 0, independent_xdma));
    CHECK_TRUE(independent_cuda.last_accepted_value == 0);
    CHECK_TRUE(independent_cuda.completed_value == 0);
    CHECK_TRUE(independent_xdma.last_accepted_value == 0);
    CHECK_TRUE(independent_xdma.completed_value == 0);
    failed.failure = FailureMode::bridge_recovery;
    rt::DeviceHealth xdma_health;
    std::uint64_t recovery_frame = 2;
    bool stale_completion_reaped = false;
    for (std::size_t attempt = 0; attempt < 16; ++attempt) {
        CHECK_TRUE(failed.run_from(recovery_frame, 1) ==
                   rt::Status::callback_failed);
        CHECK_TRUE(failed.runtime.device_health(
                       failed.xdma_backend_handle, xdma_health) ==
                   rt::Status::ok);
        rt::DeviceTimelineInfo failed_xdma;
        CHECK_TRUE(failed.runtime.device_timeline_at(
            failed.xdma_backend_handle, 0, failed_xdma));
        CHECK_TRUE(failed_xdma.last_accepted_value == 1);
        CHECK_TRUE(failed_xdma.completed_value == 0);
        ++recovery_frame;
        if (xdma_health.outstanding == 0) {
            stale_completion_reaped = true;
            break;
        }
    }
    CHECK_TRUE(stale_completion_reaped);
    CHECK_TRUE(failed.xdma_driver.h2c_calls.load() == 1);
    CHECK_TRUE(failed.xdma_driver.control_writes.load() == 1);
    CHECK_TRUE(failed.xdma_driver.event_waits.load() == 1);
    CHECK_TRUE(failed.xdma_driver.c2h_calls.load() == 0);
    rt::DeviceTimelineInfo failed_xdma_before_reuse;
    CHECK_TRUE(failed.runtime.device_timeline_at(
        failed.xdma_backend_handle, 0, failed_xdma_before_reuse));
    CHECK_TRUE(failed_xdma_before_reuse.last_accepted_value == 1);
    CHECK_TRUE(failed_xdma_before_reuse.completed_value == 0);

    failed.failure = FailureMode::none;
    failed.xdma_driver.stop_requests.store(0, std::memory_order_release);
    failed.xdma_driver.event_entered.store(false, std::memory_order_release);
    failed.xdma_driver.event_exited.store(false, std::memory_order_release);
    CHECK_TRUE(failed.run_from(recovery_frame, 1) == rt::Status::ok);
    CHECK_TRUE(failed.measured_allocations == 0);
    CHECK_TRUE(failed.timeline_observations[1].cuda_accepted ==
               recovery_frame);
    CHECK_TRUE(failed.timeline_observations[1].cuda_completed ==
               recovery_frame);
    CHECK_TRUE(failed.timeline_observations[1].xdma_accepted ==
               recovery_frame);
    CHECK_TRUE(failed.timeline_observations[1].xdma_completed ==
               recovery_frame);
    CHECK_TRUE(failed.stop() == rt::Status::ok);
    CHECK_TRUE(independent.xdma_driver.stop_requests.load() == 0);
    CHECK_TRUE(independent.xdma_driver.shutdown_calls.load() == 0);

    CHECK_TRUE(independent.finalize() == rt::Status::ok);
    CHECK_TRUE(independent.start() == rt::Status::ok);
    CHECK_TRUE(independent.run(rtfw_combined_sample::frame_count) ==
               rt::Status::ok);
    CHECK_TRUE(independent.success_contract());
    CHECK_TRUE(independent.stop() == rt::Status::ok);
    return true;
}

bool malformed_provider_outputs_fail_before_native_driver_calls() {
    for (const auto mode : {FailureMode::malformed_cuda_signal,
                            FailureMode::malformed_cuda_timeout}) {
        Scenario scenario(mode, mode == FailureMode::malformed_cuda_signal
                                    ? 6u
                                    : 7u);
        CHECK_TRUE(configure_and_start(scenario));
        CHECK_TRUE(scenario.run(1) == rt::Status::invalid_argument);
        CHECK_TRUE(scenario.measured_allocations == 0);
        CHECK_TRUE(scenario.prepare_calls.load() == 1);
        CHECK_TRUE(scenario.cuda_provider_calls.load() == 1);
        CHECK_TRUE(scenario.cuda_driver.uploads.load() == 0);
        CHECK_TRUE(scenario.cuda_driver.graph_launches.load() == 0);
        CHECK_TRUE(scenario.bridge_calls.load() == 0);
        CHECK_TRUE(scenario.xdma_provider_calls.load() == 0);
        CHECK_TRUE(scenario.validate_calls.load() == 0);
        CHECK_TRUE(scenario.timeline_observations[0].cuda_accepted == 0);
        CHECK_TRUE(scenario.timeline_observations[0].cuda_completed == 0);
        CHECK_TRUE(scenario.timeline_observations[0].xdma_accepted == 0);
        CHECK_TRUE(scenario.timeline_observations[0].xdma_completed == 0);
        constexpr std::array expected{'P', 'U'};
        CHECK_TRUE(scenario.operations.matches(expected));
        CHECK_TRUE(scenario.stop() == rt::Status::ok);
    }
    return true;
}

bool checked_stop_retries_only_unresolved_cleanup() {
    Scenario scenario(FailureMode::none, 8);
    CHECK_TRUE(configure_and_start(scenario));
    CHECK_TRUE(scenario.run(1) == rt::Status::ok);
    CHECK_TRUE(scenario.measured_allocations == 0);
    scenario.xdma_driver.fail_shutdown_once.store(true,
                                                  std::memory_order_release);
    CHECK_TRUE(scenario.stop() == rt::Status::device_reset_required);
    CHECK_TRUE(scenario.runtime.state() == rt::RuntimeState::running);
    CHECK_TRUE(scenario.xdma_driver.shutdown_calls.load() == 1);
    const auto cuda_unregistrations =
        scenario.cuda_driver.host_unregistrations.load();
    const auto cuda_frees = scenario.cuda_driver.frees.load();
    CHECK_TRUE(cuda_unregistrations == 1);
    CHECK_TRUE(cuda_frees == 1);
    CHECK_TRUE(scenario.stop() == rt::Status::ok);
    CHECK_TRUE(scenario.runtime.state() == rt::RuntimeState::stopped);
    CHECK_TRUE(scenario.xdma_driver.shutdown_calls.load() == 2);
    CHECK_TRUE(scenario.cuda_driver.host_unregistrations.load() ==
               cuda_unregistrations);
    CHECK_TRUE(scenario.cuda_driver.frees.load() == cuda_frees);
    return true;
}

} // namespace

int main() {
    (void)success_and_fixed_resources();
    (void)cuda_failure_suppresses_every_downstream_stage_and_recovers();
    (void)isolated_event_timeout_has_no_xdma_completion_and_other_fixture_runs();
    (void)malformed_provider_outputs_fail_before_native_driver_calls();
    (void)checked_stop_retries_only_unresolved_cleanup();
    if (failures != 0) {
        std::cerr << "m17_cpu_gpu_fpga_cpu_sample failures=" << failures
                  << '\n';
        return 1;
    }
    std::cout << "m17_cpu_gpu_fpga_cpu_sample passed"
                 " evidence=simulated_protocol physical_hardware=false"
                 " direct_peer_dma=false\n";
    return 0;
}
