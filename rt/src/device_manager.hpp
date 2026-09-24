#pragma once

#include "executor.hpp"
#include "command_batch.hpp"
#include "hal_v2.hpp"
#include "heterogeneous_memory.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <rt/device.hpp>
#include <rt/runtime.hpp>

namespace rt::detail {

// One submission-lane selection/park operation. The selector and returned
// pointer borrow lane-owned fixed storage; this helper allocates nothing.
template <typename Select>
auto select_device_submission_or_wait(
    std::atomic<std::uint64_t>& wake_sequence,
    const std::atomic<bool>& stopping,
    Select&& select) noexcept -> decltype(select()) {
    // Capture before scanning: a publication after an empty scan changes the
    // value passed to wait, so its notification cannot be consumed too early.
    const auto observed = wake_sequence.load(std::memory_order_acquire);
    auto* selected = select();
    if (!selected) {
        if (!stopping.load(std::memory_order_acquire)) {
            wake_sequence.wait(observed, std::memory_order_relaxed);
        }
    }
    return selected;
}

struct DeviceBackendSpec {
    std::string name;
    HalV2BackendApi api{};
    HalV2Capabilities capabilities{};
    HalBackendKind kind = HalBackendKind::adapted_device_abi_v1;
    DeviceV1CompatibilityAdapter* v1_adapter = nullptr;
    HeterogeneousMemoryState *memory_state = nullptr;
    CommandTimelineExtensionState *command_state = nullptr;
};

struct DeviceTimelineSpec {
    std::array<char, hal_v2_identifier_capacity> name{};
    std::uint32_t backend_index = 0;
    std::uint64_t initial_value = 0;
};

struct DeviceBufferSpec {
    std::array<char, RTFW_DEVICE_IDENTIFIER_CAPACITY> name{};
    std::uint32_t backend_index = 0;
    std::span<std::byte> storage{};
    rtfw_device_buffer_flags flags = 0;
    std::uint64_t bytes = 0;
    std::uint64_t domain_identity = 0;
    std::uint32_t ownership =
        static_cast<std::uint32_t>(HalV2MemoryOwnership::borrowed_host);
    std::uint32_t coherency =
        static_cast<std::uint32_t>(HalV2MemoryCoherency::host_coherent);
    std::uint32_t synchronization = hal_v2_memory_sync_none;
    bool heterogeneous = false;
    HalV2OpaqueHandle opaque_handle{};
};

enum class DeviceEventKind : std::uint8_t {
    submitted,
    completed,
    reset,
};

struct DeviceEvent {
    DeviceEventKind kind = DeviceEventKind::submitted;
    Status status = Status::ok;
    std::size_t backend_index = 0;
    std::size_t phase_index = 0;
    std::uint64_t frame_index = 0;
    std::uint64_t submission_id = 0;
    std::uint64_t timestamp_ns = 0;
    RuntimeTraceProducer producer =
        RuntimeTraceProducer::device_service;
    std::size_t worker_index =
        std::numeric_limits<std::size_t>::max();
};

using DeviceEventObserver = void (*)(
    void* user_data,
    const DeviceEvent& event) noexcept;

struct DeviceManagerStats {
    std::uint64_t submissions = 0;
    std::uint64_t completions = 0;
    std::uint64_t failures = 0;
    std::uint64_t queue_rejections = 0;
    std::uint64_t timeouts = 0;
    std::uint64_t losses = 0;
    std::uint64_t resets = 0;
    std::uint64_t service_polls = 0;
    std::uint64_t outstanding = 0;
    std::uint64_t service_starts = 0;
};

struct DeviceRateReleaseIdentity {
    std::size_t reference_index = std::numeric_limits<std::size_t>::max();
    RateDomainHandle domain{};
    PhaseHandle phase{};
    std::uint64_t supercycle_cycle = 0;
    std::uint64_t domain_release_sequence = 0;
    std::uint32_t substep_ordinal = 0;
    std::uint64_t logical_release_ns = 0;
    std::uint64_t nominal_release_ns = 0;
    std::uint64_t absolute_deadline_ns = 0;
    std::uint64_t completion_budget_ns = 0;
};

struct DeviceRateTicket {
    std::size_t slot_index = std::numeric_limits<std::size_t>::max();
    std::uint64_t batch_id = 0;
    DeviceRateReleaseIdentity identity{};

    [[nodiscard]] bool valid() const noexcept {
        return slot_index != std::numeric_limits<std::size_t>::max() &&
               batch_id != 0;
    }
};

struct DeviceRateCompletion {
    Status status = Status::invalid_state;
    std::uint64_t timestamp_domain_identity = 0;
    std::uint64_t timestamp = 0;
    bool terminal_slot_owned = false;
};

class DeviceManager final {
public:
    DeviceManager(
        std::uint32_t owner,
        std::vector<DeviceBackendSpec> backends,
        std::vector<DeviceBufferSpec> buffers,
        std::vector<DeviceTimelineSpec> timelines,
        std::size_t outstanding_capacity,
        std::size_t completion_batch);
    ~DeviceManager();

    DeviceManager(const DeviceManager&) = delete;
    DeviceManager& operator=(const DeviceManager&) = delete;

    [[nodiscard]] Status start_lane(
        Executor& executor,
        DeviceEventObserver observer,
        void* observer_data,
        ThreadPolicyProvider& provider,
        ThreadStartupGate& gate,
        const ThreadRolePlan& plan) noexcept;
    [[nodiscard]] Status start_submission_lanes(
        ThreadPolicyProvider& provider,
        ThreadStartupGate& gate,
        const ThreadRolePlan& plan) noexcept;
    [[nodiscard]] Status initialize() noexcept;
    void wait_started() const noexcept;
    Status quiesce_lane() noexcept;
    Status cleanup_lane() noexcept;
    Status stop_lane() noexcept;
    [[nodiscard]] Status stop() noexcept;
    [[nodiscard]] bool cleanup_pending() const noexcept;

    [[nodiscard]] Status submit(
        std::size_t backend_index,
        std::size_t phase_index,
        std::size_t worker_index,
        std::uint64_t frame_index,
        const DeviceSubmission& submission,
        std::uint64_t& out_submission_id) noexcept;
    [[nodiscard]] Status submit_batch(
        std::size_t backend_index,
        std::size_t phase_index,
        std::size_t worker_index,
        std::uint64_t frame_index,
        const DeviceCommandBatch& batch,
        const DeviceCommandBatch& declaration,
        std::uint64_t& out_batch_id,
        const DeviceRateReleaseIdentity* rate_identity = nullptr,
        DeviceRateTicket* rate_ticket = nullptr) noexcept;
    [[nodiscard]] Status wait_rate_batch(
        const DeviceRateTicket& ticket,
        DeviceRateCompletion& completion) noexcept;
    [[nodiscard]] Status release_rate_batch(
        const DeviceRateTicket& ticket) noexcept;
    [[nodiscard]] Status request_batch_stop() noexcept;
    [[nodiscard]] Status health(
        std::size_t backend_index,
        DeviceHealth& output) noexcept;
    [[nodiscard]] Status reset(std::size_t backend_index) noexcept;

    [[nodiscard]] DeviceManagerStats stats() const noexcept;
    [[nodiscard]] const ThreadStartupResult& startup_result() const noexcept {
        return startup_result_;
    }
    [[nodiscard]] std::size_t backend_count() const noexcept {
        return backends_.size();
    }
    [[nodiscard]] std::size_t buffer_count() const noexcept {
        return buffers_.size();
    }
    [[nodiscard]] std::size_t batch_backend_count() const noexcept {
        return batch_backend_count_;
    }
    [[nodiscard]] std::size_t timeline_count(
        std::size_t backend_index) const noexcept;
    [[nodiscard]] bool timeline_at(
        std::size_t backend_index,
        std::size_t ordinal,
        DeviceTimelineInfo& info) const noexcept;
    [[nodiscard]] bool timeline_info(
        DeviceTimelineHandle timeline,
        DeviceTimelineInfo& info) const noexcept;
    [[nodiscard]] const ThreadStartupResult* submission_startup_results()
        const noexcept { return submission_startup_results_.get(); }
    [[nodiscard]] std::size_t submission_startup_result_count() const noexcept {
        return batch_backend_count_;
    }
    void append_control_extents(
        std::vector<LogicalControlExtent>& extents,
        std::uint64_t& next_extent_id) const;

    [[nodiscard]] static bool estimate_control_storage(
        std::size_t backend_count,
        std::size_t backend_name_bytes,
        std::size_t adapted_v1_count,
        std::size_t buffer_count,
        std::size_t timeline_count,
        std::size_t batch_backend_count,
        std::size_t outstanding_capacity,
        std::size_t completion_batch,
        std::size_t& bytes) noexcept;

private:
    struct Outstanding {
        std::atomic<std::uint32_t> state{0};
        std::uint64_t submission_id = 0;
        std::uint64_t frame_index = 0;
        std::uint32_t backend_index = 0;
        std::uint32_t phase_index = 0;
        HalV2Completion early_completion{};
    };

    struct NativeMemoryState {
      std::uint64_t legacy_token = 0;
      HalV2MemoryToken heterogeneous_token{};
      std::uint8_t heterogeneous_owned = 0;
    };

    struct TimelineState {
        std::array<char, hal_v2_identifier_capacity> name{};
        std::uint32_t backend_index = 0;
        std::uint64_t initial_value = 0;
        std::atomic<std::uint64_t> last_accepted{0};
        std::atomic<std::uint64_t> completed{0};
    };

    struct BatchSlot {
        std::atomic<std::uint32_t> state{0};
        std::atomic<bool> graph_released{false};
        std::atomic<bool> cancellation_requested{false};
        std::uint64_t sequence = 0;
        std::uint64_t deadline_ns = 0;
        std::uint32_t backend_index = 0;
        std::uint32_t phase_index = 0;
        std::uint64_t frame_index = 0;
        DeviceCommandBatch batch{};
        HalV2BatchCompletion early_completion{};
        bool early_completion_valid = false;
        bool rate_owned = false;
        Status terminal_status = Status::ok;
        DeviceRateReleaseIdentity rate_identity{};
    };

    struct BatchBackendState {
        std::size_t slot_offset = 0;
        std::size_t slot_count = 0;
        std::size_t lane_index = std::numeric_limits<std::size_t>::max();
        std::atomic_flag admission = ATOMIC_FLAG_INIT;
        std::atomic<std::uint64_t> wake_sequence{0};
        std::atomic<std::uint64_t> next_sequence{1};
    };

    struct SubmissionLaneContext {
        DeviceManager* manager = nullptr;
        std::size_t backend_index = 0;
    };

    [[nodiscard]] Status initialize_backends() noexcept;
    [[nodiscard]] Status shutdown_backends() noexcept;
    [[nodiscard]] bool backend_has_registered_buffers(
        std::size_t backend_index) const noexcept;
    [[nodiscard]] bool has_backend_ownership() const noexcept;
    void service_loop() noexcept;
    static void service_entry(void* manager) noexcept;
    void process_completion(
        std::size_t backend_index,
        const HalV2Completion& completion) noexcept;
    void finalize_completion(
        Outstanding& slot,
        std::size_t backend_index,
        const HalV2Completion& completion) noexcept;
    void fail_backend_outstanding(
        std::size_t backend_index,
        Status status) noexcept;
    void submission_loop(std::size_t backend_index) noexcept;
    static void submission_entry(void* context) noexcept;
    void poll_batch_completions(std::size_t backend_index) noexcept;
    void process_batch_completion(
        std::size_t backend_index,
        const HalV2BatchCompletion& completion) noexcept;
    void finish_batch_slot(
        BatchSlot& slot,
        Status status,
        bool publish_timeline,
        const HalV2BatchCompletion* completion = nullptr) noexcept;
    void finish_rate_quarantine(
        BatchSlot& slot,
        Status status) noexcept;
    void release_rate_slots_after_shutdown() noexcept;
    void fail_backend_batches(std::size_t backend_index, Status status) noexcept;
    void record_failure(Status status) noexcept;
    [[nodiscard]] Outstanding* acquire_outstanding() noexcept;
    void emit(const DeviceEvent& event) noexcept;

    std::uint32_t owner_ = 0;
    std::vector<DeviceBackendSpec> backends_;
    std::vector<std::uint8_t> initialized_backends_;
    std::vector<DeviceBufferSpec> buffers_;
    std::vector<DeviceTimelineSpec> timeline_specs_;
    std::vector<NativeMemoryState> native_memory_;
    std::unique_ptr<TimelineState[]> timelines_;
    std::size_t timeline_count_ = 0;
    std::unique_ptr<BatchBackendState[]> batch_backends_;
    std::unique_ptr<BatchSlot[]> batch_slots_;
    std::size_t batch_slot_count_ = 0;
    std::size_t batch_backend_count_ = 0;
    std::unique_ptr<NativeThread[]> submission_threads_;
    std::unique_ptr<ThreadStartupResult[]> submission_startup_results_;
    std::unique_ptr<SubmissionLaneContext[]> submission_contexts_;
    std::unique_ptr<HalV2BatchCompletion[]> batch_completion_buffer_;
    std::unique_ptr<Outstanding[]> outstanding_slots_;
    std::size_t outstanding_capacity_ = 0;
    std::unique_ptr<HalV2Completion[]> completion_buffer_;
    std::size_t completion_batch_ = 0;
    NativeThread service_thread_;
    ThreadStartupResult startup_result_{};
    WaitStrategy wait_strategy_ = WaitStrategy::park;
    Executor* executor_ = nullptr;
    DeviceEventObserver observer_ = nullptr;
    void* observer_data_ = nullptr;
    std::atomic<bool> started_{false};
    std::atomic<bool> stopping_{false};
    std::atomic<bool> batch_admission_open_{false};
    std::atomic<bool> batch_stopping_{false};
    std::atomic<bool> service_ready_{false};
    std::atomic<std::size_t> slot_hint_{0};
    std::atomic<std::uint64_t> next_submission_id_{1};
    std::atomic<std::uint64_t> next_batch_id_{1};
    std::atomic<std::uint64_t> submissions_{0};
    std::atomic<std::uint64_t> completions_{0};
    std::atomic<std::uint64_t> failures_{0};
    std::atomic<std::uint64_t> queue_rejections_{0};
    std::atomic<std::uint64_t> timeouts_{0};
    std::atomic<std::uint64_t> losses_{0};
    std::atomic<std::uint64_t> resets_{0};
    std::atomic<std::uint64_t> service_polls_{0};
    std::atomic<std::uint64_t> outstanding_count_{0};
    std::atomic<std::uint64_t> service_starts_{0};
    std::atomic<std::uint64_t> wake_sequence_{0};
};

} // namespace rt::detail
