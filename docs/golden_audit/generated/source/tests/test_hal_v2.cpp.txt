#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

#include <rt/runtime.hpp>

#include "rt/src/hal_v2.hpp"

namespace {

using namespace std::chrono_literals;

rt::RuntimeConfig hal_config(std::size_t backend_capacity = 1) {
    rt::RuntimeConfig config;
    config.callback_capacity = 1;
    config.worker_count = 1;
    config.executor_queue_capacity = 4;
    config.task_scratch_slots = 4;
    config.trace_capacity = 32;
    config.device_backend_capacity = backend_capacity;
    config.device_buffer_capacity = 1;
    config.device_outstanding_capacity = 4;
    config.device_completion_batch = 4;
    return config;
}

template <typename Range>
bool all_zero(const Range& values) {
    return std::all_of(
        values.begin(), values.end(), [](const auto value) {
            return value == 0;
        });
}

template <typename Range>
void set_identifier(Range& destination, std::string_view value) {
    destination.fill(0);
    std::copy(value.begin(), value.end(), destination.begin());
}

struct NativeProbe {
    explicit NativeProbe(std::string_view identifier = "test.hal.native") {
        set_identifier(capabilities.backend_id, identifier);
    }

    rt::HalV2BackendApi api() {
        rt::HalV2BackendApi output;
        output.instance = this;
        output.get_capabilities = &get_capabilities;
        output.initialize = &initialize;
        output.register_buffer = &register_buffer;
        output.unregister_buffer = &unregister_buffer;
        output.submit = &submit;
        output.poll = &poll;
        output.cancel = &cancel;
        output.get_health = &get_health;
        output.reset = &reset;
        output.shutdown = &shutdown;
        return output;
    }

    rt::HalV2Capabilities capabilities = [] {
        rt::HalV2Capabilities output;
        output.max_in_flight = 8;
        output.max_registered_buffers = 8;
        output.max_buffer_bytes = 4096;
        output.supports_cancel = 1;
        output.supports_reset = 1;
        output.deterministic_mock = 1;
        output.control_storage_bytes = 37;
        return output;
    }();
    rt::HalV2Status capability_status = rt::HalV2Status::ok;
    bool throw_capabilities = false;
    bool throw_submit = false;
    std::atomic<std::size_t> capability_calls{0};
    std::atomic<std::size_t> initialize_calls{0};
    std::atomic<std::size_t> register_calls{0};
    std::atomic<std::size_t> unregister_calls{0};
    std::atomic<std::size_t> submit_calls{0};
    std::atomic<std::size_t> poll_calls{0};
    std::atomic<std::size_t> cancel_calls{0};
    std::atomic<std::size_t> health_calls{0};
    std::atomic<std::size_t> reset_calls{0};
    std::atomic<std::size_t> shutdown_calls{0};
    std::atomic<std::uint64_t> pending_submission{0};
    std::atomic<std::uint64_t> submissions{0};
    std::atomic<std::uint64_t> completions{0};
    rt::HalV2InitializeConfig observed_initialize{};
    rt::HalV2BufferRegistration observed_buffer{};
    rt::HalV2Submission observed_submission{};
    std::uint64_t observed_unregister_token = 0;
    std::uint64_t observed_cancel_id = 0;
    static constexpr std::uint64_t native_buffer_token = 0xabcdu;

    static NativeProbe* self(void* instance) {
        return static_cast<NativeProbe*>(instance);
    }

    static rt::HalV2Status get_capabilities(
        void* instance, rt::HalV2Capabilities* output) {
        auto* probe = self(instance);
        if (!probe || !output) {
            return rt::HalV2Status::invalid_argument;
        }
        probe->capability_calls.fetch_add(1, std::memory_order_relaxed);
        if (probe->throw_capabilities) {
            throw std::runtime_error("capabilities");
        }
        if (probe->capability_status != rt::HalV2Status::ok) {
            return probe->capability_status;
        }
        *output = probe->capabilities;
        return rt::HalV2Status::ok;
    }

    static rt::HalV2Status initialize(
        void* instance, const rt::HalV2InitializeConfig* config) {
        auto* probe = self(instance);
        if (!probe || !config) {
            return rt::HalV2Status::invalid_argument;
        }
        probe->initialize_calls.fetch_add(1, std::memory_order_relaxed);
        probe->observed_initialize = *config;
        return rt::HalV2Status::ok;
    }

    static rt::HalV2Status register_buffer(
        void* instance,
        const rt::HalV2BufferRegistration* registration,
        std::uint64_t* token) {
        auto* probe = self(instance);
        if (!probe || !registration || !token) {
            return rt::HalV2Status::invalid_argument;
        }
        probe->register_calls.fetch_add(1, std::memory_order_relaxed);
        probe->observed_buffer = *registration;
        *token = native_buffer_token;
        return rt::HalV2Status::ok;
    }

    static rt::HalV2Status unregister_buffer(
        void* instance, std::uint64_t token) {
        auto* probe = self(instance);
        if (!probe) {
            return rt::HalV2Status::invalid_argument;
        }
        probe->unregister_calls.fetch_add(1, std::memory_order_relaxed);
        probe->observed_unregister_token = token;
        return rt::HalV2Status::ok;
    }

    static rt::HalV2Status submit(
        void* instance, const rt::HalV2Submission* submission) {
        auto* probe = self(instance);
        if (!probe || !submission) {
            return rt::HalV2Status::invalid_argument;
        }
        probe->submit_calls.fetch_add(1, std::memory_order_relaxed);
        if (probe->throw_submit) {
            throw std::runtime_error("submit");
        }
        probe->observed_submission = *submission;
        probe->submissions.fetch_add(1, std::memory_order_relaxed);
        probe->pending_submission.store(
            submission->submission_id, std::memory_order_release);
        return rt::HalV2Status::ok;
    }

    static rt::HalV2Status poll(
        void* instance,
        rt::HalV2Completion* output,
        std::uint64_t capacity,
        std::uint64_t* count) {
        auto* probe = self(instance);
        if (!probe || !output || capacity == 0 || !count) {
            return rt::HalV2Status::invalid_argument;
        }
        probe->poll_calls.fetch_add(1, std::memory_order_relaxed);
        *count = 0;
        const auto submission = probe->pending_submission.exchange(
            0, std::memory_order_acq_rel);
        if (submission == 0) {
            return rt::HalV2Status::ok;
        }
        output[0] = {};
        output[0].submission_id = submission;
        output[0].device_timestamp_ns = 1234;
        output[0].value = 5678;
        *count = 1;
        probe->completions.fetch_add(1, std::memory_order_relaxed);
        return rt::HalV2Status::ok;
    }

    static rt::HalV2Status cancel(void* instance, std::uint64_t id) {
        auto* probe = self(instance);
        if (!probe) {
            return rt::HalV2Status::invalid_argument;
        }
        probe->cancel_calls.fetch_add(1, std::memory_order_relaxed);
        probe->observed_cancel_id = id;
        return rt::HalV2Status::unsupported;
    }

    static rt::HalV2Status get_health(
        void* instance, rt::HalV2Health* output) {
        auto* probe = self(instance);
        if (!probe || !output) {
            return rt::HalV2Status::invalid_argument;
        }
        probe->health_calls.fetch_add(1, std::memory_order_relaxed);
        *output = {};
        output->state = static_cast<std::uint32_t>(
            rt::HalV2HealthState::healthy);
        output->generation = 9;
        output->submissions = probe->submissions.load(
            std::memory_order_acquire);
        output->completions = probe->completions.load(
            std::memory_order_acquire);
        output->outstanding = probe->pending_submission.load(
            std::memory_order_acquire) == 0 ? 0u : 1u;
        return rt::HalV2Status::ok;
    }

    static rt::HalV2Status reset(void* instance) {
        auto* probe = self(instance);
        if (!probe) {
            return rt::HalV2Status::invalid_argument;
        }
        probe->reset_calls.fetch_add(1, std::memory_order_relaxed);
        return rt::HalV2Status::ok;
    }

    static rt::HalV2Status shutdown(void* instance) {
        auto* probe = self(instance);
        if (!probe) {
            return rt::HalV2Status::invalid_argument;
        }
        probe->shutdown_calls.fetch_add(1, std::memory_order_relaxed);
        return rt::HalV2Status::ok;
    }
};

struct V1Probe {
    explicit V1Probe(std::string_view identifier = "test.hal.v1") {
        std::fill(std::begin(backend_id), std::end(backend_id), 0);
        std::copy(identifier.begin(), identifier.end(), std::begin(backend_id));
        completion.struct_size = sizeof(completion);
        completion.status = RTFW_DEVICE_STATUS_TIMEOUT;
        completion.submission_id = 77;
        completion.device_timestamp_ns = 88;
        completion.value = 99;
        health.struct_size = sizeof(health);
        health.state = RTFW_DEVICE_HEALTH_DEGRADED;
        health.last_status = RTFW_DEVICE_STATUS_RESET_REQUIRED;
        health.generation = 1;
        health.submissions = 2;
        health.completions = 3;
        health.queue_rejections = 4;
        health.timeouts = 5;
        health.errors = 6;
        health.losses = 7;
        health.cancellations = 8;
        health.resets = 9;
        health.outstanding = 10;
    }

    rtfw_device_backend_api api() {
        rtfw_device_backend_api output{};
        output.struct_size = sizeof(output);
        output.abi_version = RTFW_DEVICE_ABI_VERSION;
        output.instance = this;
        output.get_capabilities = &get_capabilities;
        output.initialize = &initialize;
        output.register_buffer = &register_buffer;
        output.unregister_buffer = &unregister_buffer;
        output.submit = &submit;
        output.poll = &poll;
        output.cancel = &cancel;
        output.get_health = &get_health;
        output.reset = &reset;
        output.shutdown = &shutdown;
        return output;
    }

    char backend_id[RTFW_DEVICE_IDENTIFIER_CAPACITY]{};
    rtfw_device_status capability_status = RTFW_DEVICE_STATUS_OK;
    bool throw_capabilities = false;
    bool throw_poll = false;
    std::atomic<std::size_t> capability_calls{0};
    std::atomic<std::size_t> initialize_calls{0};
    std::atomic<std::size_t> register_calls{0};
    std::atomic<std::size_t> unregister_calls{0};
    std::atomic<std::size_t> submit_calls{0};
    std::atomic<std::size_t> poll_calls{0};
    std::atomic<std::size_t> cancel_calls{0};
    std::atomic<std::size_t> health_calls{0};
    std::atomic<std::size_t> reset_calls{0};
    std::atomic<std::size_t> shutdown_calls{0};
    rtfw_device_status cancel_status = RTFW_DEVICE_STATUS_UNSUPPORTED;
    rtfw_device_status register_status = RTFW_DEVICE_STATUS_OK;
    rtfw_device_status reset_status = RTFW_DEVICE_STATUS_OK;
    rtfw_device_status shutdown_status = RTFW_DEVICE_STATUS_OK;
    bool throw_register = false;
    bool runtime_poll = false;
    std::uint64_t buffer_token = 0x4455u;
    std::atomic<std::uint64_t> pending_submission{0};
    rtfw_device_init_config observed_initialize{};
    rtfw_device_buffer_registration observed_buffer{};
    rtfw_device_submission observed_submission{};
    std::uint64_t observed_unregister_token = 0;
    std::uint64_t observed_cancel_id = 0;
    rtfw_device_completion completion{};
    rtfw_device_health health{};

    static V1Probe* self(void* instance) {
        return static_cast<V1Probe*>(instance);
    }

    static rtfw_device_status get_capabilities(
        void* instance, rtfw_device_capabilities* output) {
        auto* probe = self(instance);
        if (!probe || !output) {
            return RTFW_DEVICE_STATUS_INVALID_ARGUMENT;
        }
        probe->capability_calls.fetch_add(1, std::memory_order_relaxed);
        if (probe->throw_capabilities) {
            throw std::runtime_error("v1 capabilities");
        }
        if (probe->capability_status != RTFW_DEVICE_STATUS_OK) {
            return probe->capability_status;
        }
        *output = {};
        output->struct_size = sizeof(*output);
        output->abi_version = RTFW_DEVICE_ABI_VERSION;
        output->max_in_flight = 8;
        output->max_registered_buffers = 8;
        output->max_buffer_bytes = 4096;
        output->inline_payload_capacity =
            RTFW_DEVICE_INLINE_PAYLOAD_CAPACITY;
        output->buffer_ref_capacity = RTFW_DEVICE_BUFFER_REF_CAPACITY;
        output->supports_cancel = 1;
        output->supports_reset = 1;
        output->deterministic_mock = 1;
        output->control_storage_bytes = 37;
        std::copy(
            std::begin(probe->backend_id),
            std::end(probe->backend_id),
            std::begin(output->backend_id));
        return RTFW_DEVICE_STATUS_OK;
    }

    static rtfw_device_status initialize(
        void* instance, const rtfw_device_init_config* config) {
        auto* probe = self(instance);
        if (!probe || !config) {
            return RTFW_DEVICE_STATUS_INVALID_ARGUMENT;
        }
        probe->initialize_calls.fetch_add(1, std::memory_order_relaxed);
        probe->observed_initialize = *config;
        return RTFW_DEVICE_STATUS_OK;
    }

    static rtfw_device_status register_buffer(
        void* instance,
        const rtfw_device_buffer_registration* registration,
        std::uint64_t* token) {
        auto* probe = self(instance);
        if (!probe || !registration || !token) {
            return RTFW_DEVICE_STATUS_INVALID_ARGUMENT;
        }
        probe->register_calls.fetch_add(1, std::memory_order_relaxed);
        probe->observed_buffer = *registration;
        *token = probe->buffer_token;
        if (probe->throw_register) {
            throw std::runtime_error("v1 register buffer");
        }
        return probe->register_status;
    }

    static rtfw_device_status unregister_buffer(
        void* instance, std::uint64_t token) {
        auto* probe = self(instance);
        if (!probe) {
            return RTFW_DEVICE_STATUS_INVALID_ARGUMENT;
        }
        probe->unregister_calls.fetch_add(1, std::memory_order_relaxed);
        probe->observed_unregister_token = token;
        return RTFW_DEVICE_STATUS_OK;
    }

    static rtfw_device_status submit(
        void* instance, const rtfw_device_submission* submission) {
        auto* probe = self(instance);
        if (!probe || !submission) {
            return RTFW_DEVICE_STATUS_INVALID_ARGUMENT;
        }
        probe->submit_calls.fetch_add(1, std::memory_order_relaxed);
        probe->observed_submission = *submission;
        if (probe->runtime_poll) {
            probe->pending_submission.store(
                submission->submission_id, std::memory_order_release);
        }
        return RTFW_DEVICE_STATUS_OK;
    }

    static rtfw_device_status poll(
        void* instance,
        rtfw_device_completion* output,
        std::uint64_t capacity,
        std::uint64_t* count) {
        auto* probe = self(instance);
        if (!probe || !output || capacity == 0 || !count) {
            return RTFW_DEVICE_STATUS_INVALID_ARGUMENT;
        }
        probe->poll_calls.fetch_add(1, std::memory_order_relaxed);
        if (probe->throw_poll) {
            throw std::runtime_error("v1 poll");
        }
        output[0] = probe->completion;
        if (probe->runtime_poll) {
            const auto submission_id = probe->pending_submission.exchange(
                0, std::memory_order_acq_rel);
            if (submission_id == 0) {
                *count = 0;
                return RTFW_DEVICE_STATUS_OK;
            }
            output[0].submission_id = submission_id;
        }
        *count = 1;
        return RTFW_DEVICE_STATUS_OK;
    }

    static rtfw_device_status cancel(void* instance, std::uint64_t id) {
        auto* probe = self(instance);
        if (!probe) {
            return RTFW_DEVICE_STATUS_INVALID_ARGUMENT;
        }
        probe->cancel_calls.fetch_add(1, std::memory_order_relaxed);
        probe->observed_cancel_id = id;
        return probe->cancel_status;
    }

    static rtfw_device_status get_health(
        void* instance, rtfw_device_health* output) {
        auto* probe = self(instance);
        if (!probe || !output) {
            return RTFW_DEVICE_STATUS_INVALID_ARGUMENT;
        }
        probe->health_calls.fetch_add(1, std::memory_order_relaxed);
        *output = probe->health;
        return RTFW_DEVICE_STATUS_OK;
    }

    static rtfw_device_status reset(void* instance) {
        auto* probe = self(instance);
        if (!probe) {
            return RTFW_DEVICE_STATUS_INVALID_ARGUMENT;
        }
        probe->reset_calls.fetch_add(1, std::memory_order_relaxed);
        return probe->reset_status;
    }

    static rtfw_device_status shutdown(void* instance) {
        auto* probe = self(instance);
        if (!probe) {
            return RTFW_DEVICE_STATUS_INVALID_ARGUMENT;
        }
        probe->shutdown_calls.fetch_add(1, std::memory_order_relaxed);
        return probe->shutdown_status;
    }
};

struct SubmissionRequest {
    rt::DeviceBufferHandle buffer{};
};

rt::CallbackResult prepare_submission(
    void* user_data,
    const rt::DeviceCallbackContext&,
    rt::DeviceSubmission& submission) {
    const auto& request = *static_cast<SubmissionRequest*>(user_data);
    submission.timeout_ns = 99'000;
    submission.opcode = 0x1234u;
    submission.payload_size = 3;
    submission.payload[0] = 0x11u;
    submission.payload[1] = 0x22u;
    submission.payload[2] = 0x33u;
    submission.buffer_count = 1;
    submission.buffers[0].buffer_token = request.buffer.value;
    submission.buffers[0].access = RTFW_DEVICE_ACCESS_READ_WRITE;
    submission.buffers[0].offset = 2;
    submission.buffers[0].bytes = 5;
    return rt::CallbackResult::ok;
}

rt::CallbackResult prepare_no_buffer_submission(
    void*,
    const rt::DeviceCallbackContext&,
    rt::DeviceSubmission& submission) {
    submission.timeout_ns = 99'000;
    submission.opcode = 0x1234u;
    return rt::CallbackResult::ok;
}

rt::Status register_native(
    rt::Runtime& runtime,
    NativeProbe& probe,
    std::string_view name,
    rt::DeviceBackendHandle& handle) {
    return runtime.register_device_backend(
        rt::HalV2BackendRegistration{name, probe.api()}, handle);
}

rt::CheckpointMetadata checkpoint_metadata(rt::Runtime& runtime) {
    std::array<std::byte, 1024> bytes{};
    rt::ArtifactWriteResult result;
    EXPECT_EQ(
        runtime.write_checkpoint(0, bytes, result),
        rt::Status::ok);
    rt::CheckpointMetadata output;
    EXPECT_EQ(
        rt::inspect_checkpoint_artifact(
            std::span<const std::byte>(bytes.data(), result.bytes_written),
            output),
        rt::Status::ok);
    return output;
}

} // namespace

TEST(HalV2, ApiVersionDefaultsAndLayoutsAreExact) {
    EXPECT_EQ(rt::hal_v2_api_version, 2u);
    EXPECT_EQ(rt::hal_v2_identifier_capacity, 64u);
    EXPECT_EQ(rt::hal_v2_inline_payload_capacity, 128u);
    EXPECT_EQ(rt::hal_v2_buffer_ref_capacity, 8u);
    EXPECT_EQ(sizeof(rt::HalV2Capabilities), sizeof(rtfw_device_capabilities));
    EXPECT_EQ(sizeof(rt::HalV2InitializeConfig), sizeof(rtfw_device_init_config));
    EXPECT_EQ(
        sizeof(rt::HalV2BufferRegistration),
        sizeof(rtfw_device_buffer_registration));
    EXPECT_EQ(sizeof(rt::HalV2BufferReference), sizeof(rtfw_device_buffer_ref));
    EXPECT_EQ(sizeof(rt::HalV2Submission), sizeof(rtfw_device_submission));
    EXPECT_EQ(sizeof(rt::HalV2Completion), sizeof(rtfw_device_completion));
    EXPECT_EQ(sizeof(rt::HalV2Health), sizeof(rtfw_device_health));
    EXPECT_EQ(sizeof(rt::HalV2BackendApi), sizeof(rtfw_device_backend_api));

    const rt::HalV2Capabilities capabilities;
    const rt::HalV2InitializeConfig initialize;
    const rt::HalV2BufferRegistration buffer;
    const rt::HalV2Submission submission;
    const rt::HalV2Completion completion;
    const rt::HalV2Health health;
    const rt::HalV2BackendApi api;
    EXPECT_EQ(capabilities.struct_size, sizeof(capabilities));
    EXPECT_EQ(capabilities.api_version, 2u);
    EXPECT_EQ(initialize.struct_size, sizeof(initialize));
    EXPECT_EQ(initialize.api_version, 2u);
    EXPECT_EQ(buffer.struct_size, sizeof(buffer));
    EXPECT_EQ(submission.struct_size, sizeof(submission));
    EXPECT_EQ(submission.api_version, 2u);
    EXPECT_EQ(completion.struct_size, sizeof(completion));
    EXPECT_EQ(health.struct_size, sizeof(health));
    EXPECT_EQ(api.struct_size, sizeof(api));
    EXPECT_EQ(api.api_version, 2u);
    EXPECT_EQ(api.instance, nullptr);
    EXPECT_TRUE(all_zero(capabilities.reserved));
    EXPECT_TRUE(all_zero(initialize.reserved));
    EXPECT_TRUE(all_zero(buffer.reserved));
    EXPECT_TRUE(all_zero(submission.reserved));
    EXPECT_TRUE(all_zero(completion.reserved));
    EXPECT_TRUE(all_zero(health.reserved));
    EXPECT_TRUE(all_zero(api.reserved));
}

TEST(HalV2, NativeRegistrationAndOperationTranslationAreExact) {
    NativeProbe probe;
    auto copied_api = probe.api();
    rt::Runtime runtime;
    ASSERT_EQ(runtime.configure(hal_config()), rt::Status::ok);
    rt::DeviceBackendHandle backend;
    ASSERT_EQ(
        runtime.register_device_backend(
            {"native", copied_api}, backend),
        rt::Status::ok);
    copied_api.poll = nullptr;

    std::array<std::byte, 16> storage{};
    rt::DeviceBufferHandle buffer;
    ASSERT_EQ(
        runtime.register_device_buffer(
            {"native.buffer", backend, storage}, buffer),
        rt::Status::ok);
    SubmissionRequest request{buffer};
    rt::PhaseHandle phase;
    ASSERT_EQ(
        runtime.register_device_phase(
            {"native.phase", backend, &prepare_submission, &request},
            phase),
        rt::Status::ok);
    ASSERT_EQ(runtime.finalize(), rt::Status::ok);
    rt::DeviceBackendHandle frozen;
    EXPECT_EQ(register_native(runtime, probe, "late", frozen), rt::Status::invalid_state);
    ASSERT_EQ(runtime.start(), rt::Status::ok);
    ASSERT_EQ(runtime.step({41, 1ms, std::nullopt}), rt::Status::ok);

    auto health = rt::make_device_health();
    ASSERT_EQ(runtime.device_health(backend, health), rt::Status::ok);
    EXPECT_EQ(health.state, RTFW_DEVICE_HEALTH_HEALTHY);
    EXPECT_EQ(health.generation, 9u);
    EXPECT_EQ(health.submissions, 1u);
    EXPECT_EQ(health.completions, 1u);
    EXPECT_EQ(runtime.reset_device(backend), rt::Status::ok);
    ASSERT_EQ(runtime.stop(), rt::Status::ok);

    EXPECT_EQ(probe.capability_calls.load(), 1u);
    EXPECT_EQ(probe.initialize_calls.load(), 1u);
    EXPECT_EQ(probe.register_calls.load(), 1u);
    EXPECT_EQ(probe.submit_calls.load(), 1u);
    EXPECT_GE(probe.poll_calls.load(), 1u);
    EXPECT_EQ(probe.health_calls.load(), 1u);
    EXPECT_EQ(probe.reset_calls.load(), 1u);
    EXPECT_EQ(probe.unregister_calls.load(), 1u);
    EXPECT_EQ(probe.shutdown_calls.load(), 1u);
    EXPECT_EQ(probe.observed_initialize.struct_size,
              sizeof(rt::HalV2InitializeConfig));
    EXPECT_EQ(probe.observed_initialize.api_version, 2u);
    EXPECT_EQ(probe.observed_initialize.requested_in_flight, 4u);
    EXPECT_EQ(probe.observed_initialize.requested_registered_buffers, 1u);
    EXPECT_TRUE(all_zero(probe.observed_initialize.reserved));
    EXPECT_EQ(probe.observed_buffer.data, storage.data());
    EXPECT_EQ(probe.observed_buffer.bytes, storage.size());
    EXPECT_EQ(
        probe.observed_buffer.flags,
        RTFW_DEVICE_BUFFER_HOST_READ |
            RTFW_DEVICE_BUFFER_HOST_WRITE |
            RTFW_DEVICE_BUFFER_DEVICE_READ |
            RTFW_DEVICE_BUFFER_DEVICE_WRITE);
    EXPECT_EQ(
        std::string_view(probe.observed_buffer.name.data()),
        "native.buffer");
    EXPECT_TRUE(all_zero(probe.observed_buffer.reserved));
    EXPECT_NE(probe.observed_submission.submission_id, 0u);
    EXPECT_EQ(probe.observed_submission.frame_index, 41u);
    EXPECT_EQ(probe.observed_submission.timeout_ns, 99'000u);
    EXPECT_EQ(probe.observed_submission.opcode, 0x1234u);
    EXPECT_EQ(probe.observed_submission.payload_size, 3u);
    EXPECT_EQ(probe.observed_submission.payload[0], 0x11u);
    EXPECT_EQ(probe.observed_submission.payload[1], 0x22u);
    EXPECT_EQ(probe.observed_submission.payload[2], 0x33u);
    EXPECT_EQ(probe.observed_submission.buffer_count, 1u);
    EXPECT_EQ(
        probe.observed_submission.buffers[0].buffer_token,
        NativeProbe::native_buffer_token);
    EXPECT_EQ(
        probe.observed_submission.buffers[0].access,
        RTFW_DEVICE_ACCESS_READ_WRITE);
    EXPECT_EQ(probe.observed_submission.buffers[0].offset, 2u);
    EXPECT_EQ(probe.observed_submission.buffers[0].bytes, 5u);
    EXPECT_TRUE(all_zero(probe.observed_submission.reserved));
    EXPECT_EQ(
        probe.observed_unregister_token,
        NativeProbe::native_buffer_token);
}

TEST(HalV2, MalformedTablesAndCapabilitiesFailTransactionally) {
    NativeProbe probe;
    const auto good = probe.api();
    const auto expect_invalid_api = [&](rt::HalV2BackendApi api) {
        rt::Runtime runtime;
        EXPECT_EQ(runtime.configure(hal_config()), rt::Status::ok);
        rt::DeviceBackendHandle handle;
        EXPECT_EQ(
            runtime.register_device_backend({"bad", api}, handle),
            rt::Status::invalid_argument);
        EXPECT_FALSE(handle.valid());
    };

    auto malformed = good;
    malformed.struct_size = sizeof(malformed) - 1;
    expect_invalid_api(malformed);
    malformed = good;
    malformed.api_version = 1;
    expect_invalid_api(malformed);
    malformed = good;
    malformed.instance = nullptr;
    expect_invalid_api(malformed);
    malformed = good;
    malformed.get_capabilities = nullptr;
    expect_invalid_api(malformed);
    malformed = good;
    malformed.initialize = nullptr;
    expect_invalid_api(malformed);
    malformed = good;
    malformed.register_buffer = nullptr;
    expect_invalid_api(malformed);
    malformed = good;
    malformed.unregister_buffer = nullptr;
    expect_invalid_api(malformed);
    malformed = good;
    malformed.submit = nullptr;
    expect_invalid_api(malformed);
    malformed = good;
    malformed.poll = nullptr;
    expect_invalid_api(malformed);
    malformed = good;
    malformed.cancel = nullptr;
    expect_invalid_api(malformed);
    malformed = good;
    malformed.get_health = nullptr;
    expect_invalid_api(malformed);
    malformed = good;
    malformed.reset = nullptr;
    expect_invalid_api(malformed);
    malformed = good;
    malformed.shutdown = nullptr;
    expect_invalid_api(malformed);
    malformed = good;
    malformed.reserved[3] = 1;
    expect_invalid_api(malformed);

    const auto expect_invalid_capabilities = [&](NativeProbe& candidate) {
        rt::Runtime runtime;
        EXPECT_EQ(runtime.configure(hal_config()), rt::Status::ok);
        rt::DeviceBackendHandle handle;
        EXPECT_EQ(
            register_native(runtime, candidate, "bad.caps", handle),
            rt::Status::invalid_argument);
        EXPECT_FALSE(handle.valid());
    };
    NativeProbe short_caps;
    short_caps.capabilities.struct_size = sizeof(rt::HalV2Capabilities) - 1;
    expect_invalid_capabilities(short_caps);
    NativeProbe wrong_version;
    wrong_version.capabilities.api_version = 1;
    expect_invalid_capabilities(wrong_version);
    NativeProbe zero_capacity;
    zero_capacity.capabilities.max_in_flight = 0;
    expect_invalid_capabilities(zero_capacity);
    NativeProbe invalid_boolean;
    invalid_boolean.capabilities.supports_reset = 2;
    expect_invalid_capabilities(invalid_boolean);
    NativeProbe invalid_identifier;
    set_identifier(invalid_identifier.capabilities.backend_id, "bad id");
    expect_invalid_capabilities(invalid_identifier);
    NativeProbe reserved;
    reserved.capabilities.reserved[0] = 1;
    expect_invalid_capabilities(reserved);

    NativeProbe first("test.hal.first");
    NativeProbe second("test.hal.second");
    rt::Runtime bounded;
    ASSERT_EQ(bounded.configure(hal_config()), rt::Status::ok);
    rt::DeviceBackendHandle first_handle;
    rt::DeviceBackendHandle ignored;
    ASSERT_EQ(register_native(bounded, first, "only", first_handle), rt::Status::ok);
    EXPECT_EQ(register_native(bounded, first, "only", ignored), rt::Status::capacity_exceeded);
    EXPECT_EQ(register_native(bounded, second, "second", ignored), rt::Status::capacity_exceeded);
    EXPECT_FALSE(ignored.valid());

    rt::Runtime duplicate;
    ASSERT_EQ(duplicate.configure(hal_config(2)), rt::Status::ok);
    ASSERT_EQ(
        register_native(duplicate, first, "duplicate", first_handle),
        rt::Status::ok);
    EXPECT_EQ(
        register_native(duplicate, second, "duplicate", ignored),
        rt::Status::invalid_argument);
    EXPECT_FALSE(ignored.valid());
}

TEST(HalV2, NativeAndV1StatusesAndExceptionsAreEquivalent) {
    struct Case {
        std::int32_t value;
        rt::Status expected;
    };
    constexpr std::array cases{
        Case{0, rt::Status::ok},
        Case{-1, rt::Status::invalid_argument},
        Case{-2, rt::Status::invalid_state},
        Case{-3, rt::Status::device_queue_full},
        Case{-4, rt::Status::device_timeout},
        Case{-5, rt::Status::device_error},
        Case{-6, rt::Status::device_lost},
        Case{-7, rt::Status::device_canceled},
        Case{-8, rt::Status::device_error},
        Case{-9, rt::Status::resource_exhausted},
        Case{-10, rt::Status::device_error},
        Case{-11, rt::Status::device_reset_required},
        Case{17, rt::Status::device_error},
    };
    for (const auto& test_case : cases) {
        SCOPED_TRACE(test_case.value);
        NativeProbe native;
        native.capability_status =
            static_cast<rt::HalV2Status>(test_case.value);
        rt::Runtime native_runtime;
        ASSERT_EQ(native_runtime.configure(hal_config()), rt::Status::ok);
        rt::DeviceBackendHandle native_handle;
        EXPECT_EQ(
            register_native(native_runtime, native, "status", native_handle),
            test_case.expected);
        EXPECT_EQ(native.capability_calls.load(), 1u);
        EXPECT_EQ(native_handle.valid(), test_case.expected == rt::Status::ok);

        V1Probe v1;
        v1.capability_status = test_case.value;
        rt::Runtime v1_runtime;
        ASSERT_EQ(v1_runtime.configure(hal_config()), rt::Status::ok);
        rt::DeviceBackendHandle v1_handle;
        EXPECT_EQ(
            v1_runtime.register_device_backend(
                {"status", v1.api()}, v1_handle),
            test_case.expected);
        EXPECT_EQ(v1.capability_calls.load(), 1u);
        EXPECT_EQ(v1_handle.valid(), test_case.expected == rt::Status::ok);
    }

    NativeProbe native_throw;
    native_throw.throw_capabilities = true;
    rt::Runtime native_runtime;
    ASSERT_EQ(native_runtime.configure(hal_config()), rt::Status::ok);
    rt::DeviceBackendHandle handle;
    EXPECT_EQ(
        register_native(native_runtime, native_throw, "throw", handle),
        rt::Status::device_error);
    EXPECT_FALSE(handle.valid());

    V1Probe v1_throw;
    v1_throw.throw_capabilities = true;
    rt::Runtime v1_runtime;
    ASSERT_EQ(v1_runtime.configure(hal_config()), rt::Status::ok);
    EXPECT_EQ(
        v1_runtime.register_device_backend(
            {"throw", v1_throw.api()}, handle),
        rt::Status::device_error);
    EXPECT_FALSE(handle.valid());

    NativeProbe submit_throw;
    submit_throw.throw_submit = true;
    rt::Runtime step_runtime;
    ASSERT_EQ(step_runtime.configure(hal_config()), rt::Status::ok);
    rt::DeviceBackendHandle backend;
    ASSERT_EQ(register_native(step_runtime, submit_throw, "throw.submit", backend), rt::Status::ok);
    rt::PhaseHandle phase;
    ASSERT_EQ(
        step_runtime.register_device_phase(
            {"throw.phase", backend, &prepare_no_buffer_submission, nullptr},
            phase),
        rt::Status::ok);
    ASSERT_EQ(step_runtime.finalize(), rt::Status::ok);
    ASSERT_EQ(step_runtime.start(), rt::Status::ok);
    EXPECT_EQ(
        step_runtime.step({1, 1ms, std::nullopt}),
        rt::Status::device_error);
    EXPECT_EQ(submit_throw.submit_calls.load(), 1u);
    EXPECT_EQ(step_runtime.stop(), rt::Status::ok);
}

TEST(HalV2, DeviceAbiV1AdapterPreservesEveryCoreFieldAndFailsClosed) {
    V1Probe probe("test.hal.translation");
    rt::detail::DeviceV1CompatibilityAdapter adapter(probe.api());
    ASSERT_EQ(
        adapter.prepare_completion_storage(2),
        rt::Status::ok);
    const auto& api = adapter.api();

    rt::HalV2Capabilities capabilities;
    ASSERT_EQ(
        api.get_capabilities(api.instance, &capabilities),
        rt::HalV2Status::ok);
    EXPECT_EQ(capabilities.struct_size, sizeof(capabilities));
    EXPECT_EQ(capabilities.api_version, 2u);
    EXPECT_EQ(capabilities.max_in_flight, 8u);
    EXPECT_EQ(capabilities.max_registered_buffers, 8u);
    EXPECT_EQ(capabilities.max_buffer_bytes, 4096u);
    EXPECT_EQ(capabilities.inline_payload_capacity, 128u);
    EXPECT_EQ(capabilities.buffer_ref_capacity, 8u);
    EXPECT_EQ(capabilities.supports_cancel, 1u);
    EXPECT_EQ(capabilities.supports_reset, 1u);
    EXPECT_EQ(capabilities.deterministic_mock, 1u);
    EXPECT_EQ(
        std::string_view(capabilities.backend_id.data()),
        "test.hal.translation");
    EXPECT_EQ(capabilities.control_storage_bytes, 37u);
    EXPECT_TRUE(all_zero(capabilities.reserved));
    EXPECT_EQ(probe.capability_calls.load(), 1u);

    rt::HalV2InitializeConfig initialize;
    initialize.requested_in_flight = 6;
    initialize.requested_registered_buffers = 2;
    ASSERT_EQ(
        api.initialize(api.instance, &initialize),
        rt::HalV2Status::ok);
    EXPECT_EQ(probe.initialize_calls.load(), 1u);
    EXPECT_EQ(
        probe.observed_initialize.struct_size,
        sizeof(rtfw_device_init_config));
    EXPECT_EQ(
        probe.observed_initialize.abi_version,
        RTFW_DEVICE_ABI_VERSION);
    EXPECT_EQ(probe.observed_initialize.requested_in_flight, 6u);
    EXPECT_EQ(
        probe.observed_initialize.requested_registered_buffers,
        2u);
    EXPECT_TRUE(std::all_of(
        std::begin(probe.observed_initialize.reserved),
        std::end(probe.observed_initialize.reserved),
        [](std::uint64_t value) { return value == 0; }));

    std::array<std::byte, 32> storage{};
    rt::HalV2BufferRegistration registration;
    registration.flags =
        RTFW_DEVICE_BUFFER_HOST_READ |
        RTFW_DEVICE_BUFFER_DEVICE_WRITE;
    registration.data = storage.data();
    registration.bytes = storage.size();
    set_identifier(registration.name, "translation.buffer");
    std::uint64_t token = 0;
    ASSERT_EQ(
        api.register_buffer(
            api.instance, &registration, &token),
        rt::HalV2Status::ok);
    EXPECT_EQ(probe.register_calls.load(), 1u);
    EXPECT_EQ(token, probe.buffer_token);
    EXPECT_EQ(
        probe.observed_buffer.struct_size,
        sizeof(rtfw_device_buffer_registration));
    EXPECT_EQ(probe.observed_buffer.flags, registration.flags);
    EXPECT_EQ(probe.observed_buffer.data, storage.data());
    EXPECT_EQ(probe.observed_buffer.bytes, storage.size());
    EXPECT_EQ(
        std::string_view(probe.observed_buffer.name),
        "translation.buffer");
    EXPECT_TRUE(std::all_of(
        std::begin(probe.observed_buffer.reserved),
        std::end(probe.observed_buffer.reserved),
        [](std::uint64_t value) { return value == 0; }));

    probe.register_status = RTFW_DEVICE_STATUS_ERROR;
    token = 0;
    EXPECT_EQ(
        api.register_buffer(api.instance, &registration, &token),
        rt::HalV2Status::error);
    EXPECT_EQ(token, probe.buffer_token);
    probe.register_status = RTFW_DEVICE_STATUS_OK;
    probe.throw_register = true;
    token = 0;
    EXPECT_EQ(
        api.register_buffer(api.instance, &registration, &token),
        rt::HalV2Status::internal_error);
    EXPECT_EQ(token, probe.buffer_token);
    probe.throw_register = false;

    rt::HalV2Submission submission;
    submission.submission_id = 77;
    submission.frame_index = 123;
    submission.timeout_ns = 456;
    submission.opcode = 0x789u;
    submission.payload_size = 3;
    submission.payload[0] = 0x12u;
    submission.payload[1] = 0x34u;
    submission.payload[2] = 0x56u;
    submission.payload.back() = 0xa5u;
    submission.buffer_count = 2;
    submission.buffers[0].buffer_token = 0x100u;
    submission.buffers[0].access = RTFW_DEVICE_ACCESS_READ;
    submission.buffers[0].offset = 4;
    submission.buffers[0].bytes = 5;
    submission.buffers[1].buffer_token = 0x200u;
    submission.buffers[1].access = RTFW_DEVICE_ACCESS_WRITE;
    submission.buffers[1].offset = 6;
    submission.buffers[1].bytes = 7;
    ASSERT_EQ(
        api.submit(api.instance, &submission),
        rt::HalV2Status::ok);
    EXPECT_EQ(probe.submit_calls.load(), 1u);
    EXPECT_EQ(
        probe.observed_submission.struct_size,
        sizeof(rtfw_device_submission));
    EXPECT_EQ(
        probe.observed_submission.abi_version,
        RTFW_DEVICE_ABI_VERSION);
    EXPECT_EQ(probe.observed_submission.submission_id, 77u);
    EXPECT_EQ(probe.observed_submission.frame_index, 123u);
    EXPECT_EQ(probe.observed_submission.timeout_ns, 456u);
    EXPECT_EQ(probe.observed_submission.opcode, 0x789u);
    EXPECT_EQ(probe.observed_submission.flags, 0u);
    EXPECT_EQ(probe.observed_submission.payload_size, 3u);
    EXPECT_EQ(probe.observed_submission.payload[0], 0x12u);
    EXPECT_EQ(probe.observed_submission.payload[1], 0x34u);
    EXPECT_EQ(probe.observed_submission.payload[2], 0x56u);
    EXPECT_EQ(
        probe.observed_submission.payload[
            RTFW_DEVICE_INLINE_PAYLOAD_CAPACITY - 1],
        0xa5u);
    EXPECT_EQ(probe.observed_submission.buffer_count, 2u);
    EXPECT_EQ(probe.observed_submission.buffers[0].buffer_token, 0x100u);
    EXPECT_EQ(
        probe.observed_submission.buffers[0].access,
        RTFW_DEVICE_ACCESS_READ);
    EXPECT_EQ(probe.observed_submission.buffers[0].offset, 4u);
    EXPECT_EQ(probe.observed_submission.buffers[0].bytes, 5u);
    EXPECT_EQ(probe.observed_submission.buffers[0].reserved0, 0u);
    EXPECT_EQ(probe.observed_submission.buffers[1].buffer_token, 0x200u);
    EXPECT_EQ(
        probe.observed_submission.buffers[1].access,
        RTFW_DEVICE_ACCESS_WRITE);
    EXPECT_EQ(probe.observed_submission.buffers[1].offset, 6u);
    EXPECT_EQ(probe.observed_submission.buffers[1].bytes, 7u);
    EXPECT_EQ(probe.observed_submission.buffers[1].reserved0, 0u);
    EXPECT_TRUE(std::all_of(
        std::begin(probe.observed_submission.reserved),
        std::end(probe.observed_submission.reserved),
        [](std::uint64_t value) { return value == 0; }));

    std::array<rt::HalV2Completion, 2> completions{};
    std::uint64_t completion_count = 0;
    ASSERT_EQ(
        api.poll(
            api.instance,
            completions.data(),
            completions.size(),
            &completion_count),
        rt::HalV2Status::ok);
    EXPECT_EQ(probe.poll_calls.load(), 1u);
    ASSERT_EQ(completion_count, 1u);
    EXPECT_EQ(completions[0].status, RTFW_DEVICE_STATUS_TIMEOUT);
    EXPECT_EQ(completions[0].submission_id, 77u);
    EXPECT_EQ(completions[0].device_timestamp_ns, 88u);
    EXPECT_EQ(completions[0].value, 99u);
    EXPECT_TRUE(all_zero(completions[0].reserved));

    rt::HalV2Health health;
    ASSERT_EQ(
        api.get_health(api.instance, &health),
        rt::HalV2Status::ok);
    EXPECT_EQ(probe.health_calls.load(), 1u);
    EXPECT_EQ(
        health.state,
        static_cast<std::uint32_t>(rt::HalV2HealthState::degraded));
    EXPECT_EQ(
        health.last_status,
        static_cast<std::int32_t>(rt::HalV2Status::reset_required));
    EXPECT_EQ(health.generation, 1u);
    EXPECT_EQ(health.submissions, 2u);
    EXPECT_EQ(health.completions, 3u);
    EXPECT_EQ(health.queue_rejections, 4u);
    EXPECT_EQ(health.timeouts, 5u);
    EXPECT_EQ(health.errors, 6u);
    EXPECT_EQ(health.losses, 7u);
    EXPECT_EQ(health.cancellations, 8u);
    EXPECT_EQ(health.resets, 9u);
    EXPECT_EQ(health.outstanding, 10u);
    EXPECT_TRUE(all_zero(health.reserved));

    EXPECT_EQ(
        api.unregister_buffer(api.instance, token),
        rt::HalV2Status::ok);
    EXPECT_EQ(probe.unregister_calls.load(), 1u);
    EXPECT_EQ(probe.observed_unregister_token, token);
    EXPECT_EQ(
        api.cancel(api.instance, 0x7788u),
        rt::HalV2Status::unsupported);
    EXPECT_EQ(probe.cancel_calls.load(), 1u);
    EXPECT_EQ(probe.observed_cancel_id, 0x7788u);
    probe.reset_status = 42;
    EXPECT_EQ(
        api.reset(api.instance),
        rt::HalV2Status::internal_error);
    EXPECT_EQ(probe.reset_calls.load(), 1u);
    probe.shutdown_status = RTFW_DEVICE_STATUS_RESET_REQUIRED;
    EXPECT_EQ(
        api.shutdown(api.instance),
        rt::HalV2Status::reset_required);
    EXPECT_EQ(probe.shutdown_calls.load(), 1u);

    const auto poll_calls = probe.poll_calls.load();
    probe.completion.reserved[0] = 1;
    completions[0].value = 0xfeedu;
    completion_count = 9;
    EXPECT_EQ(
        api.poll(
            api.instance,
            completions.data(),
            completions.size(),
            &completion_count),
        rt::HalV2Status::internal_error);
    EXPECT_EQ(probe.poll_calls.load(), poll_calls + 1);
    EXPECT_EQ(completion_count, 0u);
    EXPECT_EQ(completions[0].value, 0xfeedu);
    probe.completion.reserved[0] = 0;
    probe.completion.status = 42;
    EXPECT_EQ(
        api.poll(
            api.instance,
            completions.data(),
            completions.size(),
            &completion_count),
        rt::HalV2Status::internal_error);
    EXPECT_EQ(probe.poll_calls.load(), poll_calls + 2);
    EXPECT_EQ(completion_count, 0u);
    probe.throw_poll = true;
    EXPECT_EQ(
        api.poll(
            api.instance,
            completions.data(),
            completions.size(),
            &completion_count),
        rt::HalV2Status::internal_error);
    EXPECT_EQ(probe.poll_calls.load(), poll_calls + 3);
    EXPECT_EQ(completion_count, 0u);

    const auto health_calls = probe.health_calls.load();
    probe.health.state = 42;
    health.generation = 0xfeedu;
    EXPECT_EQ(
        api.get_health(api.instance, &health),
        rt::HalV2Status::internal_error);
    EXPECT_EQ(probe.health_calls.load(), health_calls + 1);
    EXPECT_EQ(health.generation, 0xfeedu);
    probe.health.state = RTFW_DEVICE_HEALTH_HEALTHY;
    probe.health.last_status = 42;
    EXPECT_EQ(
        api.get_health(api.instance, &health),
        rt::HalV2Status::internal_error);
    EXPECT_EQ(probe.health_calls.load(), health_calls + 2);
    EXPECT_EQ(health.generation, 0xfeedu);
}

TEST(HalV2, NativeIdentityIsSeparateAndV1IdentityRemainsStable) {
    NativeProbe native("test.hal.identity");
    V1Probe first_v1("test.hal.identity");
    V1Probe second_v1("test.hal.identity");

    rt::Runtime first;
    rt::Runtime second;
    rt::Runtime native_runtime;
    ASSERT_EQ(first.configure(hal_config()), rt::Status::ok);
    ASSERT_EQ(second.configure(hal_config()), rt::Status::ok);
    ASSERT_EQ(native_runtime.configure(hal_config()), rt::Status::ok);
    rt::DeviceBackendHandle handle;
    ASSERT_EQ(
        first.register_device_backend(
            {"identity", first_v1.api()}, handle),
        rt::Status::ok);
    ASSERT_EQ(
        second.register_device_backend(
            {"identity", second_v1.api()}, handle),
        rt::Status::ok);
    ASSERT_EQ(
        register_native(native_runtime, native, "identity", handle),
        rt::Status::ok);
    std::array<std::byte, 1> first_state{};
    std::array<std::byte, 1> second_state{};
    std::array<std::byte, 1> native_state{};
    ASSERT_EQ(
        first.register_state({"identity.state", 1, first_state}),
        rt::Status::ok);
    ASSERT_EQ(
        second.register_state({"identity.state", 1, second_state}),
        rt::Status::ok);
    ASSERT_EQ(
        native_runtime.register_state(
            {"identity.state", 1, native_state}),
        rt::Status::ok);
    ASSERT_EQ(first.finalize(), rt::Status::ok);
    ASSERT_EQ(second.finalize(), rt::Status::ok);
    ASSERT_EQ(native_runtime.finalize(), rt::Status::ok);
    const auto first_metadata = checkpoint_metadata(first);
    const auto second_metadata = checkpoint_metadata(second);
    const auto native_metadata = checkpoint_metadata(native_runtime);
    EXPECT_EQ(first_metadata.graph_id, second_metadata.graph_id);
    EXPECT_NE(first_metadata.graph_id, native_metadata.graph_id);
    EXPECT_EQ(first_metadata.replay_id, second_metadata.replay_id);
    EXPECT_NE(first_metadata.replay_id, native_metadata.replay_id);
}

TEST(HalV2, NativeAndAdaptedV1RuntimeObservationsAreEquivalent) {
    NativeProbe native("test.hal.equivalent");
    V1Probe v1("test.hal.equivalent");
    v1.runtime_poll = true;
    v1.completion.status = RTFW_DEVICE_STATUS_OK;
    v1.completion.device_timestamp_ns = 1234;
    v1.completion.value = 5678;
    v1.health.state = RTFW_DEVICE_HEALTH_HEALTHY;
    v1.health.last_status = RTFW_DEVICE_STATUS_OK;
    v1.health.generation = 9;
    v1.health.submissions = 1;
    v1.health.completions = 1;
    v1.health.queue_rejections = 0;
    v1.health.timeouts = 0;
    v1.health.errors = 0;
    v1.health.losses = 0;
    v1.health.cancellations = 0;
    v1.health.resets = 0;
    v1.health.outstanding = 0;

    rt::Runtime native_runtime;
    rt::Runtime adapted_runtime;
    const auto config = hal_config();
    ASSERT_EQ(native_runtime.configure(config), rt::Status::ok);
    ASSERT_EQ(adapted_runtime.configure(config), rt::Status::ok);

    rt::DeviceBackendHandle native_backend;
    rt::DeviceBackendHandle adapted_backend;
    ASSERT_EQ(
        register_native(
            native_runtime, native, "equivalent", native_backend),
        rt::Status::ok);
    ASSERT_EQ(
        adapted_runtime.register_device_backend(
            {"equivalent", v1.api()}, adapted_backend),
        rt::Status::ok);

    std::array<std::byte, 16> native_storage{};
    std::array<std::byte, 16> adapted_storage{};
    rt::DeviceBufferHandle native_buffer;
    rt::DeviceBufferHandle adapted_buffer;
    ASSERT_EQ(
        native_runtime.register_device_buffer(
            {"equivalent.buffer", native_backend, native_storage},
            native_buffer),
        rt::Status::ok);
    ASSERT_EQ(
        adapted_runtime.register_device_buffer(
            {"equivalent.buffer", adapted_backend, adapted_storage},
            adapted_buffer),
        rt::Status::ok);

    SubmissionRequest native_request{native_buffer};
    SubmissionRequest adapted_request{adapted_buffer};
    rt::PhaseHandle native_phase;
    rt::PhaseHandle adapted_phase;
    ASSERT_EQ(
        native_runtime.register_device_phase(
            {"equivalent.phase",
             native_backend,
             &prepare_submission,
             &native_request},
            native_phase),
        rt::Status::ok);
    ASSERT_EQ(
        adapted_runtime.register_device_phase(
            {"equivalent.phase",
             adapted_backend,
             &prepare_submission,
             &adapted_request},
            adapted_phase),
        rt::Status::ok);

    ASSERT_EQ(native_runtime.finalize(), rt::Status::ok);
    ASSERT_EQ(adapted_runtime.finalize(), rt::Status::ok);
    ASSERT_EQ(native_runtime.start(), rt::Status::ok);
    ASSERT_EQ(adapted_runtime.start(), rt::Status::ok);

    EXPECT_EQ(
        native_runtime.step({17, 1ms, std::nullopt}),
        rt::Status::ok);
    EXPECT_EQ(
        adapted_runtime.step({17, 1ms, std::nullopt}),
        rt::Status::ok);
    EXPECT_EQ(native_storage, adapted_storage);

    auto native_health = rt::make_device_health();
    auto adapted_health = rt::make_device_health();
    ASSERT_EQ(
        native_runtime.device_health(native_backend, native_health),
        rt::Status::ok);
    ASSERT_EQ(
        adapted_runtime.device_health(adapted_backend, adapted_health),
        rt::Status::ok);
    EXPECT_EQ(native_health.state, adapted_health.state);
    EXPECT_EQ(native_health.last_status, adapted_health.last_status);
    EXPECT_EQ(native_health.generation, adapted_health.generation);
    EXPECT_EQ(native_health.submissions, adapted_health.submissions);
    EXPECT_EQ(native_health.completions, adapted_health.completions);
    EXPECT_EQ(native_health.queue_rejections, adapted_health.queue_rejections);
    EXPECT_EQ(native_health.timeouts, adapted_health.timeouts);
    EXPECT_EQ(native_health.errors, adapted_health.errors);
    EXPECT_EQ(native_health.losses, adapted_health.losses);
    EXPECT_EQ(native_health.cancellations, adapted_health.cancellations);
    EXPECT_EQ(native_health.resets, adapted_health.resets);
    EXPECT_EQ(native_health.outstanding, adapted_health.outstanding);

    ASSERT_EQ(native_runtime.reset_device(native_backend), rt::Status::ok);
    ASSERT_EQ(adapted_runtime.reset_device(adapted_backend), rt::Status::ok);

    rt::RuntimeMetricSnapshot native_metrics;
    rt::RuntimeMetricSnapshot adapted_metrics;
    ASSERT_EQ(
        native_runtime.metrics_snapshot(
            rt::RuntimeMetricWindow::cumulative,
            nullptr,
            native_metrics),
        rt::Status::ok);
    ASSERT_EQ(
        adapted_runtime.metrics_snapshot(
            rt::RuntimeMetricWindow::cumulative,
            nullptr,
            adapted_metrics),
        rt::Status::ok);
    ASSERT_EQ(native_metrics.sample_count, adapted_metrics.sample_count);
    for (std::size_t index = 0; index < native_metrics.sample_count; ++index) {
        SCOPED_TRACE(index);
        EXPECT_EQ(native_metrics.samples[index].id, adapted_metrics.samples[index].id);
        EXPECT_EQ(
            native_metrics.samples[index].kind,
            adapted_metrics.samples[index].kind);
        EXPECT_EQ(
            native_metrics.samples[index].value,
            adapted_metrics.samples[index].value);
    }

    std::array<rt::RuntimeTraceEvent, 64> native_trace{};
    std::array<rt::RuntimeTraceEvent, 64> adapted_trace{};
    rt::RuntimeTraceCursor native_cursor;
    rt::RuntimeTraceCursor adapted_cursor;
    rt::RuntimeTraceReadResult native_trace_result;
    rt::RuntimeTraceReadResult adapted_trace_result;
    ASSERT_EQ(
        native_runtime.read_trace(
            native_cursor, native_trace, native_trace_result),
        rt::Status::ok);
    ASSERT_EQ(
        adapted_runtime.read_trace(
            adapted_cursor, adapted_trace, adapted_trace_result),
        rt::Status::ok);
    std::array<rt::RuntimeTraceEvent, 3> native_device_events{};
    std::array<rt::RuntimeTraceEvent, 3> adapted_device_events{};
    std::size_t native_device_count = 0;
    std::size_t adapted_device_count = 0;
    for (std::size_t index = 0;
         index < native_trace_result.events_read;
         ++index) {
        if (native_trace[index].type >=
                rt::RuntimeTraceEventType::device_submitted &&
            native_device_count < native_device_events.size()) {
            native_device_events[native_device_count++] = native_trace[index];
        }
    }
    for (std::size_t index = 0;
         index < adapted_trace_result.events_read;
         ++index) {
        if (adapted_trace[index].type >=
                rt::RuntimeTraceEventType::device_submitted &&
            adapted_device_count < adapted_device_events.size()) {
            adapted_device_events[adapted_device_count++] = adapted_trace[index];
        }
    }
    ASSERT_EQ(native_device_count, native_device_events.size());
    ASSERT_EQ(adapted_device_count, adapted_device_events.size());
    for (std::size_t index = 0; index < native_device_events.size(); ++index) {
        SCOPED_TRACE(index);
        EXPECT_EQ(native_device_events[index].type, adapted_device_events[index].type);
        EXPECT_EQ(
            native_device_events[index].status,
            adapted_device_events[index].status);
        EXPECT_EQ(
            native_device_events[index].producer,
            adapted_device_events[index].producer);
        EXPECT_EQ(
            native_device_events[index].frame_index,
            adapted_device_events[index].frame_index);
        EXPECT_EQ(
            native_device_events[index].callback_index,
            adapted_device_events[index].callback_index);
        EXPECT_EQ(
            native_device_events[index].value,
            adapted_device_events[index].value);
    }

    ASSERT_EQ(native_runtime.stop(), rt::Status::ok);
    ASSERT_EQ(adapted_runtime.stop(), rt::Status::ok);
    EXPECT_EQ(native.initialize_calls.load(), v1.initialize_calls.load());
    EXPECT_EQ(native.register_calls.load(), v1.register_calls.load());
    EXPECT_EQ(native.submit_calls.load(), v1.submit_calls.load());
    EXPECT_EQ(native.health_calls.load(), v1.health_calls.load());
    EXPECT_EQ(native.reset_calls.load(), v1.reset_calls.load());
    EXPECT_EQ(native.unregister_calls.load(), v1.unregister_calls.load());
    EXPECT_EQ(native.shutdown_calls.load(), v1.shutdown_calls.load());
}

TEST(HalV2, AdaptedV1StorageIsExactInsideSixRowMemoryPlan) {
    NativeProbe native("test.hal.accounting");
    V1Probe v1("test.hal.accounting");
    const auto config = hal_config();
    rt::Runtime native_runtime;
    rt::Runtime adapted_runtime;
    ASSERT_EQ(native_runtime.configure(config), rt::Status::ok);
    ASSERT_EQ(adapted_runtime.configure(config), rt::Status::ok);
    rt::DeviceBackendHandle handle;
    ASSERT_EQ(
        register_native(native_runtime, native, "accounting", handle),
        rt::Status::ok);
    ASSERT_EQ(
        adapted_runtime.register_device_backend(
            {"accounting", v1.api()}, handle),
        rt::Status::ok);
    ASSERT_EQ(native_runtime.finalize(), rt::Status::ok);
    ASSERT_EQ(adapted_runtime.finalize(), rt::Status::ok);
    rt::MemoryPlan native_plan;
    rt::MemoryPlan adapted_plan;
    ASSERT_TRUE(native_runtime.memory_plan(native_plan));
    ASSERT_TRUE(adapted_runtime.memory_plan(adapted_plan));
    const auto adapter_bytes =
        sizeof(rt::detail::DeviceV1CompatibilityAdapter) +
        config.device_completion_batch * sizeof(rtfw_device_completion);
    EXPECT_EQ(
        adapted_plan.device_control_bytes,
        native_plan.device_control_bytes + adapter_bytes);
    EXPECT_EQ(native_plan.device_backend_reported_bytes, 37u);
    EXPECT_EQ(adapted_plan.device_backend_reported_bytes, 37u);
    EXPECT_EQ(
        native_plan.planned_bytes,
        native_plan.runtime_control_bytes +
            native_plan.executor_control_bytes +
            native_plan.device_control_bytes +
            native_plan.phase_scratch_total_bytes +
            native_plan.task_scratch_total_bytes +
            native_plan.trace_storage_bytes);
    EXPECT_EQ(
        adapted_plan.planned_bytes,
        adapted_plan.runtime_control_bytes +
            adapted_plan.executor_control_bytes +
            adapted_plan.device_control_bytes +
            adapted_plan.phase_scratch_total_bytes +
            adapted_plan.task_scratch_total_bytes +
            adapted_plan.trace_storage_bytes);
}

TEST(HalV2, AdaptedV1StorageSurvivesConfiguringGrowthAndIsIsolated) {
    constexpr std::size_t backend_count = 8;
    std::array<V1Probe, backend_count> probes{
        V1Probe{"test.growth.0"},
        V1Probe{"test.growth.1"},
        V1Probe{"test.growth.2"},
        V1Probe{"test.growth.3"},
        V1Probe{"test.growth.4"},
        V1Probe{"test.growth.5"},
        V1Probe{"test.growth.6"},
        V1Probe{"test.growth.7"},
    };
    rt::Runtime runtime;
    ASSERT_EQ(runtime.configure(hal_config(backend_count)), rt::Status::ok);
    for (std::size_t index = 0; index < probes.size(); ++index) {
        const auto name = std::string("growth.") + std::to_string(index);
        rt::DeviceBackendHandle handle;
        ASSERT_EQ(
            runtime.register_device_backend(
                {name, probes[index].api()}, handle),
            rt::Status::ok);
        EXPECT_EQ(handle.index(), index);
    }
    ASSERT_EQ(runtime.finalize(), rt::Status::ok);
    ASSERT_EQ(runtime.start(), rt::Status::ok);
    ASSERT_EQ(runtime.stop(), rt::Status::ok);
    for (const auto& probe : probes) {
        EXPECT_EQ(probe.capability_calls.load(), 1u);
        EXPECT_EQ(probe.initialize_calls.load(), 1u);
        EXPECT_EQ(probe.shutdown_calls.load(), 1u);
    }
}
