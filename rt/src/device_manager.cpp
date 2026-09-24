#include "device_manager.hpp"

#include "aligned_storage.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>
#include <new>
#include <thread>
#include <utility>

#include <rt/arch.hpp>

namespace {

constexpr std::uint32_t kOutstandingFree = 0;
constexpr std::uint32_t kOutstandingOwned = 1;
constexpr std::uint32_t kOutstandingSubmitting = 2;
constexpr std::uint32_t kOutstandingSubmitted = 3;
constexpr std::uint32_t kOutstandingEarlyReady = 4;
constexpr std::uint32_t kBatchFree = 0;
constexpr std::uint32_t kBatchReserved = 1;
constexpr std::uint32_t kBatchQueued = 2;
constexpr std::uint32_t kBatchSubmitting = 3;
constexpr std::uint32_t kBatchSubmitted = 4;
constexpr std::uint32_t kBatchEarlyReady = 5;
constexpr std::uint32_t kBatchOwned = 6;
constexpr std::uint32_t kBatchCompletionReady = 7;
constexpr std::uint32_t kBatchRateTerminal = 8;
constexpr std::uint32_t kBatchRateQuarantineOwned = 9;
constexpr std::uint32_t kBatchRateQuarantined = 10;

constexpr std::uint8_t kBackendOwnershipNone = 0;
constexpr std::uint8_t kBackendOwnershipInitialized = 1;
constexpr std::uint8_t kBackendOwnershipInitializationUncertain = 2;

bool bytes_zero(const std::uint64_t* values, std::size_t count) noexcept {
    for (std::size_t index = 0; index < count; ++index) {
        if (values[index] != 0) {
            return false;
        }
    }
    return true;
}

bool valid_access(std::uint32_t access) noexcept {
    return access == RTFW_DEVICE_ACCESS_READ ||
           access == RTFW_DEVICE_ACCESS_WRITE ||
           access == RTFW_DEVICE_ACCESS_READ_WRITE;
}

std::uint64_t monotonic_now_ns() noexcept {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

rt::HalV2Status runtime_status_to_hal(rt::Status status) noexcept {
    switch (status) {
    case rt::Status::device_queue_full:
        return rt::HalV2Status::queue_full;
    case rt::Status::device_timeout:
        return rt::HalV2Status::timeout;
    case rt::Status::device_lost:
        return rt::HalV2Status::lost;
    case rt::Status::device_canceled:
        return rt::HalV2Status::canceled;
    case rt::Status::device_reset_required:
        return rt::HalV2Status::reset_required;
    case rt::Status::resource_exhausted:
        return rt::HalV2Status::resource_exhausted;
    default:
        return rt::HalV2Status::error;
    }
}

} // namespace

namespace rt::detail {

DeviceManager::DeviceManager(std::uint32_t owner,
                             std::vector<DeviceBackendSpec> backends,
                             std::vector<DeviceBufferSpec> buffers,
                             std::vector<DeviceTimelineSpec> timelines,
                             std::size_t outstanding_capacity,
                             std::size_t completion_batch)
    : owner_(owner), backends_(std::move(backends)),
      initialized_backends_(backends_.size(), 0), buffers_(std::move(buffers)),
      timeline_specs_(std::move(timelines)),
      native_memory_(buffers_.size()),
      outstanding_slots_(
          outstanding_capacity == 0
              ? nullptr
              : std::make_unique<Outstanding[]>(outstanding_capacity)),
      outstanding_capacity_(outstanding_capacity),
      completion_buffer_(
          completion_batch == 0
              ? nullptr
              : std::make_unique<HalV2Completion[]>(completion_batch)),
      completion_batch_(completion_batch) {
    timeline_count_ = timeline_specs_.size();
    if (timeline_count_ != 0) {
        timelines_ = std::make_unique<TimelineState[]>(timeline_count_);
        for (std::size_t index = 0; index < timeline_count_; ++index) {
            timelines_[index].name = timeline_specs_[index].name;
            timelines_[index].backend_index = timeline_specs_[index].backend_index;
            timelines_[index].initial_value = timeline_specs_[index].initial_value;
            timelines_[index].last_accepted.store(
                timeline_specs_[index].initial_value, std::memory_order_relaxed);
            timelines_[index].completed.store(
                timeline_specs_[index].initial_value, std::memory_order_relaxed);
        }
    }
    batch_backends_ = backends_.empty()
        ? nullptr
        : std::make_unique<BatchBackendState[]>(backends_.size());
    for (std::size_t index = 0; index < backends_.size(); ++index) {
        if (!backends_[index].command_state) {
            continue;
        }
        batch_backends_[index].slot_offset = batch_slot_count_;
        batch_backends_[index].slot_count = outstanding_capacity_;
        batch_backends_[index].lane_index = batch_backend_count_++;
        batch_slot_count_ += outstanding_capacity_;
    }
    if (batch_slot_count_ != 0) {
        batch_slots_ = std::make_unique<BatchSlot[]>(batch_slot_count_);
        batch_completion_buffer_ =
            std::make_unique<HalV2BatchCompletion[]>(completion_batch_);
    }
    if (batch_backend_count_ != 0) {
        submission_threads_ = std::make_unique<NativeThread[]>(batch_backend_count_);
        submission_startup_results_ =
            std::make_unique<ThreadStartupResult[]>(batch_backend_count_);
        submission_contexts_ =
            std::make_unique<SubmissionLaneContext[]>(batch_backend_count_);
        for (std::size_t backend_index = 0; backend_index < backends_.size();
             ++backend_index) {
            const auto lane = batch_backends_[backend_index].lane_index;
            if (lane != std::numeric_limits<std::size_t>::max()) {
                submission_contexts_[lane] = {this, backend_index};
            }
        }
    }
}

DeviceManager::~DeviceManager() {
    (void)stop();
}

void DeviceManager::append_control_extents(
    std::vector<LogicalControlExtent>& extents,
    std::uint64_t& next_extent_id) const {
    const auto add = [&](const void* data, std::size_t count, std::size_t size) {
        if (count == 0) {
            return;
        }
        extents.push_back({
            next_extent_id++,
            ControlExtentOwner::device,
            data,
            count * size,
        });
    };
    add(this, 1, sizeof(*this));
    add(backends_.data(), backends_.capacity(), sizeof(backends_[0]));
    for (const auto& backend : backends_) {
        const auto begin = reinterpret_cast<std::uintptr_t>(&backend);
        const auto end = begin + sizeof(backend);
        const auto name = reinterpret_cast<std::uintptr_t>(
            backend.name.data());
        if (name < begin || name >= end) {
            add(backend.name.data(), backend.name.capacity() + 1, 1);
        }
        if (backend.v1_adapter) {
            add(backend.v1_adapter, 1, sizeof(*backend.v1_adapter));
            add(
                backend.v1_adapter->completion_storage(),
                backend.v1_adapter->completion_capacity(),
                sizeof(rtfw_device_completion));
        }
        if (backend.memory_state) {
          add(backend.memory_state, 1, sizeof(*backend.memory_state));
        }
        if (backend.command_state) {
          add(backend.command_state, 1, sizeof(*backend.command_state));
        }
    }
    add(initialized_backends_.data(), initialized_backends_.capacity(), sizeof(initialized_backends_[0]));
    add(buffers_.data(), buffers_.capacity(), sizeof(buffers_[0]));
    add(native_memory_.data(), native_memory_.capacity(),
        sizeof(native_memory_[0]));
    add(timelines_.get(), timeline_count_, sizeof(TimelineState));
    add(batch_backends_.get(), backends_.size(), sizeof(BatchBackendState));
    add(batch_slots_.get(), batch_slot_count_, sizeof(BatchSlot));
    add(submission_threads_.get(), batch_backend_count_, sizeof(NativeThread));
    add(submission_startup_results_.get(), batch_backend_count_,
        sizeof(ThreadStartupResult));
    add(submission_contexts_.get(), batch_backend_count_,
        sizeof(SubmissionLaneContext));
    add(batch_completion_buffer_.get(),
        batch_backend_count_ == 0 ? 0 : completion_batch_,
        sizeof(HalV2BatchCompletion));
    add(outstanding_slots_.get(), outstanding_capacity_, sizeof(Outstanding));
    add(completion_buffer_.get(), completion_batch_, sizeof(HalV2Completion));
}

bool DeviceManager::estimate_control_storage(
    std::size_t backend_count,
    std::size_t backend_name_bytes,
    std::size_t adapted_v1_count,
    std::size_t buffer_count,
    std::size_t timeline_count,
    std::size_t batch_backend_count,
    std::size_t outstanding_capacity,
    std::size_t completion_batch,
    std::size_t& bytes) noexcept {
    bytes = sizeof(DeviceManager);
    const auto add =
        [&bytes](std::size_t count, std::size_t size) {
            std::size_t product = 0;
            std::size_t total = 0;
            if (!checked_multiply(count, size, product) ||
                !checked_add(bytes, product, total)) {
                return false;
            }
            bytes = total;
            return true;
        };
    std::size_t adapted_completion_count = 0;
    if (!checked_multiply(
            adapted_v1_count,
            completion_batch,
            adapted_completion_count)) {
        return false;
    }
    std::size_t batch_slot_count = 0;
    if (!checked_multiply(batch_backend_count, outstanding_capacity,
                          batch_slot_count)) {
        return false;
    }
    return add(backend_count, sizeof(DeviceBackendSpec)) &&
           add(backend_name_bytes, 1) &&
           add(adapted_v1_count, sizeof(DeviceV1CompatibilityAdapter)) &&
           add(adapted_completion_count, sizeof(rtfw_device_completion)) &&
           add(backend_count, sizeof(HeterogeneousMemoryState)) &&
           add(batch_backend_count, sizeof(CommandTimelineExtensionState)) &&
           add(backend_count, sizeof(std::uint8_t)) &&
           add(buffer_count, sizeof(DeviceBufferSpec)) &&
           add(buffer_count, sizeof(NativeMemoryState)) &&
           add(timeline_count, sizeof(TimelineState)) &&
           add(backend_count, sizeof(BatchBackendState)) &&
           add(batch_slot_count, sizeof(BatchSlot)) &&
           add(batch_backend_count, sizeof(NativeThread)) &&
           add(batch_backend_count, sizeof(ThreadStartupResult)) &&
           add(batch_backend_count, sizeof(SubmissionLaneContext)) &&
           add(batch_backend_count == 0 ? 0 : completion_batch,
               sizeof(HalV2BatchCompletion)) &&
           add(outstanding_capacity, sizeof(Outstanding)) &&
           add(completion_batch, sizeof(HalV2Completion));
}

Status DeviceManager::initialize_backends() noexcept {
    for (std::size_t index = 0;
         index < backends_.size();
         ++index) {
        auto& backend = backends_[index];
        HalV2InitializeConfig config;
        config.requested_in_flight = outstanding_capacity_;
        std::size_t buffer_count = 0;
        for (const auto& buffer : buffers_) {
            buffer_count += buffer.backend_index == index ? 1u : 0u;
        }
        config.requested_registered_buffers = buffer_count;

        HalV2Status device_status = HalV2Status::internal_error;
        try {
            device_status = backend.api.initialize(
                backend.api.instance,
                &config);
        } catch (...) {
            device_status = HalV2Status::internal_error;
        }
        const auto status = hal_v2_status_to_runtime(device_status);
        if (status != Status::ok) {
            initialized_backends_[index] =
                kBackendOwnershipInitializationUncertain;
            (void)shutdown_backends();
            return status;
        }
        initialized_backends_[index] =
            kBackendOwnershipInitialized;
    }

    for (std::size_t index = 0; index < buffers_.size(); ++index) {
        const auto& buffer = buffers_[index];
        auto &backend = backends_[buffer.backend_index];
        HalV2Status device_status = HalV2Status::internal_error;
        auto &native = native_memory_[index];
        if (buffer.heterogeneous) {
          if (!backend.memory_state ||
              !backend.memory_state->native_extension) {
            (void)shutdown_backends();
            return Status::invalid_state;
          }
          HalV2MemoryRegistration registration;
          registration.domain_identity = buffer.domain_identity;
          registration.bytes = buffer.bytes;
          registration.ownership = buffer.ownership;
          registration.access = buffer.flags;
          registration.coherency = buffer.coherency;
          registration.synchronization = buffer.synchronization;
          registration.host_data = buffer.storage.data();
          registration.opaque_handle = buffer.opaque_handle;
          std::copy(buffer.name.begin(), buffer.name.end(),
                    registration.name.begin());
          native.heterogeneous_owned = 1;
          try {
            device_status = backend.memory_state->extension.register_memory(
                backend.memory_state->extension.instance, &registration,
                &native.heterogeneous_token);
          } catch (...) {
            device_status = HalV2Status::internal_error;
          }
          if (device_status == HalV2Status::ok &&
              !validate_memory_token(native.heterogeneous_token, true)) {
            device_status = HalV2Status::internal_error;
          }
        } else {
          HalV2BufferRegistration registration;
          registration.flags = buffer.flags;
          registration.data = buffer.storage.data();
          registration.bytes = buffer.bytes;
          std::copy(buffer.name.begin(), buffer.name.end(),
                    registration.name.begin());
          try {
            device_status = backend.api.register_buffer(
                backend.api.instance, &registration, &native.legacy_token);
          } catch (...) {
            device_status = HalV2Status::internal_error;
          }
        }
        const auto status = hal_v2_status_to_runtime(device_status);
        if (status != Status::ok ||
            (!buffer.heterogeneous && native.legacy_token == 0)) {
          (void)shutdown_backends();
          return status == Status::ok ? Status::device_error : status;
        }
    }
    return Status::ok;
}

bool DeviceManager::backend_has_registered_buffers(
    std::size_t backend_index) const noexcept {
    for (std::size_t index = 0;
         index < buffers_.size();
         ++index) {
      if (buffers_[index].backend_index == backend_index &&
          (native_memory_[index].legacy_token != 0 ||
           native_memory_[index].heterogeneous_owned != 0)) {
        return true;
      }
    }
    return false;
}

bool DeviceManager::has_backend_ownership() const noexcept {
  return std::any_of(native_memory_.begin(), native_memory_.end(),
                     [](const NativeMemoryState &state) {
                       return state.legacy_token != 0 ||
                              state.heterogeneous_owned != 0;
                     }) ||
         std::any_of(initialized_backends_.begin(), initialized_backends_.end(),
                     [](std::uint8_t initialized) { return initialized != 0; });
}

bool DeviceManager::cleanup_pending() const noexcept {
    if (has_backend_ownership() || service_thread_.joinable()) {
        return true;
    }
    for (std::size_t lane = 0; lane < batch_backend_count_; ++lane) {
        if (submission_threads_[lane].joinable()) {
            return true;
        }
    }
    return false;
}

Status DeviceManager::shutdown_backends() noexcept {
    Status first_failure = Status::ok;
    for (std::size_t index = buffers_.size(); index != 0; --index) {
        const auto buffer_index = index - 1;
        auto &native = native_memory_[buffer_index];
        if (native.legacy_token == 0 && native.heterogeneous_owned == 0) {
          continue;
        }
        const auto backend_index =
            static_cast<std::size_t>(
                buffers_[buffer_index].backend_index);
        if (backend_index >= backends_.size() ||
            initialized_backends_[backend_index] ==
                kBackendOwnershipNone) {
            if (first_failure == Status::ok) {
                first_failure = Status::invalid_state;
            }
            continue;
        }
        auto& backend = backends_[backend_index];
        HalV2Status device_status = HalV2Status::internal_error;
        if (buffers_[buffer_index].heterogeneous) {
          HalV2MemoryRegistration registration;
          const auto &buffer = buffers_[buffer_index];
          registration.domain_identity = buffer.domain_identity;
          registration.bytes = buffer.bytes;
          registration.ownership = buffer.ownership;
          registration.access = buffer.flags;
          registration.coherency = buffer.coherency;
          registration.synchronization = buffer.synchronization;
          registration.host_data = buffer.storage.data();
          registration.opaque_handle = buffer.opaque_handle;
          std::copy(buffer.name.begin(), buffer.name.end(),
                    registration.name.begin());
          try {
            device_status = backend.memory_state->extension.unregister_memory(
                backend.memory_state->extension.instance, &registration,
                &native.heterogeneous_token);
          } catch (...) {
            device_status = HalV2Status::internal_error;
          }
        } else {
          try {
            device_status = backend.api.unregister_buffer(backend.api.instance,
                                                          native.legacy_token);
          } catch (...) {
            device_status = HalV2Status::internal_error;
          }
        }
        const auto status = hal_v2_status_to_runtime(device_status);
        if (status == Status::ok) {
          native = {};
        } else if (first_failure == Status::ok) {
            first_failure = status;
        }
    }
    for (std::size_t index = backends_.size(); index != 0; --index) {
        const auto backend_index = index - 1;
        const auto ownership =
            initialized_backends_[backend_index];
        if (ownership == kBackendOwnershipNone ||
            backend_has_registered_buffers(backend_index)) {
            continue;
        }
        auto& backend = backends_[backend_index];
        HalV2Status device_status = HalV2Status::internal_error;
        try {
            device_status =
                backend.api.shutdown(backend.api.instance);
        } catch (...) {
            device_status = HalV2Status::internal_error;
        }
        const auto status = hal_v2_status_to_runtime(device_status);
        if (status == Status::ok ||
            (ownership ==
                 kBackendOwnershipInitializationUncertain &&
             device_status == HalV2Status::invalid_state)) {
            initialized_backends_[backend_index] =
                kBackendOwnershipNone;
        } else if (first_failure == Status::ok) {
            first_failure = status;
        }
    }
    return first_failure;
}

Status DeviceManager::start_lane(
    Executor& executor,
    DeviceEventObserver observer,
    void* observer_data,
    ThreadPolicyProvider& provider,
    ThreadStartupGate& gate,
    const ThreadRolePlan& plan) noexcept {
    if (started_.load(std::memory_order_acquire) ||
        has_backend_ownership() ||
        service_thread_.joinable() ||
        outstanding_capacity_ == 0 ||
        completion_batch_ == 0 ||
        backends_.empty()) {
        return Status::invalid_state;
    }
    executor_ = &executor;
    observer_ = observer;
    observer_data_ = observer_data;
    stopping_.store(false, std::memory_order_release);
    service_ready_.store(false, std::memory_order_relaxed);
    started_.store(true, std::memory_order_release);
    wait_strategy_ = plan.resolved.wait_strategy;
    const auto status = service_thread_.start(
        provider,
        gate,
        plan,
        0,
        startup_result_,
        &DeviceManager::service_entry,
        this);
    if (status != Status::ok) {
        started_.store(false, std::memory_order_release);
        executor_ = nullptr;
        observer_ = nullptr;
        observer_data_ = nullptr;
        return status;
    }
    return Status::ok;
}

Status DeviceManager::start_submission_lanes(
    ThreadPolicyProvider& provider,
    ThreadStartupGate& gate,
    const ThreadRolePlan& plan) noexcept {
    if (!started_.load(std::memory_order_acquire) ||
        (batch_backend_count_ != 0 &&
         (!submission_threads_ || !submission_startup_results_ ||
          !submission_contexts_))) {
        return Status::invalid_state;
    }
    batch_stopping_.store(false, std::memory_order_release);
    batch_admission_open_.store(true, std::memory_order_release);
    for (std::size_t lane = 0; lane < batch_backend_count_; ++lane) {
        const auto status = submission_threads_[lane].start(
            provider, gate, plan, lane, submission_startup_results_[lane],
            &DeviceManager::submission_entry, &submission_contexts_[lane]);
        if (status != Status::ok) {
            batch_admission_open_.store(false, std::memory_order_release);
            batch_stopping_.store(true, std::memory_order_release);
            for (std::size_t prior = lane; prior != 0; --prior) {
                const auto backend = submission_contexts_[prior - 1].backend_index;
                batch_backends_[backend].wake_sequence.fetch_add(
                    1, std::memory_order_release);
                batch_backends_[backend].wake_sequence.notify_all();
            }
            return status;
        }
    }
    return Status::ok;
}

Status DeviceManager::initialize() noexcept {
    if (!started_.load(std::memory_order_acquire)) {
        return Status::invalid_state;
    }
    return initialize_backends();
}

void DeviceManager::wait_started() const noexcept {
    service_thread_.wait_started();
    for (std::size_t lane = 0; lane < batch_backend_count_; ++lane) {
        submission_threads_[lane].wait_started();
    }
}

Status DeviceManager::request_batch_stop() noexcept {
    batch_admission_open_.store(false, std::memory_order_release);
    batch_stopping_.store(true, std::memory_order_release);
    Status first_failure = Status::ok;
    for (std::size_t backend_index = 0; backend_index < backends_.size();
         ++backend_index) {
        auto* state = backends_[backend_index].command_state;
        if (!state) {
            continue;
        }
        auto& control = batch_backends_[backend_index];
        bool request_needed =
            initialized_backends_[backend_index] != kBackendOwnershipNone;
        if (control.lane_index != std::numeric_limits<std::size_t>::max()) {
            request_needed = request_needed ||
                submission_threads_[control.lane_index].joinable();
        }
        for (std::size_t offset = 0;
             !request_needed && offset < control.slot_count; ++offset) {
            request_needed = batch_slots_[control.slot_offset + offset]
                                 .state.load(std::memory_order_acquire) !=
                             kBatchFree;
        }
        if (!request_needed) {
            continue;
        }
        HalV2Status result = HalV2Status::internal_error;
        try {
            result = state->extension.request_stop(state->extension.instance);
        } catch (...) {
            result = HalV2Status::internal_error;
        }
        const auto status = hal_v2_status_to_runtime(result);
        if (status != Status::ok && first_failure == Status::ok) {
            first_failure = status;
        }
        for (std::size_t offset = 0; offset < control.slot_count; ++offset) {
            auto& slot = batch_slots_[control.slot_offset + offset];
            const auto slot_state = slot.state.load(std::memory_order_acquire);
            if (slot_state == kBatchFree || slot_state == kBatchReserved) {
                continue;
            }
            if (slot_state != kBatchQueued &&
                slot_state != kBatchRateTerminal &&
                slot_state != kBatchRateQuarantineOwned &&
                slot_state != kBatchRateQuarantined &&
                !slot.cancellation_requested.exchange(
                    true, std::memory_order_acq_rel)) {
                try {
                    (void)state->extension.cancel(
                        state->extension.instance, slot.batch.batch_id);
                } catch (...) {
                }
            }
            if (slot_state == kBatchQueued) {
                auto expected = kBatchQueued;
                if (slot.state.compare_exchange_strong(
                        expected, kBatchOwned, std::memory_order_acq_rel,
                        std::memory_order_relaxed)) {
                    finish_batch_slot(slot, Status::device_canceled, false);
                }
            } else if (!slot.rate_owned &&
                       !slot.graph_released.exchange(
                           true, std::memory_order_acq_rel) && executor_) {
                (void)executor_->complete_external(
                    slot.phase_index, Status::device_canceled);
            }
        }
        control.wake_sequence.fetch_add(1, std::memory_order_release);
        control.wake_sequence.notify_all();
    }
    return first_failure;
}

Status DeviceManager::quiesce_lane() noexcept {
    const bool was_started =
        started_.exchange(false, std::memory_order_acq_rel);
    if (was_started || service_thread_.joinable()) {
        stopping_.store(true, std::memory_order_release);
        wake_sequence_.fetch_add(1, std::memory_order_release);
        wake_sequence_.notify_all();
        service_thread_.wait_quiescent();
        for (std::size_t index = 0;
             index < outstanding_capacity_;
             ++index) {
            auto& slot = outstanding_slots_[index];
            const auto previous =
                slot.state.exchange(
                    kOutstandingFree,
                    std::memory_order_acq_rel);
            if (previous == kOutstandingSubmitted && executor_) {
                (void)executor_->complete_external(
                    slot.phase_index,
                    Status::device_canceled);
            }
        }
        outstanding_count_.store(0, std::memory_order_release);
        executor_ = nullptr;
        observer_ = nullptr;
        observer_data_ = nullptr;
    }
    return Status::ok;
}

Status DeviceManager::cleanup_lane() noexcept {
    return service_thread_.cleanup_and_join();
}

Status DeviceManager::stop_lane() noexcept {
    const auto request_status = request_batch_stop();
    Status submission_status = Status::ok;
    for (std::size_t lane = batch_backend_count_; lane != 0; --lane) {
        if (!submission_threads_[lane - 1].joinable()) {
            continue;
        }
        submission_threads_[lane - 1].wait_quiescent();
        const auto status = submission_threads_[lane - 1].cleanup_and_join();
        if (submission_status == Status::ok && status != Status::ok) {
            submission_status = status;
        }
    }
    for (std::size_t backend_index = 0; backend_index < backends_.size();
         ++backend_index) {
        fail_backend_batches(backend_index, Status::device_canceled);
    }
    const auto quiesce_status = quiesce_lane();
    const auto cleanup_status = cleanup_lane();
    if (request_status != Status::ok) {
        return request_status;
    }
    if (submission_status != Status::ok) {
        return submission_status;
    }
    return quiesce_status != Status::ok ? quiesce_status : cleanup_status;
}

Status DeviceManager::stop() noexcept {
    const auto request_status = request_batch_stop();
    Status submission_status = Status::ok;
    for (std::size_t lane = batch_backend_count_; lane != 0; --lane) {
        if (!submission_threads_[lane - 1].joinable()) {
            continue;
        }
        submission_threads_[lane - 1].wait_quiescent();
        const auto status = submission_threads_[lane - 1].cleanup_and_join();
        if (submission_status == Status::ok && status != Status::ok) {
            submission_status = status;
        }
    }
    for (std::size_t backend_index = 0; backend_index < backends_.size();
         ++backend_index) {
        fail_backend_batches(backend_index, Status::device_canceled);
    }
    const auto quiesce_status = quiesce_lane();
    const auto backend_status = shutdown_backends();
    if (backend_status == Status::ok) {
        release_rate_slots_after_shutdown();
    }
    const auto cleanup_status = cleanup_lane();
    if (request_status != Status::ok) {
        return request_status;
    }
    if (submission_status != Status::ok) {
        return submission_status;
    }
    if (quiesce_status != Status::ok) {
        return quiesce_status;
    }
    if (backend_status != Status::ok) {
        return backend_status;
    }
    return cleanup_status;
}

void DeviceManager::release_rate_slots_after_shutdown() noexcept {
    for (std::size_t index = 0; index < batch_slot_count_; ++index) {
        auto& slot = batch_slots_[index];
        if (!slot.rate_owned) {
            continue;
        }
        const auto previous = slot.state.exchange(
            kBatchFree, std::memory_order_acq_rel);
        if (previous != kBatchFree && previous != kBatchReserved) {
            auto count = outstanding_count_.load(std::memory_order_relaxed);
            while (count != 0 &&
                   !outstanding_count_.compare_exchange_weak(
                       count, count - 1, std::memory_order_release,
                       std::memory_order_relaxed)) {
            }
        }
        slot.rate_owned = false;
    }
}

DeviceManager::Outstanding* DeviceManager::acquire_outstanding() noexcept {
    const auto first =
        slot_hint_.fetch_add(1, std::memory_order_relaxed) %
        outstanding_capacity_;
    for (std::size_t offset = 0;
         offset < outstanding_capacity_;
         ++offset) {
        auto& slot = outstanding_slots_[
            (first + offset) % outstanding_capacity_];
        auto expected = kOutstandingFree;
        if (slot.state.compare_exchange_strong(
                expected,
                kOutstandingOwned,
                std::memory_order_acq_rel,
                std::memory_order_relaxed)) {
            return &slot;
        }
    }
    return nullptr;
}

Status DeviceManager::submit(
    std::size_t backend_index,
    std::size_t phase_index,
    std::size_t worker_index,
    std::uint64_t frame_index,
    const DeviceSubmission& requested,
    std::uint64_t& out_submission_id) noexcept {
    out_submission_id = 0;
    if (!started_.load(std::memory_order_acquire) ||
        backend_index >= backends_.size() ||
        requested.struct_size < sizeof(requested) ||
        requested.abi_version != RTFW_DEVICE_ABI_VERSION ||
        requested.submission_id != 0 ||
        requested.frame_index != 0 ||
        requested.timeout_ns == 0 ||
        requested.flags != 0 ||
        requested.payload_size >
            RTFW_DEVICE_INLINE_PAYLOAD_CAPACITY ||
        requested.buffer_count >
            RTFW_DEVICE_BUFFER_REF_CAPACITY ||
        !bytes_zero(
            requested.reserved,
            std::size(requested.reserved))) {
        return Status::invalid_argument;
    }

    HalV2Submission submission;
    submission.timeout_ns = requested.timeout_ns;
    submission.opcode = requested.opcode;
    submission.flags = requested.flags;
    submission.payload_size = requested.payload_size;
    submission.buffer_count = requested.buffer_count;
    std::copy(
        std::begin(requested.payload),
        std::end(requested.payload),
        submission.payload.begin());
    for (std::size_t index = 0;
         index < requested.buffer_count;
         ++index) {
        const auto& requested_reference = requested.buffers[index];
        auto& reference = submission.buffers[index];
        const DeviceBufferHandle logical{requested_reference.buffer_token};
        if (!logical.valid() ||
            logical.owner() != owner_ ||
            logical.index() >= buffers_.size() ||
            requested_reference.reserved0 != 0 ||
            !valid_access(requested_reference.access)) {
            return Status::invalid_handle;
        }
        const auto buffer_index =
            static_cast<std::size_t>(logical.index());
        const auto& buffer = buffers_[buffer_index];
        const auto *domain =
            backends_[backend_index].memory_state
                ? find_memory_domain(*backends_[backend_index].memory_state,
                                     buffer.domain_identity)
                : nullptr;
        if (buffer.backend_index != backend_index || !domain ||
            requested_reference.offset > buffer.bytes ||
            requested_reference.bytes >
                buffer.bytes - requested_reference.offset ||
            requested_reference.offset % domain->offset_granularity != 0 ||
            requested_reference.bytes % domain->byte_granularity != 0) {
          return Status::invalid_argument;
        }
        const auto required_access =
            requested_reference.access == RTFW_DEVICE_ACCESS_READ
                ? RTFW_DEVICE_BUFFER_DEVICE_READ
            : requested_reference.access == RTFW_DEVICE_ACCESS_WRITE
                ? RTFW_DEVICE_BUFFER_DEVICE_WRITE
                : RTFW_DEVICE_BUFFER_DEVICE_READ |
                      RTFW_DEVICE_BUFFER_DEVICE_WRITE;
        if ((required_access & ~buffer.flags) != 0 ||
            (buffer.heterogeneous &&
             buffer.synchronization != hal_v2_memory_sync_none)) {
          return Status::device_error;
        }
        reference.buffer_token =
            buffer.heterogeneous ? native_memory_[buffer_index]
                                       .heterogeneous_token.submission_token
                                 : native_memory_[buffer_index].legacy_token;
        if (reference.buffer_token == 0) {
          return Status::invalid_state;
        }
        reference.access = requested_reference.access;
        reference.offset = requested_reference.offset;
        reference.bytes = requested_reference.bytes;
    }

    auto* slot = acquire_outstanding();
    if (!slot) {
        queue_rejections_.fetch_add(1, std::memory_order_relaxed);
        return Status::device_queue_full;
    }
    auto submission_id =
        next_submission_id_.fetch_add(1, std::memory_order_relaxed);
    if (submission_id == 0) {
        submission_id =
            next_submission_id_.fetch_add(1, std::memory_order_relaxed);
    }
    slot->submission_id = submission_id;
    slot->frame_index = frame_index;
    slot->backend_index =
        static_cast<std::uint32_t>(backend_index);
    slot->phase_index =
        static_cast<std::uint32_t>(phase_index);
    slot->state.store(
        kOutstandingSubmitting,
        std::memory_order_release);

    submission.submission_id = submission_id;
    submission.frame_index = frame_index;
    HalV2Status device_status = HalV2Status::internal_error;
    try {
        device_status = backends_[backend_index].api.submit(
            backends_[backend_index].api.instance,
            &submission);
    } catch (...) {
        device_status = HalV2Status::internal_error;
    }
    const auto status = hal_v2_status_to_runtime(device_status);
    if (status != Status::ok) {
        const auto previous = slot->state.exchange(
            kOutstandingFree,
            std::memory_order_acq_rel);
        if (status == Status::device_queue_full) {
            queue_rejections_.fetch_add(1, std::memory_order_relaxed);
        } else {
            record_failure(status);
        }
        // Publishing a completion for a rejected submission violates the ABI
        // contract. Fail closed instead of applying an unaccepted result.
        return previous == kOutstandingEarlyReady
            ? Status::device_error
            : status;
    }

    outstanding_count_.fetch_add(1, std::memory_order_release);
    submissions_.fetch_add(1, std::memory_order_relaxed);
    out_submission_id = submission_id;
    auto submitted_event = DeviceEvent{
        DeviceEventKind::submitted,
        Status::ok,
        backend_index,
        phase_index,
        frame_index,
        submission_id,
        0,
    };
    submitted_event.producer = RuntimeTraceProducer::worker;
    submitted_event.worker_index = worker_index;
    emit(submitted_event);
    auto expected = kOutstandingSubmitting;
    if (!slot->state.compare_exchange_strong(
            expected,
            kOutstandingSubmitted,
            std::memory_order_acq_rel,
            std::memory_order_relaxed)) {
        if (expected != kOutstandingEarlyReady) {
            fail_backend_outstanding(
                backend_index,
                Status::device_error);
            return Status::ok;
        }
        expected = kOutstandingEarlyReady;
        if (!slot->state.compare_exchange_strong(
                expected,
                kOutstandingOwned,
                std::memory_order_acq_rel,
                std::memory_order_relaxed)) {
            fail_backend_outstanding(
                backend_index,
                Status::device_error);
            return Status::ok;
        }
        finalize_completion(
            *slot,
            backend_index,
            slot->early_completion);
        return Status::ok;
    }
    wake_sequence_.fetch_add(1, std::memory_order_release);
    wake_sequence_.notify_one();
    return Status::ok;
}

std::size_t DeviceManager::timeline_count(
    std::size_t backend_index) const noexcept {
    std::size_t count = 0;
    for (std::size_t index = 0; index < timeline_count_; ++index) {
        count += timelines_[index].backend_index == backend_index ? 1u : 0u;
    }
    return count;
}

bool DeviceManager::timeline_at(
    std::size_t backend_index,
    std::size_t ordinal,
    DeviceTimelineInfo& info) const noexcept {
    for (std::size_t index = 0; index < timeline_count_; ++index) {
        const auto& timeline = timelines_[index];
        if (timeline.backend_index != backend_index) {
            continue;
        }
        if (ordinal-- != 0) {
            continue;
        }
        DeviceTimelineInfo candidate;
        candidate.timeline = DeviceTimelineHandle{
            owner_, static_cast<std::uint32_t>(index)};
        candidate.backend = DeviceBackendHandle{
            owner_, static_cast<std::uint32_t>(backend_index)};
        candidate.name = timeline.name;
        candidate.initial_value = timeline.initial_value;
        candidate.last_accepted_value =
            timeline.last_accepted.load(std::memory_order_acquire);
        candidate.completed_value =
            timeline.completed.load(std::memory_order_acquire);
        info = candidate;
        return true;
    }
    return false;
}

bool DeviceManager::timeline_info(
    DeviceTimelineHandle handle,
    DeviceTimelineInfo& info) const noexcept {
    info = {};
    if (!handle.valid() || handle.owner() != owner_ ||
        handle.index() >= timeline_count_) {
        return false;
    }
    const auto& timeline = timelines_[handle.index()];
    info.timeline = handle;
    info.backend = DeviceBackendHandle{owner_, timeline.backend_index};
    info.name = timeline.name;
    info.initial_value = timeline.initial_value;
    info.last_accepted_value =
        timeline.last_accepted.load(std::memory_order_acquire);
    info.completed_value =
        timeline.completed.load(std::memory_order_acquire);
    return true;
}

Status DeviceManager::submit_batch(
    std::size_t backend_index,
    std::size_t phase_index,
    std::size_t worker_index,
    std::uint64_t frame_index,
    const DeviceCommandBatch& requested,
    const DeviceCommandBatch& declaration,
    std::uint64_t& out_batch_id,
    const DeviceRateReleaseIdentity* rate_identity,
    DeviceRateTicket* rate_ticket) noexcept {
    out_batch_id = 0;
    if (rate_ticket) {
        *rate_ticket = {};
    }
    if (!started_.load(std::memory_order_acquire) ||
        !batch_admission_open_.load(std::memory_order_acquire) ||
        backend_index >= backends_.size() ||
        !backends_[backend_index].command_state ||
        !validate_batch_shape(requested) ||
        !batch_matches_declaration(requested, declaration) ||
        ((rate_identity == nullptr) != (rate_ticket == nullptr)) ||
        (rate_identity &&
         (rate_identity->reference_index ==
              std::numeric_limits<std::size_t>::max() ||
          !rate_identity->domain.valid() || !rate_identity->phase.valid() ||
          rate_identity->phase.index() != phase_index))) {
        return Status::invalid_argument;
    }
    const auto& capabilities =
        backends_[backend_index].command_state->capabilities;
    if (requested.command_count > capabilities.max_commands_per_batch ||
        requested.wait_count > capabilities.max_wait_points ||
        requested.signal_count > capabilities.max_signal_points ||
        requested.timeout_ns >
            std::numeric_limits<std::uint64_t>::max() - monotonic_now_ns()) {
        return Status::invalid_argument;
    }
    auto& control = batch_backends_[backend_index];
    if (control.admission.test_and_set(std::memory_order_acquire)) {
        queue_rejections_.fetch_add(1, std::memory_order_relaxed);
        return Status::device_queue_full;
    }
    const auto release_admission = [&control] {
        control.admission.clear(std::memory_order_release);
    };
    if (!batch_admission_open_.load(std::memory_order_acquire)) {
        release_admission();
        return Status::invalid_state;
    }

    BatchSlot* slot = nullptr;
    for (std::size_t offset = 0; offset < control.slot_count; ++offset) {
        auto& candidate = batch_slots_[control.slot_offset + offset];
        auto expected = kBatchFree;
        if (candidate.state.compare_exchange_strong(
                expected, kBatchReserved, std::memory_order_acq_rel,
                std::memory_order_relaxed)) {
            slot = &candidate;
            break;
        }
    }
    if (!slot) {
        release_admission();
        queue_rejections_.fetch_add(1, std::memory_order_relaxed);
        return Status::device_queue_full;
    }
    const auto reject = [&](Status status) {
        slot->state.store(kBatchFree, std::memory_order_release);
        release_admission();
        return status;
    };

    for (std::size_t index = 0; index < requested.wait_count; ++index) {
        const auto handle = DeviceTimelineHandle{
            requested.waits[index].timeline_handle};
        if (!handle.valid() || handle.owner() != owner_ ||
            handle.index() >= timeline_count_ ||
            timelines_[handle.index()].backend_index != backend_index ||
            requested.waits[index].value >
                timelines_[handle.index()].last_accepted.load(
                    std::memory_order_relaxed)) {
            return reject(Status::invalid_handle);
        }
        for (std::size_t prior = 0; prior < index; ++prior) {
            if (requested.waits[prior].timeline_handle == handle.value) {
                return reject(Status::invalid_argument);
            }
        }
    }
    for (std::size_t index = 0; index < requested.signal_count; ++index) {
        const auto handle = DeviceTimelineHandle{
            requested.signals[index].timeline_handle};
        if (!handle.valid() || handle.owner() != owner_ ||
            handle.index() >= timeline_count_ ||
            timelines_[handle.index()].backend_index != backend_index ||
            requested.signals[index].value <=
                timelines_[handle.index()].last_accepted.load(
                    std::memory_order_relaxed)) {
            return reject(Status::invalid_handle);
        }
        for (std::size_t prior = 0; prior < index; ++prior) {
            if (requested.signals[prior].timeline_handle == handle.value) {
                return reject(Status::invalid_argument);
            }
        }
    }

    DeviceCommandBatch translated = requested;
    const auto translate_reference =
        [&](const HalV2BufferReference& input,
            HalV2BufferReference& output,
            std::size_t& buffer_index) -> Status {
            const DeviceBufferHandle logical{input.buffer_token};
            if (!logical.valid() || logical.owner() != owner_ ||
                logical.index() >= buffers_.size() ||
                input.reserved0 != 0 || !valid_access(input.access)) {
                return Status::invalid_handle;
            }
            buffer_index = logical.index();
            const auto& buffer = buffers_[buffer_index];
            const auto* domain = backends_[backend_index].memory_state
                ? find_memory_domain(*backends_[backend_index].memory_state,
                                     buffer.domain_identity)
                : nullptr;
            if (buffer.backend_index != backend_index || !domain ||
                input.offset > buffer.bytes ||
                input.bytes > buffer.bytes - input.offset ||
                input.offset % domain->offset_granularity != 0 ||
                input.bytes % domain->byte_granularity != 0) {
                return Status::invalid_argument;
            }
            const auto required_access =
                input.access == RTFW_DEVICE_ACCESS_READ
                    ? RTFW_DEVICE_BUFFER_DEVICE_READ
                : input.access == RTFW_DEVICE_ACCESS_WRITE
                    ? RTFW_DEVICE_BUFFER_DEVICE_WRITE
                    : RTFW_DEVICE_BUFFER_DEVICE_READ |
                          RTFW_DEVICE_BUFFER_DEVICE_WRITE;
            if ((required_access & ~buffer.flags) != 0) {
                return Status::invalid_argument;
            }
            const auto token = buffer.heterogeneous
                ? native_memory_[buffer_index]
                      .heterogeneous_token.submission_token
                : native_memory_[buffer_index].legacy_token;
            if (token == 0) {
                return Status::invalid_state;
            }
            output = input;
            output.buffer_token = token;
            return Status::ok;
        };
    const auto covers = [](const HalV2BufferReference& operation,
                           const HalV2BufferReference& reference) {
        return operation.buffer_token == reference.buffer_token &&
               operation.offset <= reference.offset &&
               operation.bytes >= reference.bytes &&
               reference.offset - operation.offset <=
                   operation.bytes - reference.bytes;
    };
    const auto has_operation = [&](std::size_t command_index,
                                   const HalV2BufferReference& reference,
                                   HalV2MemoryOperation operation,
                                   bool before) {
        const auto matches = [&](const DeviceCommand& command) {
            if (command.operation != static_cast<std::uint32_t>(operation)) {
                return false;
            }
            const auto kind = static_cast<HalV2CommandKind>(command.kind);
            if (kind == HalV2CommandKind::memory_synchronization) {
                return covers(command.target, reference);
            }
            if (kind != HalV2CommandKind::copy) {
                return false;
            }
            return covers(operation == HalV2MemoryOperation::copy_to_device
                              ? command.destination
                              : command.source,
                          reference);
        };
        if (before) {
            for (std::size_t cursor = command_index; cursor != 0; --cursor) {
                if (matches(requested.commands[cursor - 1])) {
                    return true;
                }
            }
        } else {
            for (std::size_t cursor = command_index + 1;
                 cursor < requested.command_count; ++cursor) {
                if (matches(requested.commands[cursor])) {
                    return true;
                }
            }
        }
        return false;
    };

    for (std::size_t command_index = 0;
         command_index < requested.command_count; ++command_index) {
        const auto& command = requested.commands[command_index];
        auto& translated_command = translated.commands[command_index];
        const auto kind = static_cast<HalV2CommandKind>(command.kind);
        if (kind == HalV2CommandKind::dispatch) {
            for (std::size_t ref_index = 0;
                 ref_index < command.buffer_count; ++ref_index) {
                std::size_t buffer_index = 0;
                const auto status = translate_reference(
                    command.buffers[ref_index],
                    translated_command.buffers[ref_index], buffer_index);
                if (status != Status::ok) {
                    return reject(status);
                }
                const auto& buffer = buffers_[buffer_index];
                if ((buffer.synchronization & hal_v2_memory_sync_timeline) != 0) {
                    return reject(Status::device_error);
                }
                const bool reads = command.buffers[ref_index].access !=
                    RTFW_DEVICE_ACCESS_WRITE;
                const bool writes = command.buffers[ref_index].access !=
                    RTFW_DEVICE_ACCESS_READ;
                if (reads &&
                    (((buffer.synchronization & hal_v2_memory_sync_flush) != 0 &&
                      !has_operation(command_index, command.buffers[ref_index],
                                     HalV2MemoryOperation::flush, true)) ||
                     ((buffer.synchronization &
                       hal_v2_memory_sync_copy_to_device) != 0 &&
                      !has_operation(command_index, command.buffers[ref_index],
                                     HalV2MemoryOperation::copy_to_device,
                                     true)))) {
                    return reject(Status::device_error);
                }
                if (writes &&
                    (((buffer.synchronization &
                       hal_v2_memory_sync_invalidate) != 0 &&
                      !has_operation(command_index, command.buffers[ref_index],
                                     HalV2MemoryOperation::invalidate, false)) ||
                     ((buffer.synchronization &
                       hal_v2_memory_sync_copy_from_device) != 0 &&
                      !has_operation(command_index, command.buffers[ref_index],
                                     HalV2MemoryOperation::copy_from_device,
                                     false)))) {
                    return reject(Status::device_error);
                }
            }
        } else if (kind == HalV2CommandKind::copy) {
            std::size_t source_index = 0;
            std::size_t destination_index = 0;
            auto status = translate_reference(
                command.source, translated_command.source, source_index);
            if (status == Status::ok) {
                status = translate_reference(
                    command.destination, translated_command.destination,
                    destination_index);
            }
            if (status != Status::ok || command.source.bytes !=
                    command.destination.bytes ||
                command.source.access != RTFW_DEVICE_ACCESS_READ ||
                command.destination.access != RTFW_DEVICE_ACCESS_WRITE) {
                return reject(status == Status::ok ? Status::invalid_argument
                                                   : status);
            }
            const auto operation =
                static_cast<HalV2MemoryOperation>(command.operation);
            const auto obligation_index =
                operation == HalV2MemoryOperation::copy_to_device
                    ? destination_index
                    : source_index;
            const auto obligation = operation ==
                    HalV2MemoryOperation::copy_to_device
                ? hal_v2_memory_sync_copy_to_device
                : hal_v2_memory_sync_copy_from_device;
            if ((buffers_[obligation_index].synchronization & obligation) == 0) {
                return reject(Status::device_error);
            }
        } else {
            std::size_t target_index = 0;
            const auto status = translate_reference(
                command.target, translated_command.target, target_index);
            const auto operation =
                static_cast<HalV2MemoryOperation>(command.operation);
            const auto obligation = operation == HalV2MemoryOperation::flush
                ? hal_v2_memory_sync_flush
                : hal_v2_memory_sync_invalidate;
            if (status != Status::ok ||
                (buffers_[target_index].synchronization & obligation) == 0) {
                return reject(status == Status::ok ? Status::device_error
                                                   : status);
            }
        }
    }

    auto batch_id = next_batch_id_.fetch_add(1, std::memory_order_relaxed);
    if (batch_id == 0) {
        batch_id = next_batch_id_.fetch_add(1, std::memory_order_relaxed);
    }
    translated.batch_id = batch_id;
    translated.frame_index = frame_index;
    slot->batch = translated;
    slot->backend_index = static_cast<std::uint32_t>(backend_index);
    slot->phase_index = static_cast<std::uint32_t>(phase_index);
    slot->frame_index = frame_index;
    slot->sequence = control.next_sequence.fetch_add(
        1, std::memory_order_relaxed);
    slot->deadline_ns = monotonic_now_ns() + requested.timeout_ns;
    slot->early_completion = {};
    slot->early_completion_valid = false;
    slot->cancellation_requested.store(false, std::memory_order_relaxed);
    slot->rate_owned = rate_identity != nullptr;
    slot->terminal_status = Status::ok;
    slot->rate_identity = rate_identity
        ? *rate_identity
        : DeviceRateReleaseIdentity{};
    slot->graph_released.store(false, std::memory_order_relaxed);
    for (std::size_t index = 0; index < requested.signal_count; ++index) {
        const auto handle = DeviceTimelineHandle{
            requested.signals[index].timeline_handle};
        timelines_[handle.index()].last_accepted.store(
            requested.signals[index].value, std::memory_order_relaxed);
    }
    outstanding_count_.fetch_add(1, std::memory_order_release);
    slot->state.store(kBatchQueued, std::memory_order_release);
    release_admission();
    submissions_.fetch_add(1, std::memory_order_relaxed);
    out_batch_id = batch_id;
    if (rate_ticket) {
        rate_ticket->slot_index = static_cast<std::size_t>(
            slot - batch_slots_.get());
        rate_ticket->batch_id = batch_id;
        rate_ticket->identity = *rate_identity;
    }
    auto event = DeviceEvent{DeviceEventKind::submitted, Status::ok,
                             backend_index, phase_index, frame_index, batch_id,
                             0};
    event.producer = RuntimeTraceProducer::worker;
    event.worker_index = worker_index;
    emit(event);
    control.wake_sequence.fetch_add(1, std::memory_order_release);
    control.wake_sequence.notify_one();
    wake_sequence_.fetch_add(1, std::memory_order_release);
    wake_sequence_.notify_one();
    if (batch_stopping_.load(std::memory_order_acquire)) {
        auto expected = kBatchQueued;
        if (slot->state.compare_exchange_strong(
                expected, kBatchOwned, std::memory_order_acq_rel,
                std::memory_order_relaxed)) {
            finish_batch_slot(*slot, Status::device_canceled, false);
        }
    }
    return Status::ok;
}

void DeviceManager::emit(const DeviceEvent& event) noexcept {
    if (observer_) {
        observer_(observer_data_, event);
    }
}

void DeviceManager::process_completion(
    std::size_t backend_index,
    const HalV2Completion& completion) noexcept {
    if (!validate_hal_v2_completion(completion)) {
        fail_backend_outstanding(
            backend_index,
            Status::device_error);
        return;
    }
    for (std::size_t index = 0;
         index < outstanding_capacity_;
         ++index) {
        auto& slot = outstanding_slots_[index];
        auto state = slot.state.load(std::memory_order_acquire);
        if ((state != kOutstandingSubmitting &&
             state != kOutstandingSubmitted) ||
            slot.submission_id != completion.submission_id ||
            slot.backend_index != backend_index) {
            continue;
        }
        if (state == kOutstandingSubmitting) {
            slot.early_completion = completion;
            auto expected = kOutstandingSubmitting;
            if (slot.state.compare_exchange_strong(
                    expected,
                    kOutstandingEarlyReady,
                    std::memory_order_release,
                    std::memory_order_relaxed)) {
                return;
            }
            state = expected;
        }
        if (state == kOutstandingSubmitted) {
            auto expected = kOutstandingSubmitted;
            if (slot.state.compare_exchange_strong(
                    expected,
                    kOutstandingOwned,
                    std::memory_order_acq_rel,
                    std::memory_order_relaxed)) {
                finalize_completion(
                    slot,
                    backend_index,
                    completion);
                return;
            }
        }
        return;
    }
    failures_.fetch_add(1, std::memory_order_relaxed);
}

void DeviceManager::finalize_completion(
    Outstanding& slot,
    std::size_t backend_index,
    const HalV2Completion& completion) noexcept {
    const auto status =
        hal_v2_status_to_runtime(
            static_cast<HalV2Status>(completion.status));
    completions_.fetch_add(1, std::memory_order_relaxed);
    if (status != Status::ok) {
        record_failure(status);
    }
    emit(DeviceEvent{
        DeviceEventKind::completed,
        status,
        backend_index,
        slot.phase_index,
        slot.frame_index,
        slot.submission_id,
        completion.device_timestamp_ns,
    });
    outstanding_count_.fetch_sub(1, std::memory_order_release);
    slot.state.store(kOutstandingFree, std::memory_order_release);
    if (executor_) {
        (void)executor_->complete_external(
            slot.phase_index,
            status);
    }
}

void DeviceManager::fail_backend_outstanding(
    std::size_t backend_index,
    Status status) noexcept {
    for (std::size_t index = 0;
         index < outstanding_capacity_;
         ++index) {
        auto& slot = outstanding_slots_[index];
        auto state = slot.state.load(std::memory_order_acquire);
        if ((state != kOutstandingSubmitting &&
             state != kOutstandingSubmitted) ||
            slot.backend_index != backend_index) {
            continue;
        }
        if (state == kOutstandingSubmitting) {
            HalV2Completion completion;
            completion.status = static_cast<std::int32_t>(
                runtime_status_to_hal(status));
            completion.submission_id = slot.submission_id;
            slot.early_completion = completion;
            auto expected = kOutstandingSubmitting;
            if (slot.state.compare_exchange_strong(
                    expected,
                    kOutstandingEarlyReady,
                    std::memory_order_release,
                    std::memory_order_relaxed)) {
                continue;
            }
            state = expected;
        }
        if (state != kOutstandingSubmitted) {
            continue;
        }
        auto expected = kOutstandingSubmitted;
        if (!slot.state.compare_exchange_strong(
                expected,
                kOutstandingOwned,
                std::memory_order_acq_rel,
                std::memory_order_relaxed)) {
            continue;
        }
        record_failure(status);
        emit(DeviceEvent{
            DeviceEventKind::completed,
            status,
            backend_index,
            slot.phase_index,
            slot.frame_index,
            slot.submission_id,
            0,
        });
        if (executor_) {
            (void)executor_->complete_external(
                slot.phase_index,
                status);
        }
        outstanding_count_.fetch_sub(1, std::memory_order_release);
        slot.state.store(kOutstandingFree, std::memory_order_release);
    }
}

void DeviceManager::record_failure(Status status) noexcept {
    failures_.fetch_add(1, std::memory_order_relaxed);
    if (status == Status::device_timeout) {
        timeouts_.fetch_add(1, std::memory_order_relaxed);
    }
    if (status == Status::device_lost) {
        losses_.fetch_add(1, std::memory_order_relaxed);
    }
}

void DeviceManager::finish_batch_slot(
    BatchSlot& slot,
    Status status,
    bool publish_timeline,
    const HalV2BatchCompletion* completion) noexcept {
    if (publish_timeline) {
        for (std::size_t index = 0; index < slot.batch.signal_count; ++index) {
            const auto handle = DeviceTimelineHandle{
                slot.batch.signals[index].timeline_handle};
            if (handle.owner() != owner_ || handle.index() >= timeline_count_) {
                status = Status::device_error;
                publish_timeline = false;
                break;
            }
        }
    }
    if (publish_timeline) {
        for (std::size_t index = 0; index < slot.batch.signal_count; ++index) {
            const auto handle = DeviceTimelineHandle{
                slot.batch.signals[index].timeline_handle};
            auto& completed = timelines_[handle.index()].completed;
            auto current = completed.load(std::memory_order_relaxed);
            while (current < slot.batch.signals[index].value &&
                   !completed.compare_exchange_weak(
                       current, slot.batch.signals[index].value,
                       std::memory_order_release,
                       std::memory_order_relaxed)) {
            }
        }
    }
    completions_.fetch_add(1, std::memory_order_relaxed);
    if (status != Status::ok) {
        record_failure(status);
    }
    emit(DeviceEvent{DeviceEventKind::completed, status, slot.backend_index,
                     slot.phase_index, slot.frame_index, slot.batch.batch_id,
                     0});
    if (slot.rate_owned) {
        slot.graph_released.store(true, std::memory_order_release);
        slot.terminal_status = status;
        if (status == Status::ok && completion) {
            slot.early_completion = *completion;
            slot.early_completion_valid = true;
        }
        slot.state.store(kBatchRateTerminal, std::memory_order_release);
        slot.state.notify_all();
        return;
    }
    if (!slot.graph_released.exchange(true, std::memory_order_acq_rel) &&
        executor_) {
        (void)executor_->complete_external(slot.phase_index, status);
    }
    outstanding_count_.fetch_sub(1, std::memory_order_release);
    const auto backend_index = static_cast<std::size_t>(slot.backend_index);
    slot.state.store(kBatchFree, std::memory_order_release);
    if (backend_index < backends_.size()) {
        batch_backends_[backend_index].wake_sequence.fetch_add(
            1, std::memory_order_release);
        batch_backends_[backend_index].wake_sequence.notify_all();
    }
}

void DeviceManager::finish_rate_quarantine(
    BatchSlot& slot,
    Status status) noexcept {
    completions_.fetch_add(1, std::memory_order_relaxed);
    if (status != Status::ok) {
        record_failure(status);
    }
    emit(DeviceEvent{DeviceEventKind::completed, status, slot.backend_index,
                     slot.phase_index, slot.frame_index, slot.batch.batch_id,
                     0});
    slot.graph_released.store(true, std::memory_order_release);
    slot.terminal_status = status;
    slot.early_completion_valid = false;
    slot.state.store(kBatchRateQuarantined, std::memory_order_release);
    slot.state.notify_all();
}

Status DeviceManager::wait_rate_batch(
    const DeviceRateTicket& ticket,
    DeviceRateCompletion& completion) noexcept {
    completion = {};
    if (!ticket.valid() || ticket.slot_index >= batch_slot_count_) {
        return Status::invalid_argument;
    }
    auto& slot = batch_slots_[ticket.slot_index];
    const auto identity_matches = [&] {
        return slot.rate_owned && slot.batch.batch_id == ticket.batch_id &&
               slot.rate_identity.reference_index ==
                   ticket.identity.reference_index &&
               slot.rate_identity.domain == ticket.identity.domain &&
               slot.rate_identity.phase == ticket.identity.phase &&
               slot.rate_identity.supercycle_cycle ==
                   ticket.identity.supercycle_cycle &&
               slot.rate_identity.domain_release_sequence ==
                   ticket.identity.domain_release_sequence &&
               slot.rate_identity.substep_ordinal ==
                   ticket.identity.substep_ordinal &&
               slot.rate_identity.logical_release_ns ==
                   ticket.identity.logical_release_ns &&
               slot.rate_identity.nominal_release_ns ==
                   ticket.identity.nominal_release_ns &&
               slot.rate_identity.absolute_deadline_ns ==
                   ticket.identity.absolute_deadline_ns &&
               slot.rate_identity.completion_budget_ns ==
                   ticket.identity.completion_budget_ns;
    };
    auto state = slot.state.load(std::memory_order_acquire);
    if (state == kBatchFree || state == kBatchReserved ||
        !identity_matches()) {
        return Status::invalid_handle;
    }
    for (;;) {
        state = slot.state.load(std::memory_order_acquire);
        if (!identity_matches()) {
            return Status::invalid_handle;
        }
        if (state == kBatchRateTerminal) {
            auto expected = kBatchRateTerminal;
            if (!slot.state.compare_exchange_strong(
                    expected, kBatchOwned, std::memory_order_acq_rel,
                    std::memory_order_relaxed)) {
                continue;
            }
            const auto result = slot.terminal_status;
            completion.status = result;
            completion.timestamp_domain_identity =
                slot.early_completion_valid
                ? slot.early_completion.timestamp_domain_identity
                : 0;
            completion.timestamp = slot.early_completion_valid
                ? slot.early_completion.device_timestamp
                : 0;
            completion.terminal_slot_owned = true;
            return result;
        }
        if (state == kBatchRateQuarantined) {
            completion.status = slot.terminal_status;
            return slot.terminal_status;
        }
        if (state == kBatchFree || state == kBatchReserved) {
            return Status::invalid_handle;
        }
        std::this_thread::yield();
    }
}

Status DeviceManager::release_rate_batch(
    const DeviceRateTicket& ticket) noexcept {
    if (!ticket.valid() || ticket.slot_index >= batch_slot_count_) {
        return Status::invalid_argument;
    }
    auto& slot = batch_slots_[ticket.slot_index];
    if (slot.state.load(std::memory_order_acquire) != kBatchOwned ||
        !slot.rate_owned || slot.batch.batch_id != ticket.batch_id ||
        slot.rate_identity.reference_index != ticket.identity.reference_index ||
        slot.rate_identity.domain != ticket.identity.domain ||
        slot.rate_identity.phase != ticket.identity.phase ||
        slot.rate_identity.supercycle_cycle !=
            ticket.identity.supercycle_cycle ||
        slot.rate_identity.domain_release_sequence !=
            ticket.identity.domain_release_sequence ||
        slot.rate_identity.substep_ordinal != ticket.identity.substep_ordinal) {
        return Status::invalid_handle;
    }
    outstanding_count_.fetch_sub(1, std::memory_order_release);
    const auto backend_index = static_cast<std::size_t>(slot.backend_index);
    slot.rate_owned = false;
    slot.early_completion_valid = false;
    slot.state.store(kBatchFree, std::memory_order_release);
    if (backend_index < backends_.size()) {
        batch_backends_[backend_index].wake_sequence.fetch_add(
            1, std::memory_order_release);
        batch_backends_[backend_index].wake_sequence.notify_all();
    }
    return Status::ok;
}

void DeviceManager::fail_backend_batches(
    std::size_t backend_index, Status status) noexcept {
    if (backend_index >= backends_.size() ||
        !backends_[backend_index].command_state) {
        return;
    }
    auto& control = batch_backends_[backend_index];
    for (std::size_t offset = 0; offset < control.slot_count; ++offset) {
        auto& slot = batch_slots_[control.slot_offset + offset];
        auto state = slot.state.load(std::memory_order_acquire);
        if (state == kBatchFree || state == kBatchReserved ||
            state == kBatchOwned) {
            continue;
        }
        if (state == kBatchSubmitting) {
            HalV2BatchCompletion completion;
            completion.status = static_cast<std::int32_t>(
                runtime_status_to_hal(status));
            completion.batch_id = slot.batch.batch_id;
            completion.signal_count = slot.batch.signal_count;
            const auto* memory = backends_[backend_index].memory_state;
            completion.timestamp_domain_identity = memory
                ? memory->snapshot.completion_timestamp_domain_identity
                : 0;
            std::copy_n(slot.batch.signals.begin(), slot.batch.signal_count,
                        completion.signals.begin());
            slot.early_completion = completion;
            slot.early_completion_valid = false;
            auto expected = kBatchSubmitting;
            if (slot.state.compare_exchange_strong(
                    expected, kBatchEarlyReady, std::memory_order_release,
                    std::memory_order_relaxed)) {
                continue;
            }
            state = expected;
        }
        if (state == kBatchQueued || state == kBatchSubmitted) {
            auto expected = state;
            if (slot.state.compare_exchange_strong(
                    expected, kBatchOwned, std::memory_order_acq_rel,
                    std::memory_order_relaxed)) {
                finish_batch_slot(slot, status, false);
            }
        } else if (state == kBatchCompletionReady) {
            auto expected = kBatchCompletionReady;
            if (slot.state.compare_exchange_strong(
                    expected, kBatchOwned, std::memory_order_acq_rel,
                    std::memory_order_relaxed)) {
                finish_batch_slot(
                    slot,
                    hal_v2_status_to_runtime(static_cast<HalV2Status>(
                        slot.early_completion.status)),
                    slot.early_completion_valid,
                    slot.early_completion_valid
                        ? &slot.early_completion
                        : nullptr);
            }
        }
    }
}

void DeviceManager::submission_loop(std::size_t backend_index) noexcept {
    auto& control = batch_backends_[backend_index];
    auto& extension = backends_[backend_index].command_state->extension;
    while (!batch_stopping_.load(std::memory_order_acquire)) {
        auto* selected = select_device_submission_or_wait(
            control.wake_sequence, batch_stopping_, [&]() noexcept {
                BatchSlot* candidate = nullptr;
                std::uint64_t selected_sequence =
                    std::numeric_limits<std::uint64_t>::max();
                for (std::size_t offset = 0; offset < control.slot_count; ++offset) {
                    auto& slot = batch_slots_[control.slot_offset + offset];
                    if (slot.state.load(std::memory_order_acquire) == kBatchQueued &&
                        slot.sequence < selected_sequence) {
                        candidate = &slot;
                        selected_sequence = slot.sequence;
                    }
                }
                return candidate;
            });
        if (!selected) {
            continue;
        }
        bool waits_ready = true;
        for (std::size_t index = 0; index < selected->batch.wait_count; ++index) {
            const auto handle = DeviceTimelineHandle{
                selected->batch.waits[index].timeline_handle};
            if (handle.owner() != owner_ || handle.index() >= timeline_count_ ||
                timelines_[handle.index()].completed.load(
                    std::memory_order_acquire) <
                    selected->batch.waits[index].value) {
                waits_ready = false;
                break;
            }
        }
        if (!waits_ready) {
            if (monotonic_now_ns() >= selected->deadline_ns) {
                auto expected = kBatchQueued;
                if (selected->state.compare_exchange_strong(
                        expected, kBatchOwned, std::memory_order_acq_rel,
                        std::memory_order_relaxed)) {
                    finish_batch_slot(*selected, Status::device_timeout, false);
                }
            } else if (wait_strategy_ == WaitStrategy::yield) {
                std::this_thread::yield();
            } else {
                rt::cpu_relax();
            }
            continue;
        }
        auto expected = kBatchQueued;
        if (!selected->state.compare_exchange_strong(
                expected, kBatchSubmitting, std::memory_order_acq_rel,
                std::memory_order_relaxed)) {
            continue;
        }
        HalV2Status callback_status = HalV2Status::internal_error;
        try {
            callback_status = extension.submit(
                extension.instance, &selected->batch);
        } catch (...) {
            callback_status = HalV2Status::internal_error;
        }
        const auto status = hal_v2_status_to_runtime(callback_status);
        if (status != Status::ok) {
            auto prior = kBatchSubmitting;
            if (selected->state.compare_exchange_strong(
                    prior, kBatchOwned, std::memory_order_acq_rel,
                    std::memory_order_relaxed)) {
                finish_batch_slot(*selected, status, false);
            } else if (prior == kBatchEarlyReady &&
                       selected->state.compare_exchange_strong(
                           prior, kBatchOwned, std::memory_order_acq_rel,
                           std::memory_order_relaxed)) {
                finish_batch_slot(*selected, Status::device_error, false);
            }
            continue;
        }
        expected = kBatchSubmitting;
        if (selected->state.compare_exchange_strong(
                expected, kBatchSubmitted, std::memory_order_acq_rel,
                std::memory_order_relaxed)) {
            continue;
        }
        if (expected == kBatchEarlyReady) {
            expected = kBatchEarlyReady;
            if (selected->state.compare_exchange_strong(
                    expected, kBatchCompletionReady, std::memory_order_acq_rel,
                    std::memory_order_relaxed)) {
                wake_sequence_.fetch_add(1, std::memory_order_release);
                wake_sequence_.notify_one();
            }
        }
    }
}

void DeviceManager::submission_entry(void* context) noexcept {
    auto* lane = static_cast<SubmissionLaneContext*>(context);
    lane->manager->submission_loop(lane->backend_index);
}

void DeviceManager::process_batch_completion(
    std::size_t backend_index,
    const HalV2BatchCompletion& completion) noexcept {
    auto& control = batch_backends_[backend_index];
    for (std::size_t offset = 0; offset < control.slot_count; ++offset) {
        auto& slot = batch_slots_[control.slot_offset + offset];
        auto state = slot.state.load(std::memory_order_acquire);
        if (slot.batch.batch_id == completion.batch_id &&
            (state == kBatchRateQuarantineOwned ||
             state == kBatchRateQuarantined)) {
            return;
        }
        if (slot.batch.batch_id != completion.batch_id ||
            (state != kBatchSubmitting && state != kBatchSubmitted)) {
            continue;
        }
        if (state == kBatchSubmitting) {
            slot.early_completion = completion;
            slot.early_completion_valid = true;
            auto expected = kBatchSubmitting;
            if (slot.state.compare_exchange_strong(
                    expected, kBatchEarlyReady, std::memory_order_release,
                    std::memory_order_relaxed)) {
                return;
            }
            state = expected;
        }
        if (state == kBatchSubmitted) {
            auto expected = kBatchSubmitted;
            if (slot.state.compare_exchange_strong(
                    expected, kBatchOwned, std::memory_order_acq_rel,
                    std::memory_order_relaxed)) {
                finish_batch_slot(
                    slot,
                    hal_v2_status_to_runtime(
                        static_cast<HalV2Status>(completion.status)),
                    true,
                    &completion);
            }
        }
        return;
    }
}

void DeviceManager::poll_batch_completions(
    std::size_t backend_index) noexcept {
    auto* command_state = backends_[backend_index].command_state;
    if (!command_state) {
        return;
    }
    std::fill_n(batch_completion_buffer_.get(), completion_batch_,
                HalV2BatchCompletion{});
    for (std::size_t index = 0; index < completion_batch_; ++index) {
        batch_completion_buffer_[index].struct_size = 0;
    }
    std::uint64_t count = 0;
    HalV2Status callback_status = HalV2Status::internal_error;
    try {
        callback_status = command_state->extension.poll(
            command_state->extension.instance, batch_completion_buffer_.get(),
            completion_batch_, &count);
    } catch (...) {
        callback_status = HalV2Status::internal_error;
    }
    service_polls_.fetch_add(1, std::memory_order_relaxed);
    const auto callback_runtime_status =
        hal_v2_status_to_runtime(callback_status);
    if (callback_runtime_status != Status::ok || count > completion_batch_) {
        fail_backend_batches(
            backend_index, callback_runtime_status == Status::ok
                               ? Status::device_error
                               : callback_runtime_status);
        return;
    }
    bool valid = true;
    auto& control = batch_backends_[backend_index];
    const auto timestamp_domain = backends_[backend_index].memory_state
        ? backends_[backend_index]
              .memory_state->snapshot.completion_timestamp_domain_identity
        : 0;
    for (std::size_t completion_index = 0;
         completion_index < static_cast<std::size_t>(count); ++completion_index) {
        const auto& completion = batch_completion_buffer_[completion_index];
        if (!validate_batch_completion(completion) ||
            completion.timestamp_domain_identity != timestamp_domain) {
            valid = false;
            break;
        }
        for (std::size_t prior = 0; prior < completion_index; ++prior) {
            if (batch_completion_buffer_[prior].batch_id == completion.batch_id) {
                valid = false;
                break;
            }
        }
        BatchSlot* matched = nullptr;
        for (std::size_t offset = 0; valid && offset < control.slot_count;
             ++offset) {
            auto& slot = batch_slots_[control.slot_offset + offset];
            const auto state = slot.state.load(std::memory_order_acquire);
            if (slot.batch.batch_id == completion.batch_id &&
                (state == kBatchSubmitting || state == kBatchSubmitted ||
                 state == kBatchRateQuarantineOwned ||
                 state == kBatchRateQuarantined)) {
                matched = &slot;
                break;
            }
        }
        if (!matched || completion.signal_count != matched->batch.signal_count) {
            valid = false;
            break;
        }
        for (std::size_t signal = 0; signal < completion.signal_count; ++signal) {
            if (completion.signals[signal].timeline_handle !=
                    matched->batch.signals[signal].timeline_handle ||
                completion.signals[signal].value !=
                    matched->batch.signals[signal].value) {
                valid = false;
                break;
            }
        }
    }
    if (!valid) {
        fail_backend_batches(backend_index, Status::device_error);
        return;
    }
    for (std::size_t completion_index = 0;
         completion_index < static_cast<std::size_t>(count); ++completion_index) {
        process_batch_completion(
            backend_index, batch_completion_buffer_[completion_index]);
    }
}

void DeviceManager::service_loop() noexcept {
    service_starts_.fetch_add(1, std::memory_order_release);
    service_ready_.store(true, std::memory_order_release);
    while (!stopping_.load(std::memory_order_acquire)) {
        if (outstanding_count_.load(std::memory_order_acquire) == 0) {
            if (wait_strategy_ == WaitStrategy::spin) {
                rt::cpu_relax();
                continue;
            }
            if (wait_strategy_ == WaitStrategy::yield) {
                std::this_thread::yield();
                continue;
            }
            const auto observed =
                wake_sequence_.load(std::memory_order_acquire);
            if (!stopping_.load(std::memory_order_acquire) &&
                outstanding_count_.load(std::memory_order_acquire) == 0) {
                wake_sequence_.wait(
                    observed,
                    std::memory_order_relaxed);
            }
            continue;
        }
        for (std::size_t backend_index = 0;
             backend_index < backends_.size();
             ++backend_index) {
            auto& batch_control = batch_backends_[backend_index];
            for (std::size_t offset = 0; offset < batch_control.slot_count;
                 ++offset) {
                auto& slot = batch_slots_[batch_control.slot_offset + offset];
                auto expected = kBatchCompletionReady;
                if (slot.state.compare_exchange_strong(
                        expected, kBatchOwned, std::memory_order_acq_rel,
                        std::memory_order_relaxed)) {
                    finish_batch_slot(
                        slot,
                        hal_v2_status_to_runtime(static_cast<HalV2Status>(
                            slot.early_completion.status)),
                        slot.early_completion_valid,
                        slot.early_completion_valid
                            ? &slot.early_completion
                            : nullptr);
                }
            }
            std::fill_n(
                completion_buffer_.get(),
                completion_batch_,
                HalV2Completion{});
            for (std::size_t index = 0; index < completion_batch_; ++index) {
                completion_buffer_[index].struct_size = 0;
            }
            std::uint64_t count = 0;
            HalV2Status device_status = HalV2Status::internal_error;
            try {
                device_status = backends_[backend_index].api.poll(
                    backends_[backend_index].api.instance,
                    completion_buffer_.get(),
                    completion_batch_,
                    &count);
            } catch (...) {
                device_status = HalV2Status::internal_error;
            }
            service_polls_.fetch_add(1, std::memory_order_relaxed);
            const auto status =
                hal_v2_status_to_runtime(device_status);
            if (status != Status::ok ||
                count > completion_batch_) {
                fail_backend_outstanding(
                    backend_index,
                    status == Status::ok
                        ? Status::device_error
                        : status);
                continue;
            }
            bool batch_valid = true;
            for (std::size_t completion_index = 0;
                 completion_index < static_cast<std::size_t>(count);
                 ++completion_index) {
                const auto& completion =
                    completion_buffer_[completion_index];
                if (!validate_hal_v2_completion(completion)) {
                    batch_valid = false;
                    break;
                }
                for (std::size_t prior = 0;
                     prior < completion_index;
                     ++prior) {
                    if (completion_buffer_[prior].submission_id ==
                        completion.submission_id) {
                        batch_valid = false;
                        break;
                    }
                }
                bool matched = false;
                for (std::size_t slot_index = 0;
                     batch_valid && slot_index < outstanding_capacity_;
                     ++slot_index) {
                    const auto& slot = outstanding_slots_[slot_index];
                    const auto slot_state =
                        slot.state.load(std::memory_order_acquire);
                    if ((slot_state == kOutstandingSubmitting ||
                         slot_state == kOutstandingSubmitted) &&
                        slot.backend_index == backend_index &&
                        slot.submission_id == completion.submission_id) {
                        matched = true;
                        break;
                    }
                }
                if (!matched) {
                    batch_valid = false;
                }
                if (!batch_valid) {
                    break;
                }
            }
            if (!batch_valid) {
                fail_backend_outstanding(
                    backend_index,
                    Status::device_error);
                continue;
            }
            for (std::size_t completion_index = 0;
                 completion_index <
                     static_cast<std::size_t>(count);
                 ++completion_index) {
                process_completion(
                    backend_index,
                    completion_buffer_[completion_index]);
            }
            poll_batch_completions(backend_index);
        }
        for (std::size_t backend_index = 0; backend_index < backends_.size();
             ++backend_index) {
            auto& control = batch_backends_[backend_index];
            for (std::size_t offset = 0; offset < control.slot_count; ++offset) {
                auto& slot = batch_slots_[control.slot_offset + offset];
                const auto state = slot.state.load(std::memory_order_acquire);
                if ((state == kBatchSubmitted ||
                     (state == kBatchSubmitting && slot.rate_owned)) &&
                    monotonic_now_ns() >= slot.deadline_ns) {
                    auto expected = state;
                    const auto target = slot.rate_owned
                        ? kBatchRateQuarantineOwned
                        : kBatchOwned;
                    if (!slot.state.compare_exchange_strong(
                            expected, target, std::memory_order_acq_rel,
                            std::memory_order_relaxed)) {
                        continue;
                    }
                    if (!slot.cancellation_requested.exchange(
                            true, std::memory_order_acq_rel)) {
                        try {
                            (void)backends_[backend_index]
                                .command_state->extension.cancel(
                                    backends_[backend_index]
                                        .command_state->extension.instance,
                                    slot.batch.batch_id);
                        } catch (...) {
                        }
                    }
                    if (slot.rate_owned) {
                        finish_rate_quarantine(
                            slot, Status::device_timeout);
                    } else {
                        finish_batch_slot(
                            slot, Status::device_timeout, false);
                    }
                }
            }
        }
        rt::cpu_relax();
    }
}

void DeviceManager::service_entry(void* manager) noexcept {
    static_cast<DeviceManager*>(manager)->service_loop();
}

Status DeviceManager::health(
    std::size_t backend_index,
    DeviceHealth& output) noexcept {
    output = make_device_health();
    if (!started_.load(std::memory_order_acquire) ||
        backend_index >= backends_.size()) {
        return Status::invalid_state;
    }
    HalV2Health hal_health;
    hal_health.reserved0 = std::numeric_limits<std::uint32_t>::max();
    HalV2Status device_status = HalV2Status::internal_error;
    try {
        device_status = backends_[backend_index].api.get_health(
            backends_[backend_index].api.instance,
            &hal_health);
    } catch (...) {
        device_status = HalV2Status::internal_error;
    }
    const auto status = hal_v2_status_to_runtime(device_status);
    if (status != Status::ok) {
        return status;
    }
    if (!validate_hal_v2_health(hal_health)) {
        return Status::device_error;
    }
    output.state = hal_health.state;
    output.last_status = hal_health.last_status;
    output.generation = hal_health.generation;
    output.submissions = hal_health.submissions;
    output.completions = hal_health.completions;
    output.queue_rejections = hal_health.queue_rejections;
    output.timeouts = hal_health.timeouts;
    output.errors = hal_health.errors;
    output.losses = hal_health.losses;
    output.cancellations = hal_health.cancellations;
    output.resets = hal_health.resets;
    output.outstanding = hal_health.outstanding;
    return Status::ok;
}

Status DeviceManager::reset(std::size_t backend_index) noexcept {
    if (!started_.load(std::memory_order_acquire) ||
        backend_index >= backends_.size()) {
        return Status::invalid_state;
    }
    for (std::size_t index = 0;
         index < outstanding_capacity_;
         ++index) {
        if (outstanding_slots_[index].state.load(
                std::memory_order_acquire) ==
                kOutstandingSubmitted &&
            outstanding_slots_[index].backend_index ==
                backend_index) {
            return Status::invalid_state;
        }
    }
    if (backends_[backend_index].command_state) {
        const auto& control = batch_backends_[backend_index];
        for (std::size_t offset = 0; offset < control.slot_count; ++offset) {
            if (batch_slots_[control.slot_offset + offset].state.load(
                    std::memory_order_acquire) != kBatchFree) {
                return Status::invalid_state;
            }
        }
    }
    HalV2Status device_status = HalV2Status::internal_error;
    try {
        device_status = backends_[backend_index].api.reset(
            backends_[backend_index].api.instance);
    } catch (...) {
        device_status = HalV2Status::internal_error;
    }
    const auto status = hal_v2_status_to_runtime(device_status);
    if (status == Status::ok) {
        resets_.fetch_add(1, std::memory_order_relaxed);
        auto reset_event = DeviceEvent{
            DeviceEventKind::reset,
            status,
            backend_index,
            std::numeric_limits<std::size_t>::max(),
            0,
            0,
            0,
        };
        reset_event.producer = RuntimeTraceProducer::host;
        emit(reset_event);
    }
    return status;
}

DeviceManagerStats DeviceManager::stats() const noexcept {
    return DeviceManagerStats{
        submissions_.load(std::memory_order_acquire),
        completions_.load(std::memory_order_acquire),
        failures_.load(std::memory_order_acquire),
        queue_rejections_.load(std::memory_order_acquire),
        timeouts_.load(std::memory_order_acquire),
        losses_.load(std::memory_order_acquire),
        resets_.load(std::memory_order_acquire),
        service_polls_.load(std::memory_order_acquire),
        outstanding_count_.load(std::memory_order_acquire),
        service_starts_.load(std::memory_order_acquire),
    };
}

} // namespace rt::detail
