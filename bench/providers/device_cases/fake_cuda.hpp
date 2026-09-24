#pragma once
// Benchmark-owned protocol driver, derived from the retained public backend tests.
#include <rt/cuda_backend.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
namespace rtfw::benchmark::device::detail {
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
    std::int32_t* graph_values{};
    std::size_t graph_elements{};
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
        if(driver->fail_next_graph_launch.exchange(false, std::memory_order_acq_rel))
            return rt::CudaDriverResult::launch_failure;
        if(!driver->graph_values || !driver->graph_elements || driver->graph_elements>1024)
            return rt::CudaDriverResult::invalid_value;
        for(std::size_t i=0;i<driver->graph_elements;++i) ++driver->graph_values[i];
        return rt::CudaDriverResult::success;
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

} // namespace rtfw::benchmark::device::detail
