#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <thread>
#include <vector>

#include <rt/cuda_backend.hpp>
#include <rt/runtime.hpp>

namespace {

class FakeCudaDriver {
public:
    static constexpr rt::CudaContext context = 0xc001u;
    static constexpr rt::CudaFunction add_one_function = 0xf001u;
    static constexpr rt::CudaGraphExec graph = 0x9001u;
    static constexpr std::size_t event_capacity = 32;
    static constexpr std::size_t allocation_capacity = 8;
    static constexpr std::size_t allocation_bytes = 4096;

    rt::CudaDriverApi api() noexcept {
        rt::CudaDriverApi result{};
        result.user_data = this;
        result.push_context = &push_context;
        result.pop_context = &pop_context;
        result.event_create = &event_create;
        result.event_destroy = &event_destroy;
        result.event_record = &event_record;
        result.event_query = &event_query;
        result.event_synchronize = &event_synchronize;
        result.stream_synchronize = &stream_synchronize;
        result.mem_alloc = &mem_alloc;
        result.mem_free = &mem_free;
        result.host_register = &host_register;
        result.host_unregister = &host_unregister;
        result.memcpy_host_to_device_async =
            &memcpy_host_to_device_async;
        result.memcpy_device_to_host_async =
            &memcpy_device_to_host_async;
        result.memcpy_device_to_device_async =
            &memcpy_device_to_device_async;
        result.memset_d8_async = &memset_d8_async;
        result.launch_kernel = &launch_kernel;
        result.monotonic_time_ns = &monotonic_time_ns;
        return result;
    }

    rt::CudaDriverApi api_v2() noexcept {
        auto result = api();
        result.struct_size = rt::cuda_driver_api_v2_struct_size;
        result.api_version = rt::cuda_driver_api_version_2;
        result.graph_launch = &graph_launch;
        return result;
    }

    void make_events_ready() noexcept {
        for (auto& event : events_) {
            if (event.recorded.load(std::memory_order_acquire)) {
                event.ready.store(true, std::memory_order_release);
            }
        }
    }

    void advance(std::uint64_t nanoseconds) noexcept {
        now_ns_.fetch_add(nanoseconds, std::memory_order_relaxed);
    }

    std::uint64_t event_destroy_attempts(
        rt::CudaEvent event) const noexcept {
        if (event == 0 || event > event_destroy_attempts_.size()) {
            return 0;
        }
        return event_destroy_attempts_[
            static_cast<std::size_t>(event - 1)]
            .load(std::memory_order_acquire);
    }

    std::atomic<bool> complete_on_record{false};
    std::atomic<bool> fail_next_pop_context{false};
    std::atomic<bool> fail_next_event_destroy{false};
    std::atomic<bool> fail_next_event_record{false};
    std::atomic<bool> fail_next_event_query{false};
    std::atomic<bool> fail_next_event_sync{false};
    std::atomic<bool> fail_next_stream_sync{false};
    std::atomic<bool> fail_next_mem_free{false};
    std::atomic<bool> fail_next_host_register{false};
    std::atomic<bool> lose_context_on_query{false};
    std::atomic<std::uint64_t> event_create_failure_call{0};
    std::atomic<std::uint64_t> event_create_calls{0};
    std::atomic<std::uint64_t> event_syncs{0};
    std::atomic<std::uint64_t> stream_syncs{0};
    std::atomic<std::uint64_t> allocations{0};
    std::atomic<std::uint64_t> frees{0};
    std::atomic<std::uint64_t> host_registrations{0};
    std::atomic<std::uint64_t> host_unregistrations{0};
    std::atomic<std::uint64_t> launches{0};
    std::atomic<std::uint64_t> graph_launches{0};
    std::atomic<bool> fail_next_graph_launch{false};
    std::array<char, 32> call_order{};
    std::atomic<std::size_t> call_order_count{0};

private:
    struct EventState {
        std::atomic<bool> allocated{false};
        std::atomic<bool> recorded{false};
        std::atomic<bool> ready{false};
    };

#if defined(_MSC_VER)
#pragma warning(push)
// The fake device storage intentionally follows max_align_t. MSVC reports
// the resulting, deliberate test-only padding as C4324 under /W4.
#pragma warning(disable : 4324)
#endif
    struct Allocation {
        bool allocated = false;
        alignas(std::max_align_t)
            std::array<std::byte, allocation_bytes> bytes{};
    };
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

    static FakeCudaDriver* self(void* user_data) noexcept {
        return static_cast<FakeCudaDriver*>(user_data);
    }

    static void record(FakeCudaDriver& driver, char value) noexcept {
        const auto index = driver.call_order_count.fetch_add(
            1, std::memory_order_relaxed);
        if (index < driver.call_order.size()) {
            driver.call_order[index] = value;
        }
    }

    static EventState* event_for(
        FakeCudaDriver& driver,
        rt::CudaEvent event) noexcept {
        if (event == 0 || event > driver.events_.size()) {
            return nullptr;
        }
        auto& state = driver.events_[
            static_cast<std::size_t>(event - 1)];
        return state.allocated.load(std::memory_order_acquire)
            ? &state
            : nullptr;
    }

    static rt::CudaDriverResult push_context(
        void* user_data,
        rt::CudaContext requested) noexcept {
        return user_data && requested == context
            ? rt::CudaDriverResult::success
            : rt::CudaDriverResult::context_lost;
    }

    static rt::CudaDriverResult pop_context(
        void* user_data,
        rt::CudaContext* out_context) noexcept {
        auto* driver = self(user_data);
        if (!driver || !out_context) {
            return rt::CudaDriverResult::invalid_value;
        }
        if (driver->fail_next_pop_context.exchange(
                false,
                std::memory_order_acq_rel)) {
            return rt::CudaDriverResult::error;
        }
        *out_context = context;
        return rt::CudaDriverResult::success;
    }

    static rt::CudaDriverResult event_create(
        void* user_data,
        rt::CudaEvent* out_event) noexcept {
        auto* driver = self(user_data);
        if (!driver || !out_event) {
            return rt::CudaDriverResult::invalid_value;
        }
        *out_event = 0;
        const auto call =
            driver->event_create_calls.fetch_add(
                1,
                std::memory_order_relaxed) + 1;
        if (call ==
            driver->event_create_failure_call.load(
                std::memory_order_acquire)) {
            return rt::CudaDriverResult::out_of_memory;
        }
        for (std::size_t index = 0;
             index < driver->events_.size();
             ++index) {
            auto expected = false;
            if (driver->events_[index].allocated.compare_exchange_strong(
                    expected,
                    true,
                    std::memory_order_acq_rel,
                    std::memory_order_relaxed)) {
                driver->events_[index].recorded.store(
                    false,
                    std::memory_order_relaxed);
                driver->events_[index].ready.store(
                    false,
                    std::memory_order_relaxed);
                *out_event = static_cast<rt::CudaEvent>(index + 1);
                return rt::CudaDriverResult::success;
            }
        }
        return rt::CudaDriverResult::out_of_memory;
    }

    static rt::CudaDriverResult event_destroy(
        void* user_data,
        rt::CudaEvent event) noexcept {
        auto* driver = self(user_data);
        auto* state =
            driver ? event_for(*driver, event) : nullptr;
        if (!state) {
            return rt::CudaDriverResult::invalid_value;
        }
        driver->event_destroy_attempts_[
            static_cast<std::size_t>(event - 1)]
            .fetch_add(1, std::memory_order_relaxed);
        if (driver->fail_next_event_destroy.exchange(
                false,
                std::memory_order_acq_rel)) {
            return rt::CudaDriverResult::error;
        }
        state->recorded.store(false, std::memory_order_relaxed);
        state->ready.store(false, std::memory_order_relaxed);
        state->allocated.store(false, std::memory_order_release);
        return rt::CudaDriverResult::success;
    }

    static rt::CudaDriverResult event_record(
        void* user_data,
        rt::CudaEvent event,
        rt::CudaStream) noexcept {
        auto* driver = self(user_data);
        auto* state =
            driver ? event_for(*driver, event) : nullptr;
        if (!state) {
            return rt::CudaDriverResult::invalid_value;
        }
        record(*driver, 'E');
        if (driver->fail_next_event_record.exchange(
                false,
                std::memory_order_acq_rel)) {
            return rt::CudaDriverResult::error;
        }
        state->recorded.store(true, std::memory_order_release);
        state->ready.store(
            driver->complete_on_record.load(
                std::memory_order_acquire),
            std::memory_order_release);
        return rt::CudaDriverResult::success;
    }

    static rt::CudaDriverResult event_query(
        void* user_data,
        rt::CudaEvent event) noexcept {
        auto* driver = self(user_data);
        auto* state =
            driver ? event_for(*driver, event) : nullptr;
        if (!state) {
            return rt::CudaDriverResult::invalid_value;
        }
        if (driver->lose_context_on_query.load(
                std::memory_order_acquire)) {
            return rt::CudaDriverResult::context_lost;
        }
        if (driver->fail_next_event_query.exchange(
                false,
                std::memory_order_acq_rel)) {
            return rt::CudaDriverResult::launch_failure;
        }
        return state->ready.load(std::memory_order_acquire)
            ? rt::CudaDriverResult::success
            : rt::CudaDriverResult::not_ready;
    }

    static rt::CudaDriverResult event_synchronize(
        void* user_data,
        rt::CudaEvent event) noexcept {
        auto* driver = self(user_data);
        auto* state =
            driver ? event_for(*driver, event) : nullptr;
        if (!state) {
            return rt::CudaDriverResult::invalid_value;
        }
        if (driver->fail_next_event_sync.exchange(
                false,
                std::memory_order_acq_rel)) {
            return rt::CudaDriverResult::launch_failure;
        }
        state->ready.store(true, std::memory_order_release);
        driver->event_syncs.fetch_add(1, std::memory_order_relaxed);
        return rt::CudaDriverResult::success;
    }

    static rt::CudaDriverResult stream_synchronize(
        void* user_data,
        rt::CudaStream) noexcept {
        auto* driver = self(user_data);
        if (!driver) {
            return rt::CudaDriverResult::invalid_value;
        }
        if (driver->fail_next_stream_sync.exchange(
                false,
                std::memory_order_acq_rel)) {
            return rt::CudaDriverResult::launch_failure;
        }
        driver->make_events_ready();
        driver->stream_syncs.fetch_add(1, std::memory_order_relaxed);
        return rt::CudaDriverResult::success;
    }

    static rt::CudaDriverResult mem_alloc(
        void* user_data,
        std::uint64_t bytes,
        rt::CudaDeviceAddress* out_address) noexcept {
        auto* driver = self(user_data);
        if (!driver || !out_address ||
            bytes == 0 || bytes > allocation_bytes) {
            return rt::CudaDriverResult::invalid_value;
        }
        for (auto& allocation : driver->memory_) {
            if (!allocation.allocated) {
                allocation.allocated = true;
                *out_address =
                    reinterpret_cast<std::uintptr_t>(
                        allocation.bytes.data());
                driver->allocations.fetch_add(
                    1,
                    std::memory_order_relaxed);
                return rt::CudaDriverResult::success;
            }
        }
        return rt::CudaDriverResult::out_of_memory;
    }

    static rt::CudaDriverResult mem_free(
        void* user_data,
        rt::CudaDeviceAddress address) noexcept {
        auto* driver = self(user_data);
        if (!driver || address == 0) {
            return rt::CudaDriverResult::invalid_value;
        }
        if (driver->fail_next_mem_free.exchange(
                false,
                std::memory_order_acq_rel)) {
            return rt::CudaDriverResult::launch_failure;
        }
        for (auto& allocation : driver->memory_) {
            if (reinterpret_cast<std::uintptr_t>(
                    allocation.bytes.data()) == address &&
                allocation.allocated) {
                allocation.allocated = false;
                driver->frees.fetch_add(
                    1,
                    std::memory_order_relaxed);
                return rt::CudaDriverResult::success;
            }
        }
        return rt::CudaDriverResult::invalid_value;
    }

    static rt::CudaDriverResult host_register(
        void* user_data,
        void* address,
        std::uint64_t bytes) noexcept {
        auto* driver = self(user_data);
        if (!driver || !address || bytes == 0) {
            return rt::CudaDriverResult::invalid_value;
        }
        if (driver->fail_next_host_register.exchange(
                false,
                std::memory_order_acq_rel)) {
            return rt::CudaDriverResult::error;
        }
        driver->host_registrations.fetch_add(
            1,
            std::memory_order_relaxed);
        return rt::CudaDriverResult::success;
    }

    static rt::CudaDriverResult host_unregister(
        void* user_data,
        void* address) noexcept {
        auto* driver = self(user_data);
        if (!driver || !address) {
            return rt::CudaDriverResult::invalid_value;
        }
        driver->host_unregistrations.fetch_add(
            1,
            std::memory_order_relaxed);
        return rt::CudaDriverResult::success;
    }

    static rt::CudaDriverResult memcpy_host_to_device_async(
        void* user_data,
        rt::CudaDeviceAddress destination,
        const void* source,
        std::uint64_t bytes,
        rt::CudaStream) noexcept {
        if (destination == 0 || !source || bytes == 0) {
            return rt::CudaDriverResult::invalid_value;
        }
        if (auto* driver = self(user_data)) {
            record(*driver, 'H');
        }
        std::memcpy(
            reinterpret_cast<void*>(
                static_cast<std::uintptr_t>(destination)),
            source,
            static_cast<std::size_t>(bytes));
        return rt::CudaDriverResult::success;
    }

    static rt::CudaDriverResult memcpy_device_to_host_async(
        void* user_data,
        void* destination,
        rt::CudaDeviceAddress source,
        std::uint64_t bytes,
        rt::CudaStream) noexcept {
        if (!destination || source == 0 || bytes == 0) {
            return rt::CudaDriverResult::invalid_value;
        }
        if (auto* driver = self(user_data)) {
            record(*driver, 'D');
        }
        std::memcpy(
            destination,
            reinterpret_cast<const void*>(
                static_cast<std::uintptr_t>(source)),
            static_cast<std::size_t>(bytes));
        return rt::CudaDriverResult::success;
    }

    static rt::CudaDriverResult memcpy_device_to_device_async(
        void*,
        rt::CudaDeviceAddress destination,
        rt::CudaDeviceAddress source,
        std::uint64_t bytes,
        rt::CudaStream) noexcept {
        if (destination == 0 || source == 0 || bytes == 0) {
            return rt::CudaDriverResult::invalid_value;
        }
        std::memmove(
            reinterpret_cast<void*>(
                static_cast<std::uintptr_t>(destination)),
            reinterpret_cast<const void*>(
                static_cast<std::uintptr_t>(source)),
            static_cast<std::size_t>(bytes));
        return rt::CudaDriverResult::success;
    }

    static rt::CudaDriverResult memset_d8_async(
        void*,
        rt::CudaDeviceAddress destination,
        std::uint8_t value,
        std::uint64_t bytes,
        rt::CudaStream) noexcept {
        if (destination == 0 || bytes == 0) {
            return rt::CudaDriverResult::invalid_value;
        }
        std::memset(
            reinterpret_cast<void*>(
                static_cast<std::uintptr_t>(destination)),
            value,
            static_cast<std::size_t>(bytes));
        return rt::CudaDriverResult::success;
    }

    static rt::CudaDriverResult launch_kernel(
        void* user_data,
        rt::CudaFunction function,
        std::uint32_t,
        std::uint32_t,
        std::uint32_t,
        std::uint32_t,
        std::uint32_t,
        std::uint32_t,
        std::uint32_t,
        rt::CudaStream,
        void* const* arguments) noexcept {
        auto* driver = self(user_data);
        if (!driver || function != add_one_function ||
            !arguments || !arguments[0] || !arguments[1]) {
            return rt::CudaDriverResult::invalid_value;
        }
        rt::CudaDeviceAddress address = 0;
        std::uint32_t count = 0;
        std::memcpy(&address, arguments[0], sizeof(address));
        std::memcpy(&count, arguments[1], sizeof(count));
        auto* values = reinterpret_cast<std::int32_t*>(
            static_cast<std::uintptr_t>(address));
        for (std::uint32_t index = 0; index < count; ++index) {
            ++values[index];
        }
        driver->launches.fetch_add(1, std::memory_order_relaxed);
        return rt::CudaDriverResult::success;
    }

    static rt::CudaDriverResult graph_launch(
        void* user_data,
        rt::CudaGraphExec requested,
        rt::CudaStream) noexcept {
        auto* driver = self(user_data);
        if (!driver || requested != graph) {
            return rt::CudaDriverResult::invalid_value;
        }
        record(*driver, 'G');
        driver->graph_launches.fetch_add(1, std::memory_order_relaxed);
        return driver->fail_next_graph_launch.exchange(
                   false, std::memory_order_acq_rel)
            ? rt::CudaDriverResult::launch_failure
            : rt::CudaDriverResult::success;
    }

    static std::uint64_t monotonic_time_ns(
        void* user_data) noexcept {
        auto* driver = self(user_data);
        return driver
            ? driver->now_ns_.load(std::memory_order_relaxed)
            : 0;
    }

    std::array<EventState, event_capacity> events_{};
    std::array<
        std::atomic<std::uint64_t>,
        event_capacity> event_destroy_attempts_{};
    std::array<Allocation, allocation_capacity> memory_{};
    std::atomic<std::uint64_t> now_ns_{1};
};

rt::CudaBackendConfig config(
    std::span<const rt::CudaStream> streams,
    std::size_t queue_capacity = 4,
    std::size_t buffer_capacity = 4) {
    rt::CudaBackendConfig result{};
    result.queue_capacity = queue_capacity;
    result.buffer_capacity = buffer_capacity;
    result.kernel_capacity = 4;
    result.context = FakeCudaDriver::context;
    result.streams = streams;
    return result;
}

rtfw_device_status initialize(
    rtfw_device_backend_api& api,
    std::size_t queue_capacity,
    std::size_t buffer_capacity) {
    rtfw_device_init_config requested{};
    requested.struct_size = sizeof(requested);
    requested.abi_version = RTFW_DEVICE_ABI_VERSION;
    requested.requested_in_flight = queue_capacity;
    requested.requested_registered_buffers = buffer_capacity;
    return api.initialize(api.instance, &requested);
}

std::uint64_t register_buffer(
    rtfw_device_backend_api& api,
    const char* name,
    void* data,
    std::size_t bytes) {
    rtfw_device_buffer_registration registration{};
    registration.struct_size = sizeof(registration);
    registration.flags =
        RTFW_DEVICE_BUFFER_HOST_READ |
        RTFW_DEVICE_BUFFER_HOST_WRITE |
        RTFW_DEVICE_BUFFER_DEVICE_READ |
        RTFW_DEVICE_BUFFER_DEVICE_WRITE;
    registration.data = data;
    registration.bytes = bytes;
    std::copy_n(
        name,
        std::strlen(name) + 1,
        registration.name);
    std::uint64_t token = 0;
    EXPECT_EQ(
        api.register_buffer(
            api.instance,
            &registration,
            &token),
        RTFW_DEVICE_STATUS_OK);
    return token;
}

rt::DeviceSubmission submission(
    std::uint64_t id,
    std::uint32_t opcode,
    std::uint64_t timeout_ns = 1000) {
    auto result = rt::make_device_submission();
    result.submission_id = id;
    result.timeout_ns = timeout_ns;
    result.opcode = opcode;
    return result;
}

rtfw_device_completion poll_one(
    rtfw_device_backend_api& api,
    std::uint64_t& count) {
    rtfw_device_completion completion{};
    count = 0;
    EXPECT_EQ(
        api.poll(api.instance, &completion, 1, &count),
        RTFW_DEVICE_STATUS_OK);
    return completion;
}

void complete_submission(
    rtfw_device_backend_api& api,
    rt::DeviceSubmission& requested) {
    ASSERT_EQ(
        api.submit(api.instance, &requested),
        RTFW_DEVICE_STATUS_OK);
    std::uint64_t count = 0;
    const auto completion = poll_one(api, count);
    ASSERT_EQ(count, 1u);
    ASSERT_EQ(completion.submission_id, requested.submission_id);
    ASSERT_EQ(completion.status, RTFW_DEVICE_STATUS_OK);
}

rt::CallbackResult prepare_cuda_noop(
    void*,
    const rt::DeviceCallbackContext&,
    rt::DeviceSubmission& requested) {
    requested.timeout_ns = 1'000'000;
    requested.opcode = rt::cuda_device_opcode_noop;
    return rt::CallbackResult::ok;
}

rt::CallbackResult mark_cuda_dependent(
    void* user_data,
    const rt::CallbackContext&) {
    auto& called = *static_cast<std::atomic<bool>*>(user_data);
    called.store(true, std::memory_order_release);
    return rt::CallbackResult::ok;
}

TEST(CudaBackend, RejectsMalformedDriverAndSetup) {
    FakeCudaDriver driver;
    const std::array<rt::CudaStream, 1> streams{0x51u};
    auto driver_api = driver.api();
    driver_api.event_query = nullptr;
    EXPECT_THROW(
        (void)rt::CudaDeviceBackend(
            driver_api,
            config(streams)),
        std::invalid_argument);

    driver_api = driver.api();
    rt::CudaDeviceBackend backend(driver_api, config(streams));
    auto api = backend.api();
    EXPECT_EQ(initialize(api, 4, 4), RTFW_DEVICE_STATUS_OK);
    std::array<std::byte, 16> bytes{};
    rtfw_device_buffer_registration registration{};
    registration.struct_size = sizeof(registration);
    registration.flags = RTFW_DEVICE_BUFFER_DEVICE_READ;
    registration.data = bytes.data();
    registration.bytes = bytes.size();
    std::memcpy(registration.name, "bad name", sizeof("bad name"));
    std::uint64_t token = 0;
    EXPECT_EQ(
        api.register_buffer(
            api.instance,
            &registration,
            &token),
        RTFW_DEVICE_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(api.shutdown(api.instance), RTFW_DEVICE_STATUS_OK);
}

TEST(CudaBackend, FailedInitializeCleanupRetriesOnlyUnresolvedEvents) {
    FakeCudaDriver driver;
    const std::array<rt::CudaStream, 1> streams{0x51u};
    rt::CudaDeviceBackend backend(
        driver.api(),
        config(streams, 3, 1));
    auto api = backend.api();

    driver.event_create_failure_call.store(
        3,
        std::memory_order_release);
    driver.fail_next_event_destroy.store(
        true,
        std::memory_order_release);
    EXPECT_EQ(
        initialize(api, 3, 0),
        RTFW_DEVICE_STATUS_RESOURCE_EXHAUSTED);
    EXPECT_EQ(driver.event_create_calls.load(), 3u);
    EXPECT_EQ(driver.event_destroy_attempts(1), 1u);
    EXPECT_EQ(driver.event_destroy_attempts(2), 1u);
    EXPECT_EQ(
        initialize(api, 3, 0),
        RTFW_DEVICE_STATUS_INVALID_STATE);

    driver.fail_next_event_destroy.store(
        true,
        std::memory_order_release);
    EXPECT_EQ(
        api.shutdown(api.instance),
        RTFW_DEVICE_STATUS_RESET_REQUIRED);
    EXPECT_EQ(driver.event_destroy_attempts(1), 1u);
    EXPECT_EQ(driver.event_destroy_attempts(2), 2u);
    EXPECT_EQ(
        initialize(api, 3, 0),
        RTFW_DEVICE_STATUS_INVALID_STATE);

    EXPECT_EQ(api.shutdown(api.instance), RTFW_DEVICE_STATUS_OK);
    EXPECT_EQ(driver.event_destroy_attempts(1), 1u);
    EXPECT_EQ(driver.event_destroy_attempts(2), 3u);
    EXPECT_EQ(initialize(api, 3, 0), RTFW_DEVICE_STATUS_OK);
    EXPECT_EQ(api.shutdown(api.instance), RTFW_DEVICE_STATUS_OK);
}

TEST(CudaBackend, FailedInitializePopRequiresCleanupBeforeReuse) {
    FakeCudaDriver driver;
    const std::array<rt::CudaStream, 1> streams{0x51u};
    rt::CudaDeviceBackend backend(
        driver.api(),
        config(streams, 2, 1));
    auto api = backend.api();

    driver.fail_next_pop_context.store(
        true,
        std::memory_order_release);
    EXPECT_EQ(initialize(api, 2, 0), RTFW_DEVICE_STATUS_ERROR);
    EXPECT_EQ(driver.event_destroy_attempts(1), 1u);
    EXPECT_EQ(driver.event_destroy_attempts(2), 1u);
    EXPECT_EQ(
        initialize(api, 2, 0),
        RTFW_DEVICE_STATUS_INVALID_STATE);

    EXPECT_EQ(api.shutdown(api.instance), RTFW_DEVICE_STATUS_OK);
    EXPECT_EQ(driver.event_destroy_attempts(1), 1u);
    EXPECT_EQ(driver.event_destroy_attempts(2), 1u);
    EXPECT_EQ(initialize(api, 2, 0), RTFW_DEVICE_STATUS_OK);
    EXPECT_EQ(api.shutdown(api.instance), RTFW_DEVICE_STATUS_OK);
}

TEST(CudaBackend, BoundedQueueSaturatesAndTimeoutQuarantinesUntilReady) {
    FakeCudaDriver driver;
    const std::array<rt::CudaStream, 1> streams{0x51u};
    rt::CudaDeviceBackend backend(
        driver.api(),
        config(streams, 1, 1));
    auto api = backend.api();
    ASSERT_EQ(initialize(api, 1, 0), RTFW_DEVICE_STATUS_OK);

    auto first = submission(
        1,
        rt::cuda_device_opcode_noop,
        10);
    auto second = submission(
        2,
        rt::cuda_device_opcode_noop,
        10);
    ASSERT_EQ(
        api.submit(api.instance, &first),
        RTFW_DEVICE_STATUS_OK);
    EXPECT_EQ(
        api.submit(api.instance, &second),
        RTFW_DEVICE_STATUS_QUEUE_FULL);

    driver.advance(11);
    std::uint64_t count = 0;
    (void)poll_one(api, count);
    EXPECT_EQ(count, 0u);
    auto health = rt::make_device_health();
    ASSERT_EQ(
        api.get_health(api.instance, &health),
        RTFW_DEVICE_STATUS_OK);
    EXPECT_EQ(health.outstanding, 1u);
    EXPECT_EQ(health.timeouts, 0u);

    driver.make_events_ready();
    const auto completion = poll_one(api, count);
    ASSERT_EQ(count, 1u);
    EXPECT_EQ(completion.submission_id, 1u);
    EXPECT_EQ(completion.status, RTFW_DEVICE_STATUS_TIMEOUT);
    health = rt::make_device_health();
    ASSERT_EQ(
        api.get_health(api.instance, &health),
        RTFW_DEVICE_STATUS_OK);
    EXPECT_EQ(health.state, RTFW_DEVICE_HEALTH_DEGRADED);
    EXPECT_EQ(health.outstanding, 0u);
    EXPECT_EQ(health.timeouts, 1u);
    EXPECT_EQ(api.shutdown(api.instance), RTFW_DEVICE_STATUS_OK);
}

TEST(CudaBackend, CopiesLaunchesKernelAndReturnsDeviceData) {
    FakeCudaDriver driver;
    driver.complete_on_record.store(true, std::memory_order_release);
    const std::array<rt::CudaStream, 2> streams{0x51u, 0x52u};
    rt::CudaDeviceBackend backend(
        driver.api(),
        config(streams));
    std::uint64_t kernel_token = 0;
    ASSERT_EQ(
        backend.register_kernel(
            FakeCudaDriver::add_one_function,
            kernel_token),
        RTFW_DEVICE_STATUS_OK);
    auto api = backend.api();
    ASSERT_EQ(initialize(api, 4, 1), RTFW_DEVICE_STATUS_OK);

    std::array<std::int32_t, 8> values{
        1, 2, 3, 4, 5, 6, 7, 8};
    const auto buffer_token = register_buffer(
        api,
        "values",
        values.data(),
        sizeof(values));

    auto upload = submission(
        1,
        rt::cuda_device_opcode_copy_host_to_device);
    upload.buffer_count = 1;
    upload.buffers[0].buffer_token = buffer_token;
    upload.buffers[0].access = RTFW_DEVICE_ACCESS_READ;
    upload.buffers[0].bytes = sizeof(values);
    complete_submission(api, upload);

    rt::CudaKernelLaunch launch{};
    launch.kernel_token = kernel_token;
    launch.grid_x = 1;
    launch.block_x = 8;
    ASSERT_TRUE(rt::cuda_kernel_add_buffer_argument(launch, 0));
    const auto value_count =
        static_cast<std::uint32_t>(values.size());
    ASSERT_TRUE(rt::cuda_kernel_add_scalar_argument(
        launch,
        value_count));
    auto kernel = submission(
        2,
        rt::cuda_device_opcode_launch_kernel);
    kernel.buffer_count = 1;
    kernel.buffers[0].buffer_token = buffer_token;
    kernel.buffers[0].access = RTFW_DEVICE_ACCESS_READ_WRITE;
    kernel.buffers[0].bytes = sizeof(values);
    rt::set_cuda_kernel_launch(kernel, launch);
    complete_submission(api, kernel);

    std::fill(values.begin(), values.end(), 0);
    auto download = submission(
        3,
        rt::cuda_device_opcode_copy_device_to_host);
    download.buffer_count = 1;
    download.buffers[0].buffer_token = buffer_token;
    download.buffers[0].access = RTFW_DEVICE_ACCESS_WRITE;
    download.buffers[0].bytes = sizeof(values);
    complete_submission(api, download);

    EXPECT_EQ(
        values,
        (std::array<std::int32_t, 8>{
            2, 3, 4, 5, 6, 7, 8, 9}));
    EXPECT_EQ(driver.launches.load(std::memory_order_acquire), 1u);
    EXPECT_EQ(
        api.unregister_buffer(api.instance, buffer_token),
        RTFW_DEVICE_STATUS_OK);
    EXPECT_EQ(api.shutdown(api.instance), RTFW_DEVICE_STATUS_OK);
    EXPECT_EQ(
        driver.allocations.load(std::memory_order_acquire),
        driver.frees.load(std::memory_order_acquire));
    EXPECT_EQ(
        driver.host_registrations.load(std::memory_order_acquire),
        driver.host_unregistrations.load(std::memory_order_acquire));
}

TEST(CudaBackend, DeviceCopyAndMemsetValidateAndPreserveRanges) {
    FakeCudaDriver driver;
    driver.complete_on_record.store(true, std::memory_order_release);
    const std::array<rt::CudaStream, 1> streams{0x51u};
    rt::CudaDeviceBackend backend(
        driver.api(),
        config(streams, 4, 2));
    auto api = backend.api();
    ASSERT_EQ(initialize(api, 4, 2), RTFW_DEVICE_STATUS_OK);

    std::array<std::byte, 16> source{};
    std::array<std::byte, 16> destination{};
    for (std::size_t index = 0; index < source.size(); ++index) {
        source[index] = static_cast<std::byte>(index + 1);
    }
    const auto source_token = register_buffer(
        api,
        "copy.source",
        source.data(),
        source.size());
    const auto destination_token = register_buffer(
        api,
        "copy.destination",
        destination.data(),
        destination.size());

    auto upload = submission(
        1,
        rt::cuda_device_opcode_copy_host_to_device);
    upload.buffer_count = 1;
    upload.buffers[0].buffer_token = source_token;
    upload.buffers[0].access = RTFW_DEVICE_ACCESS_READ;
    upload.buffers[0].bytes = source.size();
    complete_submission(api, upload);

    auto fill = submission(2, rt::cuda_device_opcode_memset_d8);
    fill.payload_size = 1;
    fill.payload[0] = 0xa5u;
    fill.buffer_count = 1;
    fill.buffers[0].buffer_token = destination_token;
    fill.buffers[0].access = RTFW_DEVICE_ACCESS_WRITE;
    fill.buffers[0].offset = 4;
    fill.buffers[0].bytes = 8;
    complete_submission(api, fill);

    auto download = submission(
        3,
        rt::cuda_device_opcode_copy_device_to_host);
    download.buffer_count = 1;
    download.buffers[0].buffer_token = destination_token;
    download.buffers[0].access = RTFW_DEVICE_ACCESS_WRITE;
    download.buffers[0].bytes = destination.size();
    complete_submission(api, download);
    for (std::size_t index = 0; index < destination.size(); ++index) {
        EXPECT_EQ(
            destination[index],
            index >= 4 && index < 12
                ? std::byte{0xa5}
                : std::byte{0});
    }

    auto copy = submission(
        4,
        rt::cuda_device_opcode_copy_device_to_device);
    copy.buffer_count = 2;
    copy.buffers[0].buffer_token = source_token;
    copy.buffers[0].access = RTFW_DEVICE_ACCESS_READ;
    copy.buffers[0].bytes = source.size();
    copy.buffers[1].buffer_token = destination_token;
    copy.buffers[1].access = RTFW_DEVICE_ACCESS_WRITE;
    copy.buffers[1].bytes = destination.size();
    complete_submission(api, copy);

    std::fill(destination.begin(), destination.end(), std::byte{0});
    download.submission_id = 5;
    complete_submission(api, download);
    EXPECT_EQ(destination, source);

    auto out_of_range = submission(
        6,
        rt::cuda_device_opcode_memset_d8);
    out_of_range.payload_size = 1;
    out_of_range.payload[0] = 0xffu;
    out_of_range.buffer_count = 1;
    out_of_range.buffers[0].buffer_token = destination_token;
    out_of_range.buffers[0].access = RTFW_DEVICE_ACCESS_WRITE;
    out_of_range.buffers[0].offset = destination.size() - 1;
    out_of_range.buffers[0].bytes = 2;
    EXPECT_EQ(
        api.submit(api.instance, &out_of_range),
        RTFW_DEVICE_STATUS_INVALID_ARGUMENT);

    auto overlapping = submission(
        7,
        rt::cuda_device_opcode_copy_device_to_device);
    overlapping.buffer_count = 2;
    overlapping.buffers[0].buffer_token = destination_token;
    overlapping.buffers[0].access = RTFW_DEVICE_ACCESS_READ;
    overlapping.buffers[0].bytes = 8;
    overlapping.buffers[1].buffer_token = destination_token;
    overlapping.buffers[1].access = RTFW_DEVICE_ACCESS_WRITE;
    overlapping.buffers[1].offset = 4;
    overlapping.buffers[1].bytes = 8;
    EXPECT_EQ(
        api.submit(api.instance, &overlapping),
        RTFW_DEVICE_STATUS_INVALID_ARGUMENT);

    EXPECT_EQ(
        api.unregister_buffer(api.instance, destination_token),
        RTFW_DEVICE_STATUS_OK);
    EXPECT_EQ(
        api.unregister_buffer(api.instance, source_token),
        RTFW_DEVICE_STATUS_OK);
    EXPECT_EQ(api.shutdown(api.instance), RTFW_DEVICE_STATUS_OK);
}

TEST(CudaBackend, ExternalDeviceBindingRetainsCallerOwnership) {
    FakeCudaDriver driver;
    driver.complete_on_record.store(true, std::memory_order_release);
    const std::array<rt::CudaStream, 1> streams{0x51u};
    auto backend_config = config(streams, 2, 1);
    backend_config.allocate_device_mirrors = false;
    backend_config.register_host_memory = false;
    rt::CudaDeviceBackend backend(
        driver.api(),
        backend_config);

    EXPECT_EQ(
        backend.bind_device_buffer(
            "overflow",
            std::numeric_limits<rt::CudaDeviceAddress>::max() - 3,
            8),
        RTFW_DEVICE_STATUS_INVALID_ARGUMENT);

    std::array<std::byte, 16> host{};
    std::array<std::byte, 16> external_device{};
    for (std::size_t index = 0; index < host.size(); ++index) {
        host[index] = static_cast<std::byte>(0x20 + index);
    }
    ASSERT_EQ(
        backend.bind_device_buffer(
            "external",
            reinterpret_cast<std::uintptr_t>(
                external_device.data()),
            external_device.size()),
        RTFW_DEVICE_STATUS_OK);

    auto api = backend.api();
    ASSERT_EQ(initialize(api, 2, 1), RTFW_DEVICE_STATUS_OK);
    const auto token = register_buffer(
        api,
        "external",
        host.data(),
        host.size());
    auto upload = submission(
        1,
        rt::cuda_device_opcode_copy_host_to_device);
    upload.buffer_count = 1;
    upload.buffers[0].buffer_token = token;
    upload.buffers[0].access = RTFW_DEVICE_ACCESS_READ;
    upload.buffers[0].bytes = host.size();
    complete_submission(api, upload);

    EXPECT_EQ(external_device, host);
    EXPECT_EQ(
        api.unregister_buffer(api.instance, token),
        RTFW_DEVICE_STATUS_OK);
    EXPECT_EQ(api.shutdown(api.instance), RTFW_DEVICE_STATUS_OK);
    EXPECT_EQ(driver.allocations.load(), 0u);
    EXPECT_EQ(driver.frees.load(), 0u);
    EXPECT_EQ(driver.host_registrations.load(), 0u);
    EXPECT_EQ(driver.host_unregistrations.load(), 0u);
}

TEST(CudaBackend, FailedRegistrationRetainsOwnershipForShutdown) {
    FakeCudaDriver driver;
    const std::array<rt::CudaStream, 1> streams{0x51u};
    rt::CudaDeviceBackend backend(
        driver.api(),
        config(streams, 1, 1));
    auto api = backend.api();
    ASSERT_EQ(initialize(api, 1, 1), RTFW_DEVICE_STATUS_OK);

    std::array<std::byte, 32> bytes{};
    rtfw_device_buffer_registration registration{};
    registration.struct_size = sizeof(registration);
    registration.flags =
        RTFW_DEVICE_BUFFER_HOST_READ |
        RTFW_DEVICE_BUFFER_DEVICE_WRITE;
    registration.data = bytes.data();
    registration.bytes = bytes.size();
    std::memcpy(
        registration.name,
        "registration.failure",
        sizeof("registration.failure"));
    driver.fail_next_host_register.store(
        true,
        std::memory_order_release);
    driver.fail_next_mem_free.store(
        true,
        std::memory_order_release);
    std::uint64_t token = 99;
    EXPECT_EQ(
        api.register_buffer(
            api.instance,
            &registration,
            &token),
        RTFW_DEVICE_STATUS_RESET_REQUIRED);
    EXPECT_EQ(token, 0u);
    EXPECT_EQ(driver.allocations.load(), 1u);
    EXPECT_EQ(driver.frees.load(), 0u);

    EXPECT_EQ(api.shutdown(api.instance), RTFW_DEVICE_STATUS_OK);
    EXPECT_EQ(driver.frees.load(), 1u);
    EXPECT_EQ(driver.host_registrations.load(), 0u);
    EXPECT_EQ(driver.host_unregistrations.load(), 0u);
}

TEST(CudaBackend, FailedEnqueueIsQuarantinedAndResetDrainsIt) {
    FakeCudaDriver driver;
    const std::array<rt::CudaStream, 1> streams{0x51u};
    rt::CudaDeviceBackend backend(
        driver.api(),
        config(streams, 1, 1));
    auto api = backend.api();
    ASSERT_EQ(initialize(api, 1, 0), RTFW_DEVICE_STATUS_OK);

    driver.fail_next_event_record.store(
        true,
        std::memory_order_release);
    auto requested = submission(
        1,
        rt::cuda_device_opcode_noop);
    EXPECT_EQ(
        api.submit(api.instance, &requested),
        RTFW_DEVICE_STATUS_ERROR);
    auto health = rt::make_device_health();
    ASSERT_EQ(
        api.get_health(api.instance, &health),
        RTFW_DEVICE_STATUS_OK);
    EXPECT_EQ(health.state, RTFW_DEVICE_HEALTH_RESET_REQUIRED);
    EXPECT_EQ(health.outstanding, 1u);
    EXPECT_EQ(api.reset(api.instance), RTFW_DEVICE_STATUS_OK);
    EXPECT_EQ(
        driver.stream_syncs.load(std::memory_order_acquire),
        1u);

    health = rt::make_device_health();
    ASSERT_EQ(
        api.get_health(api.instance, &health),
        RTFW_DEVICE_STATUS_OK);
    EXPECT_EQ(health.state, RTFW_DEVICE_HEALTH_HEALTHY);
    EXPECT_EQ(health.outstanding, 0u);
    EXPECT_EQ(health.resets, 1u);
    EXPECT_EQ(api.shutdown(api.instance), RTFW_DEVICE_STATUS_OK);
}

TEST(CudaBackend, QueryFailureRequiresSuccessfulDrainBeforeReuse) {
    FakeCudaDriver driver;
    const std::array<rt::CudaStream, 1> streams{0x51u};
    rt::CudaDeviceBackend backend(
        driver.api(),
        config(streams, 1, 1));
    auto api = backend.api();
    ASSERT_EQ(initialize(api, 1, 0), RTFW_DEVICE_STATUS_OK);

    auto requested = submission(
        1,
        rt::cuda_device_opcode_noop);
    ASSERT_EQ(
        api.submit(api.instance, &requested),
        RTFW_DEVICE_STATUS_OK);
    driver.fail_next_event_query.store(
        true,
        std::memory_order_release);
    rtfw_device_completion completion{};
    std::uint64_t count = 9;
    EXPECT_EQ(
        api.poll(api.instance, &completion, 1, &count),
        RTFW_DEVICE_STATUS_RESET_REQUIRED);
    EXPECT_EQ(count, 0u);

    auto health = rt::make_device_health();
    ASSERT_EQ(
        api.get_health(api.instance, &health),
        RTFW_DEVICE_STATUS_OK);
    EXPECT_EQ(health.state, RTFW_DEVICE_HEALTH_RESET_REQUIRED);
    EXPECT_EQ(health.outstanding, 1u);

    driver.fail_next_stream_sync.store(
        true,
        std::memory_order_release);
    EXPECT_EQ(
        api.reset(api.instance),
        RTFW_DEVICE_STATUS_RESET_REQUIRED);
    ASSERT_EQ(
        api.get_health(api.instance, &health),
        RTFW_DEVICE_STATUS_OK);
    EXPECT_EQ(health.state, RTFW_DEVICE_HEALTH_RESET_REQUIRED);
    EXPECT_EQ(health.outstanding, 1u);

    EXPECT_EQ(api.reset(api.instance), RTFW_DEVICE_STATUS_OK);
    ASSERT_EQ(
        api.get_health(api.instance, &health),
        RTFW_DEVICE_STATUS_OK);
    EXPECT_EQ(health.state, RTFW_DEVICE_HEALTH_HEALTHY);
    EXPECT_EQ(health.outstanding, 0u);
    EXPECT_EQ(api.shutdown(api.instance), RTFW_DEVICE_STATUS_OK);
}

TEST(CudaBackend, ShutdownDrainsOutstandingWorkBeforeFreeingBuffers) {
    FakeCudaDriver driver;
    const std::array<rt::CudaStream, 1> streams{0x51u};
    rt::CudaDeviceBackend backend(
        driver.api(),
        config(streams, 1, 1));
    auto api = backend.api();
    ASSERT_EQ(initialize(api, 1, 1), RTFW_DEVICE_STATUS_OK);
    std::array<std::byte, 32> bytes{};
    const auto token = register_buffer(
        api,
        "pending",
        bytes.data(),
        bytes.size());
    auto requested = submission(
        1,
        rt::cuda_device_opcode_copy_host_to_device);
    requested.buffer_count = 1;
    requested.buffers[0].buffer_token = token;
    requested.buffers[0].access = RTFW_DEVICE_ACCESS_READ;
    requested.buffers[0].bytes = bytes.size();
    ASSERT_EQ(
        api.submit(api.instance, &requested),
        RTFW_DEVICE_STATUS_OK);
    EXPECT_EQ(
        api.unregister_buffer(api.instance, token),
        RTFW_DEVICE_STATUS_INVALID_STATE);

    EXPECT_EQ(api.shutdown(api.instance), RTFW_DEVICE_STATUS_OK);
    EXPECT_EQ(
        driver.event_syncs.load(std::memory_order_acquire),
        1u);
    EXPECT_EQ(driver.allocations.load(), 1u);
    EXPECT_EQ(driver.frees.load(), 1u);
}

TEST(CudaBackend, FailedShutdownRetainsResourcesAndCanBeRetried) {
    FakeCudaDriver driver;
    const std::array<rt::CudaStream, 1> streams{0x51u};
    rt::CudaDeviceBackend backend(
        driver.api(),
        config(streams, 1, 1));
    auto api = backend.api();
    ASSERT_EQ(initialize(api, 1, 1), RTFW_DEVICE_STATUS_OK);
    std::array<std::byte, 32> bytes{};
    const auto token = register_buffer(
        api,
        "shutdown.retry",
        bytes.data(),
        bytes.size());
    auto requested = submission(
        1,
        rt::cuda_device_opcode_copy_host_to_device);
    requested.buffer_count = 1;
    requested.buffers[0].buffer_token = token;
    requested.buffers[0].access = RTFW_DEVICE_ACCESS_READ;
    requested.buffers[0].bytes = bytes.size();
    ASSERT_EQ(
        api.submit(api.instance, &requested),
        RTFW_DEVICE_STATUS_OK);

    driver.fail_next_event_sync.store(
        true,
        std::memory_order_release);
    EXPECT_EQ(
        api.shutdown(api.instance),
        RTFW_DEVICE_STATUS_RESET_REQUIRED);
    EXPECT_EQ(driver.frees.load(), 0u);
    EXPECT_EQ(driver.host_unregistrations.load(), 0u);
    EXPECT_EQ(
        api.submit(api.instance, &requested),
        RTFW_DEVICE_STATUS_INVALID_STATE);
    EXPECT_EQ(
        initialize(api, 1, 1),
        RTFW_DEVICE_STATUS_INVALID_STATE);

    EXPECT_EQ(api.shutdown(api.instance), RTFW_DEVICE_STATUS_OK);
    EXPECT_EQ(driver.allocations.load(), 1u);
    EXPECT_EQ(driver.frees.load(), 1u);
    EXPECT_EQ(driver.host_registrations.load(), 1u);
    EXPECT_EQ(driver.host_unregistrations.load(), 1u);
    auto health = rt::make_device_health();
    ASSERT_EQ(
        api.get_health(api.instance, &health),
        RTFW_DEVICE_STATUS_OK);
    EXPECT_EQ(health.state, RTFW_DEVICE_HEALTH_SHUTDOWN);
    EXPECT_EQ(health.outstanding, 0u);
}

TEST(CudaBackend, ContextLossCompletesAsLostAndCannotSoftReset) {
    FakeCudaDriver driver;
    const std::array<rt::CudaStream, 1> streams{0x51u};
    rt::CudaDeviceBackend backend(
        driver.api(),
        config(streams, 1, 1));
    auto api = backend.api();
    ASSERT_EQ(initialize(api, 1, 0), RTFW_DEVICE_STATUS_OK);
    auto requested = submission(
        1,
        rt::cuda_device_opcode_noop);
    ASSERT_EQ(
        api.submit(api.instance, &requested),
        RTFW_DEVICE_STATUS_OK);
    driver.lose_context_on_query.store(
        true,
        std::memory_order_release);
    std::uint64_t count = 0;
    const auto completion = poll_one(api, count);
    ASSERT_EQ(count, 1u);
    EXPECT_EQ(completion.status, RTFW_DEVICE_STATUS_LOST);
    EXPECT_EQ(api.reset(api.instance), RTFW_DEVICE_STATUS_LOST);
    auto health = rt::make_device_health();
    ASSERT_EQ(
        api.get_health(api.instance, &health),
        RTFW_DEVICE_STATUS_OK);
    EXPECT_EQ(health.state, RTFW_DEVICE_HEALTH_LOST);
    EXPECT_EQ(health.outstanding, 1u);
    EXPECT_EQ(api.shutdown(api.instance), RTFW_DEVICE_STATUS_OK);
    EXPECT_EQ(driver.stream_syncs.load(), 1u);
    EXPECT_EQ(initialize(api, 1, 0), RTFW_DEVICE_STATUS_LOST);
}

TEST(CudaBackend, ConcurrentSubmissionsStayWithinFixedCapacity) {
    FakeCudaDriver driver;
    driver.complete_on_record.store(true, std::memory_order_release);
    const std::array<rt::CudaStream, 4> streams{
        0x51u, 0x52u, 0x53u, 0x54u};
    constexpr std::size_t capacity = 16;
    rt::CudaDeviceBackend backend(
        driver.api(),
        config(streams, capacity, 1));
    auto api = backend.api();
    ASSERT_EQ(initialize(api, capacity, 0), RTFW_DEVICE_STATUS_OK);

    std::atomic<std::uint64_t> accepted{0};
    std::atomic<std::uint64_t> rejected{0};
    std::array<std::thread, 4> producers;
    for (std::size_t producer = 0;
         producer < producers.size();
         ++producer) {
        producers[producer] = std::thread([&, producer] {
            for (std::size_t index = 0; index < 32; ++index) {
                auto requested = submission(
                    1 + producer * 32 + index,
                    rt::cuda_device_opcode_noop);
                const auto status =
                    api.submit(api.instance, &requested);
                if (status == RTFW_DEVICE_STATUS_OK) {
                    accepted.fetch_add(1, std::memory_order_relaxed);
                } else if (
                    status == RTFW_DEVICE_STATUS_QUEUE_FULL) {
                    rejected.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto& producer : producers) {
        producer.join();
    }
    EXPECT_EQ(accepted.load(), capacity);
    EXPECT_EQ(accepted.load() + rejected.load(), 128u);

    std::array<rtfw_device_completion, capacity> completions{};
    std::uint64_t count = 0;
    EXPECT_EQ(
        api.poll(
            api.instance,
            completions.data(),
            completions.size(),
            &count),
        RTFW_DEVICE_STATUS_OK);
    EXPECT_EQ(count, capacity);
    EXPECT_EQ(api.shutdown(api.instance), RTFW_DEVICE_STATUS_OK);
}

TEST(CudaBackend, RuntimeGraphAcceptsCandidateBackendAtD0) {
    FakeCudaDriver driver;
    driver.complete_on_record.store(true, std::memory_order_release);
    const std::array<rt::CudaStream, 1> streams{0x51u};
    rt::CudaDeviceBackend backend(
        driver.api(),
        config(streams, 4, 1));

    rt::Runtime runtime;
    rt::RuntimeConfig runtime_config{};
    runtime_config.callback_capacity = 2;
    runtime_config.worker_count = 2;
    runtime_config.executor_queue_capacity = 8;
    runtime_config.task_scratch_slots = 8;
    runtime_config.device_backend_capacity = 1;
    runtime_config.device_buffer_capacity = 1;
    runtime_config.device_outstanding_capacity = 4;
    runtime_config.device_completion_batch = 4;
    ASSERT_EQ(
        runtime.configure(runtime_config),
        rt::Status::ok);

    rt::DeviceBackendHandle backend_handle;
    rt::PhaseHandle device_phase;
    rt::PhaseHandle dependent_phase;
    std::atomic<bool> dependent_called{false};
    ASSERT_EQ(
        runtime.register_device_backend(
            {"cuda.fake", backend.api()},
            backend_handle),
        rt::Status::ok);
    ASSERT_EQ(
        runtime.register_device_phase(
            {
                "cuda.noop",
                backend_handle,
                &prepare_cuda_noop,
                nullptr,
            },
            device_phase),
        rt::Status::ok);
    ASSERT_EQ(
        runtime.register_callback(
            {
                "cuda.dependent",
                &mark_cuda_dependent,
                &dependent_called,
            },
            dependent_phase),
        rt::Status::ok);
    ASSERT_EQ(
        runtime.add_dependency(device_phase, dependent_phase),
        rt::Status::ok);
    ASSERT_EQ(runtime.finalize(), rt::Status::ok);
    ASSERT_EQ(runtime.start(), rt::Status::ok);
    EXPECT_EQ(
        runtime.step({
            0,
            std::chrono::milliseconds(1),
            std::nullopt,
        }),
        rt::Status::ok);
    EXPECT_TRUE(
        dependent_called.load(std::memory_order_acquire));
    EXPECT_EQ(runtime.stop(), rt::Status::ok);
}

} // namespace

TEST(CudaBackend, DriverVersionsPreserveV1PrefixAndRequireCompleteV2) {
    EXPECT_EQ(rt::cuda_driver_api_version, 1u);
    EXPECT_EQ(rt::cuda_driver_api_v1_struct_size, 224u);
    EXPECT_EQ(rt::cuda_driver_api_v2_struct_size, sizeof(rt::CudaDriverApi));
    const rt::CudaDriverApi defaults{};
    EXPECT_EQ(defaults.struct_size, rt::cuda_driver_api_v1_struct_size);
    EXPECT_EQ(defaults.api_version, rt::cuda_driver_api_version_1);
    EXPECT_EQ(defaults.graph_launch, nullptr);

    FakeCudaDriver driver;
    const std::array<rt::CudaStream, 1> streams{0x51u};
    rt::CudaDeviceBackend v1_backend(driver.api(), config(streams));
    EXPECT_EQ(v1_backend.hal_v2_registration("cuda.v1").api.instance, nullptr);
    auto malformed = driver.api();
    malformed.graph_launch = +[](void*, rt::CudaGraphExec,
                                 rt::CudaStream) noexcept {
        return rt::CudaDriverResult::success;
    };
    EXPECT_THROW((void)rt::CudaDeviceBackend(malformed, config(streams)),
                 std::invalid_argument);
    malformed = driver.api_v2();
    malformed.struct_size = rt::cuda_driver_api_v1_struct_size;
    EXPECT_THROW((void)rt::CudaDeviceBackend(malformed, config(streams)),
                 std::invalid_argument);
    malformed = driver.api_v2();
    malformed.reserved_v2[0] = 1;
    EXPECT_THROW((void)rt::CudaDeviceBackend(malformed, config(streams)),
                 std::invalid_argument);
}

TEST(CudaBackend, GraphRegistryIsBoundedUniqueCopiedAndFrozen) {
    FakeCudaDriver driver;
    const std::array<rt::CudaStream, 1> streams{0x51u};
    rt::CudaDeviceBackend backend(driver.api_v2(), config(streams));
    const std::array bindings{
        rt::CudaGraphBufferBinding{"buffer.a", RTFW_DEVICE_ACCESS_READ_WRITE}};
    EXPECT_EQ(backend.register_graph(0, FakeCudaDriver::graph, bindings),
              RTFW_DEVICE_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(backend.register_graph(7, FakeCudaDriver::graph, bindings),
              RTFW_DEVICE_STATUS_OK);
    EXPECT_EQ(backend.register_graph(7, FakeCudaDriver::graph + 1, bindings),
              RTFW_DEVICE_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(backend.register_graph(8, FakeCudaDriver::graph, bindings),
              RTFW_DEVICE_STATUS_INVALID_ARGUMENT);
    for (std::uint16_t id = 8; id <= 22; ++id) {
        EXPECT_EQ(backend.register_graph(
                      id, FakeCudaDriver::graph + id, bindings),
                  RTFW_DEVICE_STATUS_OK);
    }
    EXPECT_EQ(backend.register_graph(23, FakeCudaDriver::graph + 23, bindings),
              RTFW_DEVICE_STATUS_RESOURCE_EXHAUSTED);
    auto registration = backend.hal_v2_registration("cuda.native");
    rt::HalV2InitializeConfig initialize{};
    initialize.requested_in_flight = 4;
    initialize.requested_registered_buffers = 4;
    ASSERT_EQ(registration.api.initialize(registration.api.instance, &initialize),
              rt::HalV2Status::ok);
    EXPECT_EQ(backend.register_graph(23, FakeCudaDriver::graph + 23, bindings),
              RTFW_DEVICE_STATUS_INVALID_STATE);
    EXPECT_EQ(registration.api.shutdown(registration.api.instance),
              rt::HalV2Status::ok);
}

TEST(CudaBackend, NativeRegistrationDiscoversTruthfulStagedMemory) {
    FakeCudaDriver driver;
    const std::array<rt::CudaStream, 1> streams{0x51u};
    rt::CudaDeviceBackend backend(driver.api_v2(), config(streams));
    const auto registration = backend.hal_v2_registration("cuda.native");
    ASSERT_NE(registration.memory_topology, nullptr);
    ASSERT_NE(registration.command_timeline, nullptr);
    rt::HalV2Capabilities capabilities{};
    EXPECT_EQ(registration.api.get_capabilities(registration.api.instance,
                                                &capabilities),
              rt::HalV2Status::ok);
    EXPECT_STREQ(capabilities.backend_id.data(), "rtfw.cuda.native.v2");
    rt::HalV2MemoryTopologySnapshot snapshot{};
    EXPECT_EQ(registration.memory_topology->discover(
                  registration.memory_topology->instance, &snapshot),
              rt::HalV2Status::ok);
    ASSERT_EQ(snapshot.memory_domain_count, 2u);
    EXPECT_EQ(snapshot.memory_domains[1].kind,
              static_cast<std::uint32_t>(
                  rt::HalV2MemoryDomainKind::pinned_host));
    EXPECT_EQ(snapshot.memory_domains[1].coherency,
              static_cast<std::uint32_t>(
                  rt::HalV2MemoryCoherency::staged_copy));
    EXPECT_EQ(snapshot.completion_timestamp_domain_identity, 1u);
    rt::HalV2CommandTimelineCapabilities command{};
    EXPECT_EQ(registration.command_timeline->get_capabilities(
                  registration.command_timeline->instance, &command),
              rt::HalV2Status::ok);
    EXPECT_EQ(command.max_commands_per_batch,
              rt::hal_v2_command_capacity);
}

TEST(CudaBackend, NativeStagedDomainDoesNotInventPinningWhenDisabled) {
    FakeCudaDriver driver;
    const std::array<rt::CudaStream, 1> streams{0x51u};
    auto requested = config(streams);
    requested.register_host_memory = false;
    rt::CudaDeviceBackend backend(driver.api_v2(), requested);
    const auto registration = backend.hal_v2_registration("cuda.native");
    rt::HalV2MemoryTopologySnapshot snapshot{};
    ASSERT_EQ(registration.memory_topology->discover(
                  registration.memory_topology->instance, &snapshot),
              rt::HalV2Status::ok);
    ASSERT_EQ(snapshot.memory_domain_count, 2u);
    EXPECT_EQ(snapshot.memory_domains[1].kind,
              static_cast<std::uint32_t>(rt::HalV2MemoryDomainKind::host));
    EXPECT_EQ(snapshot.memory_domains[1].coherency,
              static_cast<std::uint32_t>(
                  rt::HalV2MemoryCoherency::staged_copy));
}

TEST(CudaBackend, NativeGraphBatchExecutesCopyGraphCopyThenOneEvent) {
    FakeCudaDriver driver;
    driver.complete_on_record.store(true, std::memory_order_release);
    const std::array<rt::CudaStream, 1> streams{0x51u};
    rt::CudaDeviceBackend backend(driver.api_v2(), config(streams, 4, 1));
    const std::array bindings{
        rt::CudaGraphBufferBinding{"buffer.a", RTFW_DEVICE_ACCESS_READ_WRITE}};
    ASSERT_EQ(backend.register_graph(7, FakeCudaDriver::graph, bindings),
              RTFW_DEVICE_STATUS_OK);
    const auto registration = backend.hal_v2_registration("cuda.native");
    rt::HalV2InitializeConfig initialize{};
    initialize.requested_in_flight = 4;
    initialize.requested_registered_buffers = 1;
    ASSERT_EQ(registration.api.initialize(registration.api.instance, &initialize),
              rt::HalV2Status::ok);
    std::array<std::byte, 16> storage{};
    rt::HalV2MemoryRegistration memory{};
    memory.domain_identity = 2;
    memory.bytes = storage.size();
    memory.ownership = static_cast<std::uint32_t>(
        rt::HalV2MemoryOwnership::borrowed_host);
    memory.access = RTFW_DEVICE_BUFFER_HOST_READ |
                    RTFW_DEVICE_BUFFER_HOST_WRITE |
                    RTFW_DEVICE_BUFFER_DEVICE_READ |
                    RTFW_DEVICE_BUFFER_DEVICE_WRITE;
    memory.coherency = static_cast<std::uint32_t>(
        rt::HalV2MemoryCoherency::staged_copy);
    memory.synchronization = rt::hal_v2_memory_sync_copy_to_device |
                             rt::hal_v2_memory_sync_copy_from_device;
    memory.host_data = storage.data();
    std::copy_n("buffer.a", sizeof("buffer.a"), memory.name.begin());
    rt::HalV2MemoryToken token{};
    ASSERT_EQ(registration.memory_topology->register_memory(
                  registration.memory_topology->instance, &memory, &token),
              rt::HalV2Status::ok);

    rt::DeviceCommandBatch batch{};
    batch.batch_id = 1;
    batch.frame_index = 3;
    batch.timeout_ns = 1000;
    batch.command_count = 3;
    batch.signal_count = 1;
    batch.signals[0].timeline_handle = 11;
    batch.signals[0].value = 1;
    auto reference = rt::HalV2BufferReference{
        token.submission_token, RTFW_DEVICE_ACCESS_READ, 0, 0,
        storage.size()};
    auto& upload = batch.commands[0];
    upload.kind = static_cast<std::uint32_t>(rt::HalV2CommandKind::copy);
    upload.operation = static_cast<std::uint32_t>(
        rt::HalV2MemoryOperation::copy_to_device);
    upload.source = reference;
    upload.source.access = RTFW_DEVICE_ACCESS_READ;
    upload.destination = reference;
    upload.destination.access = RTFW_DEVICE_ACCESS_WRITE;
    auto& graph = batch.commands[1];
    graph.kind = static_cast<std::uint32_t>(rt::HalV2CommandKind::dispatch);
    graph.opcode = rt::cuda_device_opcode_graph(7);
    graph.buffer_count = 1;
    graph.buffers[0] = reference;
    graph.buffers[0].access = RTFW_DEVICE_ACCESS_READ_WRITE;
    auto& download = batch.commands[2];
    download = upload;
    download.operation = static_cast<std::uint32_t>(
        rt::HalV2MemoryOperation::copy_from_device);
    ASSERT_EQ(registration.command_timeline->submit(
                  registration.command_timeline->instance, &batch),
              rt::HalV2Status::ok);
    rt::HalV2BatchCompletion completion{};
    std::uint64_t count = 0;
    EXPECT_EQ(registration.command_timeline->poll(
                  registration.command_timeline->instance, &completion, 1,
                  &count),
              rt::HalV2Status::ok);
    ASSERT_EQ(count, 1u);
    EXPECT_EQ(completion.batch_id, 1u);
    EXPECT_EQ(completion.signal_count, 1u);
    EXPECT_EQ(driver.graph_launches.load(), 1u);
    ASSERT_GE(driver.call_order_count.load(), 4u);
    EXPECT_EQ(driver.call_order[0], 'H');
    EXPECT_EQ(driver.call_order[1], 'G');
    EXPECT_EQ(driver.call_order[2], 'D');
    EXPECT_EQ(driver.call_order[3], 'E');
    EXPECT_EQ(registration.memory_topology->unregister_memory(
                  registration.memory_topology->instance, &memory, &token),
              rt::HalV2Status::ok);
    EXPECT_EQ(registration.api.shutdown(registration.api.instance),
              rt::HalV2Status::ok);
}

TEST(CudaBackend, NativeGraphRejectsUnknownBindingAndLegacyBufferBeforeLaunch) {
    FakeCudaDriver driver;
    const std::array<rt::CudaStream, 1> streams{0x51u};
    rt::CudaDeviceBackend backend(driver.api_v2(), config(streams, 2, 2));
    const std::array bindings{
        rt::CudaGraphBufferBinding{"staged", RTFW_DEVICE_ACCESS_READ}};
    ASSERT_EQ(backend.register_graph(9, FakeCudaDriver::graph, bindings),
              RTFW_DEVICE_STATUS_OK);
    const auto registration = backend.hal_v2_registration("cuda.native");
    rt::HalV2InitializeConfig initialize{};
    initialize.requested_in_flight = 2;
    initialize.requested_registered_buffers = 2;
    ASSERT_EQ(registration.api.initialize(registration.api.instance, &initialize),
              rt::HalV2Status::ok);
    std::array<std::byte, 8> bytes{};
    rt::HalV2BufferRegistration legacy{};
    legacy.flags = RTFW_DEVICE_BUFFER_HOST_READ |
                   RTFW_DEVICE_BUFFER_DEVICE_READ;
    legacy.data = bytes.data();
    legacy.bytes = bytes.size();
    std::copy_n("staged", sizeof("staged"), legacy.name.begin());
    std::uint64_t token = 0;
    ASSERT_EQ(registration.api.register_buffer(registration.api.instance,
                                               &legacy, &token),
              rt::HalV2Status::ok);
    rt::DeviceCommandBatch batch{};
    batch.batch_id = 2;
    batch.timeout_ns = 100;
    batch.command_count = 1;
    batch.signal_count = 1;
    batch.signals[0].timeline_handle = 4;
    batch.signals[0].value = 1;
    auto& command = batch.commands[0];
    command.kind = static_cast<std::uint32_t>(rt::HalV2CommandKind::dispatch);
    command.opcode = rt::cuda_device_opcode_graph(9);
    command.buffer_count = 1;
    command.buffers[0] = {token, RTFW_DEVICE_ACCESS_READ, 0, 0,
                          bytes.size()};
    EXPECT_EQ(registration.command_timeline->submit(
                  registration.command_timeline->instance, &batch),
              rt::HalV2Status::invalid_argument);
    EXPECT_EQ(driver.graph_launches.load(), 0u);
    command.opcode = rt::cuda_device_opcode_graph(10);
    EXPECT_EQ(registration.command_timeline->submit(
                  registration.command_timeline->instance, &batch),
              rt::HalV2Status::invalid_argument);
    EXPECT_EQ(driver.graph_launches.load(), 0u);
    EXPECT_EQ(registration.api.unregister_buffer(registration.api.instance,
                                                 token),
              rt::HalV2Status::ok);
    EXPECT_EQ(registration.api.shutdown(registration.api.instance),
              rt::HalV2Status::ok);
}

TEST(CudaBackend, NativePathRejectsDuplicateDeviceV1Use) {
    FakeCudaDriver driver;
    const std::array<rt::CudaStream, 1> streams{0x51u};
    rt::CudaDeviceBackend backend(driver.api_v2(), config(streams));
    const auto native = backend.hal_v2_registration("cuda.native");
    rt::HalV2Capabilities capabilities{};
    ASSERT_EQ(native.api.get_capabilities(native.api.instance, &capabilities),
              rt::HalV2Status::ok);
    auto v1 = backend.api();
    rtfw_device_capabilities v1_capabilities{};
    v1_capabilities.struct_size = sizeof(v1_capabilities);
    EXPECT_EQ(v1.get_capabilities(v1.instance, &v1_capabilities),
              RTFW_DEVICE_STATUS_INVALID_STATE);
}

TEST(CudaBackend, NativeGraphFailureQuarantinesUntilReset) {
    FakeCudaDriver driver;
    const std::array<rt::CudaStream, 1> streams{0x51u};
    rt::CudaDeviceBackend backend(driver.api_v2(), config(streams, 1, 1));
    const std::array<rt::CudaGraphBufferBinding, 0> bindings{};
    ASSERT_EQ(backend.register_graph(3, FakeCudaDriver::graph, bindings),
              RTFW_DEVICE_STATUS_OK);
    const auto registration = backend.hal_v2_registration("cuda.native");
    rt::HalV2InitializeConfig initialize{};
    initialize.requested_in_flight = 1;
    initialize.requested_registered_buffers = 1;
    ASSERT_EQ(registration.api.initialize(registration.api.instance, &initialize),
              rt::HalV2Status::ok);
    rt::DeviceCommandBatch batch{};
    batch.batch_id = 1;
    batch.timeout_ns = 100;
    batch.command_count = 1;
    batch.signal_count = 1;
    batch.signals[0].timeline_handle = 1;
    batch.signals[0].value = 1;
    batch.commands[0].kind = static_cast<std::uint32_t>(
        rt::HalV2CommandKind::dispatch);
    batch.commands[0].opcode = rt::cuda_device_opcode_graph(3);
    driver.fail_next_graph_launch.store(true, std::memory_order_release);
    EXPECT_EQ(registration.command_timeline->submit(
                  registration.command_timeline->instance, &batch),
              rt::HalV2Status::reset_required);
    rt::HalV2Health health{};
    ASSERT_EQ(registration.api.get_health(registration.api.instance, &health),
              rt::HalV2Status::ok);
    EXPECT_EQ(health.state, static_cast<std::uint32_t>(
                                rt::HalV2HealthState::reset_required));
    EXPECT_EQ(registration.api.reset(registration.api.instance),
              rt::HalV2Status::ok);
    EXPECT_EQ(driver.stream_syncs.load(), 1u);
    batch.batch_id = 2;
    batch.signals[0].value = 2;
    EXPECT_EQ(registration.command_timeline->submit(
                  registration.command_timeline->instance, &batch),
              rt::HalV2Status::ok);
    EXPECT_EQ(registration.api.shutdown(registration.api.instance),
              rt::HalV2Status::ok);
}

TEST(CudaBackend, NativeGraphTimeoutRetainsUntilEventReadyAndStopIsIdempotent) {
    FakeCudaDriver driver;
    const std::array<rt::CudaStream, 1> streams{0x51u};
    rt::CudaDeviceBackend backend(driver.api_v2(), config(streams, 1, 1));
    const std::array<rt::CudaGraphBufferBinding, 0> bindings{};
    ASSERT_EQ(backend.register_graph(4, FakeCudaDriver::graph, bindings),
              RTFW_DEVICE_STATUS_OK);
    const auto registration = backend.hal_v2_registration("cuda.native");
    rt::HalV2InitializeConfig initialize{};
    initialize.requested_in_flight = 1;
    initialize.requested_registered_buffers = 1;
    ASSERT_EQ(registration.api.initialize(registration.api.instance, &initialize),
              rt::HalV2Status::ok);
    rt::DeviceCommandBatch batch{};
    batch.batch_id = 1;
    batch.timeout_ns = 10;
    batch.command_count = 1;
    batch.signal_count = 1;
    batch.signals[0].timeline_handle = 1;
    batch.signals[0].value = 1;
    batch.commands[0].kind = static_cast<std::uint32_t>(
        rt::HalV2CommandKind::dispatch);
    batch.commands[0].opcode = rt::cuda_device_opcode_graph(4);
    ASSERT_EQ(registration.command_timeline->submit(
                  registration.command_timeline->instance, &batch),
              rt::HalV2Status::ok);
    driver.advance(11);
    rt::HalV2BatchCompletion completion{};
    std::uint64_t count = 0;
    EXPECT_EQ(registration.command_timeline->poll(
                  registration.command_timeline->instance, &completion, 1,
                  &count),
              rt::HalV2Status::ok);
    EXPECT_EQ(count, 0u);
    EXPECT_EQ(registration.command_timeline->request_stop(
                  registration.command_timeline->instance),
              rt::HalV2Status::ok);
    EXPECT_EQ(registration.command_timeline->request_stop(
                  registration.command_timeline->instance),
              rt::HalV2Status::ok);
    driver.make_events_ready();
    EXPECT_EQ(registration.command_timeline->poll(
                  registration.command_timeline->instance, &completion, 1,
                  &count),
              rt::HalV2Status::ok);
    ASSERT_EQ(count, 1u);
    EXPECT_EQ(completion.status,
              static_cast<std::int32_t>(rt::HalV2Status::timeout));
    EXPECT_EQ(registration.api.shutdown(registration.api.instance),
              rt::HalV2Status::ok);
}

namespace {
struct StoppedCudaNative {
    std::unique_ptr<FakeCudaDriver> driver=std::make_unique<FakeCudaDriver>();
    std::array<rt::CudaStream,1> streams{0x51u};
    std::unique_ptr<rt::CudaDeviceBackend> backend=
        std::make_unique<rt::CudaDeviceBackend>(driver->api_v2(),config(streams,1,1));
    rt::HalV2BackendRegistration registration=backend->hal_v2_registration("stopped.cuda");
    std::array<std::byte,32> bytes{};
    std::uint64_t token{};
    void start() {
        rt::HalV2InitializeConfig init{}; init.requested_in_flight=1; init.requested_registered_buffers=1;
        ASSERT_EQ(registration.api.initialize(registration.api.instance,&init),rt::HalV2Status::ok);
        rt::HalV2BufferRegistration buffer; buffer.data=bytes.data(); buffer.bytes=bytes.size();
        buffer.flags=RTFW_DEVICE_BUFFER_HOST_READ|RTFW_DEVICE_BUFFER_HOST_WRITE|
                     RTFW_DEVICE_BUFFER_DEVICE_READ|RTFW_DEVICE_BUFFER_DEVICE_WRITE;
        std::copy_n("stopped.buffer",sizeof("stopped.buffer"),buffer.name.begin());
        ASSERT_EQ(registration.api.register_buffer(registration.api.instance,&buffer,&token),rt::HalV2Status::ok);
        rt::DeviceCommandBatch batch; batch.batch_id=1; batch.timeout_ns=1'000'000'000;
        batch.command_count=1; batch.signal_count=1;
        batch.signals[0].timeline_handle=1; batch.signals[0].value=1;
        auto& command=batch.commands[0]; command.kind=static_cast<std::uint32_t>(rt::HalV2CommandKind::dispatch);
        command.opcode=rt::cuda_device_opcode_copy_host_to_device; command.buffer_count=1;
        command.buffers[0]={token,RTFW_DEVICE_ACCESS_READ,0,0,bytes.size()};
        ASSERT_EQ(registration.command_timeline->submit(registration.command_timeline->instance,&batch),rt::HalV2Status::ok);
    }
    rt::HalV2Status unregister() { return registration.api.unregister_buffer(registration.api.instance,token); }
    void stop() { ASSERT_EQ(registration.command_timeline->request_stop(registration.command_timeline->instance),rt::HalV2Status::ok); }
    ~StoppedCudaNative() { (void)registration.api.shutdown(registration.api.instance); }
};
}

TEST(CudaBackend, StoppedHalV2PendingUnregisterRetiresExactlyOnceAndRetries) {
    StoppedCudaNative fixture; fixture.start(); auto& driver=*fixture.driver;
    EXPECT_EQ(fixture.unregister(),rt::HalV2Status::invalid_state);
    EXPECT_EQ(driver.event_syncs.load(),0u); EXPECT_EQ(driver.host_unregistrations.load(),0u);
    fixture.stop(); driver.fail_next_event_sync=true;
    EXPECT_NE(fixture.unregister(),rt::HalV2Status::ok);
    rt::HalV2Health health;
    ASSERT_EQ(fixture.registration.api.get_health(fixture.registration.api.instance,&health),rt::HalV2Status::ok);
    EXPECT_EQ(health.outstanding,1u); EXPECT_EQ(driver.host_unregistrations.load(),0u); EXPECT_EQ(driver.frees.load(),0u);
    EXPECT_EQ(fixture.unregister(),rt::HalV2Status::ok);
    EXPECT_EQ(driver.event_syncs.load(),1u); EXPECT_EQ(driver.host_unregistrations.load(),1u); EXPECT_EQ(driver.frees.load(),1u);
    ASSERT_EQ(fixture.registration.api.get_health(fixture.registration.api.instance,&health),rt::HalV2Status::ok);
    EXPECT_EQ(health.outstanding,0u);
    EXPECT_EQ(fixture.registration.api.shutdown(fixture.registration.api.instance),rt::HalV2Status::ok);
    EXPECT_EQ(driver.event_syncs.load(),1u);
}

TEST(CudaBackend, StoppedHalV2QuarantineAndLossRetainOnFailedSynchronization) {
    for(bool lost:{false,true}) {
        StoppedCudaNative fixture; fixture.start(); auto& driver=*fixture.driver;
        if(lost) driver.lose_context_on_query=true; else driver.fail_next_event_query=true;
        rt::HalV2BatchCompletion completion; std::uint64_t count=0;
        const auto polled=fixture.registration.command_timeline->poll(
            fixture.registration.command_timeline->instance,&completion,1,&count);
        EXPECT_EQ(polled,lost?rt::HalV2Status::ok:rt::HalV2Status::reset_required);
        if(lost) {
            EXPECT_EQ(count,1u); EXPECT_EQ(completion.status,static_cast<std::int32_t>(rt::HalV2Status::lost));
            EXPECT_EQ(fixture.registration.api.reset(fixture.registration.api.instance),rt::HalV2Status::lost);
        }
        EXPECT_EQ(fixture.unregister(),rt::HalV2Status::invalid_state);
        fixture.stop(); driver.fail_next_stream_sync=true;
        EXPECT_NE(fixture.unregister(),rt::HalV2Status::ok);
        EXPECT_EQ(driver.frees.load(),0u); EXPECT_EQ(driver.host_unregistrations.load(),0u);
        rt::HalV2Health health;
        ASSERT_EQ(fixture.registration.api.get_health(fixture.registration.api.instance,&health),rt::HalV2Status::ok);
        EXPECT_EQ(health.outstanding,1u);
        EXPECT_EQ(fixture.unregister(),rt::HalV2Status::ok);
        EXPECT_EQ(driver.stream_syncs.load(),1u); EXPECT_EQ(driver.frees.load(),1u);
        ASSERT_EQ(fixture.registration.api.get_health(fixture.registration.api.instance,&health),rt::HalV2Status::ok);
        EXPECT_EQ(health.outstanding,0u);
        if(lost) { EXPECT_EQ(fixture.registration.api.reset(fixture.registration.api.instance),rt::HalV2Status::lost); }
    }
}

#include "../samples/cuda_physics/pipeline/simulated_driver.hpp"

TEST(CudaBackend, StoppedHalV2RuntimePipelineRetiresPendingAndLostBuffers) {
    namespace p=rtfw::cuda_physics::pipeline;
    for(bool active:{false,true}) for(bool graph:{false,true}) for(bool lost:{false,true}) {
        SCOPED_TRACE(::testing::Message()<<"active="<<active<<" graph="<<graph<<" lost="<<lost);
        p::Options options{{17,1,0,2},graph,active};
        auto driver=std::make_unique<p::SimulatedDriver>(options);
        auto session=driver->session();
        if(lost) session.driver.event_query=[](void*,rt::CudaEvent) noexcept { return rt::CudaDriverResult::context_lost; };
        auto scenario=std::make_unique<p::Scenario>(options,session);
        ASSERT_EQ(scenario->prepare(),rt::Status::ok);
        ASSERT_EQ(scenario->start(),rt::Status::ok);
        if(lost) {
            EXPECT_EQ(scenario->step(),rt::Status::device_lost);
        } else {
            driver->hold=true;
            rt::Status result=rt::Status::internal_error;
            std::thread worker([&]{result=scenario->step();});
            const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(3);
            while((driver->records<2 || driver->not_ready==0) && std::chrono::steady_clock::now()<until)
                std::this_thread::yield();
            const bool accepted=driver->records==2 && driver->not_ready>0;
            const auto requested=scenario->stop();
            worker.join();
            EXPECT_TRUE(accepted);
            EXPECT_EQ(requested,rt::Status::invalid_state);
            // Active references whose vendor cannot cancel remain quarantined
            // until their finite completion budget; ordinary graph stop is immediate.
            EXPECT_EQ(result,active?rt::Status::device_timeout:rt::Status::device_canceled);
        }
        EXPECT_EQ(scenario->completed(),0u);
        EXPECT_EQ(scenario->publications(0),0u); EXPECT_EQ(scenario->publications(1),0u);
        EXPECT_EQ(scenario->stop(),rt::Status::ok);
        EXPECT_EQ(scenario->stop(),rt::Status::ok);
        EXPECT_TRUE(driver->clean());
        EXPECT_EQ(driver->registrations.load(),4u);
        EXPECT_EQ(driver->unregistrations.load(),4u);
        EXPECT_EQ(driver->backend_allocations.load(),0u); EXPECT_EQ(driver->backend_frees.load(),0u);
        EXPECT_TRUE(driver->protocol_ok.load());
    }
}

namespace {
rt::HalV2MemoryRegistration stopped_memory(std::span<std::byte> bytes) {
    rt::HalV2MemoryRegistration memory;
    memory.domain_identity=2; memory.bytes=bytes.size(); memory.host_data=bytes.data();
    memory.ownership=static_cast<std::uint32_t>(rt::HalV2MemoryOwnership::borrowed_host);
    memory.access=RTFW_DEVICE_BUFFER_HOST_READ|RTFW_DEVICE_BUFFER_HOST_WRITE|
                  RTFW_DEVICE_BUFFER_DEVICE_READ|RTFW_DEVICE_BUFFER_DEVICE_WRITE;
    memory.coherency=static_cast<std::uint32_t>(rt::HalV2MemoryCoherency::staged_copy);
    memory.synchronization=rt::hal_v2_memory_sync_copy_to_device|rt::hal_v2_memory_sync_copy_from_device;
    std::copy_n("rollback.buffer",sizeof("rollback.buffer"),memory.name.begin());
    return memory;
}
}

TEST(CudaBackend, StoppedHalV2UncertainRegistrationRollsBackAbsentOrResidualOwnership) {
    for(bool residual:{false,true}) {
        StoppedCudaNative fixture;
        auto& r=fixture.registration; auto& driver=*fixture.driver;
        rt::HalV2InitializeConfig init; init.requested_in_flight=1; init.requested_registered_buffers=1;
        ASSERT_EQ(r.api.initialize(r.api.instance,&init),rt::HalV2Status::ok);
        const auto memory=stopped_memory(fixture.bytes); rt::HalV2MemoryToken token;
        driver.fail_next_host_register=true; driver.fail_next_mem_free=residual;
        EXPECT_NE(r.memory_topology->register_memory(r.memory_topology->instance,&memory,&token),rt::HalV2Status::ok);
        EXPECT_EQ(token.submission_token,0u);
        EXPECT_EQ(driver.allocations.load(),1u); EXPECT_EQ(driver.host_registrations.load(),0u);
        auto invalid=memory; invalid.reserved[0]=1;
        EXPECT_EQ(r.memory_topology->unregister_memory(r.memory_topology->instance,&invalid,&token),rt::HalV2Status::invalid_argument);
        if(residual) {
            invalid=memory; ++invalid.bytes;
            EXPECT_EQ(r.memory_topology->unregister_memory(r.memory_topology->instance,&invalid,&token),rt::HalV2Status::invalid_argument);
            driver.fail_next_mem_free=true;
            EXPECT_NE(r.memory_topology->unregister_memory(r.memory_topology->instance,&memory,&token),rt::HalV2Status::ok);
            EXPECT_EQ(driver.frees.load(),0u);
        }
        EXPECT_EQ(r.memory_topology->unregister_memory(r.memory_topology->instance,&memory,&token),rt::HalV2Status::ok);
        EXPECT_EQ(r.memory_topology->unregister_memory(r.memory_topology->instance,&memory,&token),rt::HalV2Status::ok);
        EXPECT_EQ(driver.frees.load(),1u);
        EXPECT_EQ(r.api.shutdown(r.api.instance),rt::HalV2Status::ok);
    }
}

TEST(CudaBackend, StoppedHalV2ZeroTokenCannotReleaseSuccessfulRegistration) {
    StoppedCudaNative fixture; auto& r=fixture.registration;
    rt::HalV2InitializeConfig init; init.requested_in_flight=1; init.requested_registered_buffers=1;
    ASSERT_EQ(r.api.initialize(r.api.instance,&init),rt::HalV2Status::ok);
    const auto memory=stopped_memory(fixture.bytes); rt::HalV2MemoryToken token,empty;
    ASSERT_EQ(r.memory_topology->register_memory(r.memory_topology->instance,&memory,&token),rt::HalV2Status::ok);
    EXPECT_EQ(r.memory_topology->unregister_memory(r.memory_topology->instance,&memory,&empty),rt::HalV2Status::invalid_argument);
    auto invalid=token; invalid.reserved[0]=1;
    EXPECT_EQ(r.memory_topology->unregister_memory(r.memory_topology->instance,&memory,&invalid),rt::HalV2Status::invalid_argument);
    auto descriptor=memory; descriptor.coherency=0;
    EXPECT_EQ(r.memory_topology->unregister_memory(r.memory_topology->instance,&descriptor,&token),rt::HalV2Status::invalid_argument);
    EXPECT_EQ(fixture.driver->frees.load(),0u); EXPECT_EQ(fixture.driver->host_unregistrations.load(),0u);
    EXPECT_EQ(r.memory_topology->unregister_memory(r.memory_topology->instance,&memory,&token),rt::HalV2Status::ok);
    EXPECT_EQ(fixture.driver->frees.load(),1u); EXPECT_EQ(fixture.driver->host_unregistrations.load(),1u);
}

TEST(CudaBackend, StoppedHalV2RuntimeStartupFailureReleasesUncertainMemory) {
    namespace p=rtfw::cuda_physics::pipeline;
    struct Driver : p::SimulatedDriver {
        unsigned attempts=0;
        explicit Driver(const p::Options& options):p::SimulatedDriver(options) {}
        p::Session failing_session() {
            auto result=session();
            result.driver.host_register=[](void* driver_data,void* host,std::uint64_t bytes) noexcept {
                auto& owner=static_cast<Driver&>(*static_cast<p::SimulatedDriver*>(driver_data));
                if(++owner.attempts==2) return rt::CudaDriverResult::error;
                const auto original=owner.session().driver;
                return original.host_register(driver_data,host,bytes);
            };
            return result;
        }
    };
    for(bool active:{false,true}) for(bool graph:{false,true}) {
        p::Options options{{17,1,0,2},graph,active};
        auto driver=std::make_unique<Driver>(options);
        auto scenario=std::make_unique<p::Scenario>(options,driver->failing_session());
        ASSERT_EQ(scenario->prepare(),rt::Status::ok);
        EXPECT_NE(scenario->start(),rt::Status::ok);
        EXPECT_EQ(driver->attempts,2u);
        EXPECT_EQ(scenario->stop(),rt::Status::ok);
        EXPECT_EQ(scenario->stop(),rt::Status::ok);
        EXPECT_EQ(scenario->publications(0),0u); EXPECT_EQ(scenario->publications(1),0u);
        EXPECT_EQ(driver->registrations.load(),1u); EXPECT_EQ(driver->unregistrations.load(),1u);
        EXPECT_TRUE(driver->clean()); EXPECT_EQ(driver->backend_frees.load(),0u);
    }
}
