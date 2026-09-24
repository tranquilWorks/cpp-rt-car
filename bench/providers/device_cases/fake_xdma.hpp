#pragma once
// Fixed benchmark-owned card memory; actual XdmaDeviceBackend runs its I/O team.
#include <rt/xdma_backend.hpp>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <thread>
namespace rtfw::benchmark::device::detail {
struct FakeXdmaDriver {
    // Keep the deterministic fake below Windows' default 1 MiB thread stack.
    // The largest stress range is 64 KiB; 128 KiB leaves explicit headroom.
    std::array<std::byte, 1u << 17u> device{};
    std::atomic<bool> initialized{false};
    std::atomic<bool> blocked{false};
    std::atomic<bool> in_transfer{false};
    std::atomic<std::int32_t> next_result{
        static_cast<std::int32_t>(rt::XdmaDriverResult::success)};
    std::atomic<std::uint64_t> now_ns{1};
    std::atomic<std::uint64_t> transfers{0};
    std::atomic<std::uint64_t> resets{0};
    std::atomic<std::uint64_t> shutdowns{0};
    std::array<std::uint32_t, 64> control{};
    std::atomic<std::uint64_t> control_reads{0};
    std::atomic<std::uint64_t> control_writes{0};
    std::atomic<std::uint64_t> event_waits{0};
    std::atomic<std::uint64_t> stop_requests{0};
    std::atomic<bool> event_ready{false};
    std::atomic<bool> event_waiting{false};
    std::atomic<std::uint32_t> event_value{0};
    std::atomic<bool> fail_initialize_after_acquire_once{false};
    std::atomic<bool> fail_shutdown_once{false};
    std::atomic<bool> short_transfer_once{false};

    static FakeXdmaDriver* self(void* user_data) noexcept {
        return static_cast<FakeXdmaDriver*>(user_data);
    }

    static rt::XdmaDriverResult initialize(void* user_data) noexcept {
        auto* fake = self(user_data);
        bool expected = false;
        if (!fake ||
            !fake->initialized.compare_exchange_strong(
                expected,
                true,
                std::memory_order_acq_rel)) {
            return rt::XdmaDriverResult::invalid_value;
        }
        if (fake->fail_initialize_after_acquire_once.exchange(
                false,
                std::memory_order_acq_rel)) {
            return rt::XdmaDriverResult::io_error;
        }
        return rt::XdmaDriverResult::success;
    }

    static rt::XdmaTransferResult transfer(
        void* user_data,
        rt::XdmaDirection direction,
        std::uint32_t,
        std::uint64_t device_offset,
        void* host_data,
        std::uint64_t bytes) noexcept {
        auto* fake = self(user_data);
        rt::XdmaTransferResult output{};
        if (!fake || !host_data ||
            device_offset > fake->device.size() ||
            bytes > fake->device.size() - device_offset) {
            output.result = rt::XdmaDriverResult::invalid_value;
            return output;
        }
        fake->in_transfer.store(true, std::memory_order_release);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (fake->blocked.load(std::memory_order_acquire)) {
            if (std::chrono::steady_clock::now() >= deadline) {
                output.result = rt::XdmaDriverResult::timeout;
                fake->in_transfer.store(false, std::memory_order_release);
                return output;
            }
            std::this_thread::yield();
        }
        const auto requested = static_cast<rt::XdmaDriverResult>(
            fake->next_result.exchange(
                static_cast<std::int32_t>(
                    rt::XdmaDriverResult::success),
                std::memory_order_acq_rel));
        if (requested != rt::XdmaDriverResult::success) {
            output.result = requested;
            fake->in_transfer.store(false, std::memory_order_release);
            return output;
        }
        const auto transferred = bytes - (fake->short_transfer_once.exchange(false) ? 1u : 0u);
        auto* device = fake->device.data() +
            static_cast<std::size_t>(device_offset);
        if (direction == rt::XdmaDirection::host_to_card) {
            std::memcpy(
                device,
                host_data,
                static_cast<std::size_t>(transferred));
        } else {
            std::memcpy(
                host_data,
                device,
                static_cast<std::size_t>(transferred));
        }
        output.result = rt::XdmaDriverResult::success;
        output.bytes_transferred = transferred;
        fake->transfers.fetch_add(1, std::memory_order_relaxed);
        fake->now_ns.fetch_add(10, std::memory_order_relaxed);
        fake->in_transfer.store(false, std::memory_order_release);
        return output;
    }

    static rt::XdmaDriverResult reset(void* user_data) noexcept {
        auto* fake = self(user_data);
        if (!fake || !fake->initialized.load(std::memory_order_acquire)) {
            return rt::XdmaDriverResult::invalid_value;
        }
        fake->resets.fetch_add(1, std::memory_order_relaxed);
        return rt::XdmaDriverResult::success;
    }

    static rt::XdmaDriverResult shutdown(void* user_data) noexcept {
        auto* fake = self(user_data);
        if (!fake) {
            return rt::XdmaDriverResult::invalid_value;
        }
        fake->shutdowns.fetch_add(1, std::memory_order_relaxed);
        if (fake->fail_shutdown_once.exchange(
                false,
                std::memory_order_acq_rel)) {
            return rt::XdmaDriverResult::reset_required;
        }
        bool expected = true;
        return fake->initialized.compare_exchange_strong(
                   expected,
                   false,
                   std::memory_order_acq_rel)
            ? rt::XdmaDriverResult::success
            : rt::XdmaDriverResult::invalid_value;
    }

    static std::uint64_t monotonic(void* user_data) noexcept {
        auto* fake = self(user_data);
        return fake ? fake->now_ns.load(std::memory_order_acquire) : 0;
    }

    static rt::XdmaControlReadResult control_read32(
        void* user_data,
        std::uint32_t offset) noexcept {
        auto* fake = self(user_data);
        rt::XdmaControlReadResult output{};
        if (!fake || (offset & 3u) != 0 || offset / 4u >= fake->control.size()) {
            output.result = rt::XdmaDriverResult::invalid_value;
            return output;
        }
        fake->control_reads.fetch_add(1, std::memory_order_relaxed);
        output.value = fake->control[offset / 4u];
        output.result = rt::XdmaDriverResult::success;
        return output;
    }

    static rt::XdmaDriverResult control_write32(
        void* user_data,
        std::uint32_t offset,
        std::uint32_t value) noexcept {
        auto* fake = self(user_data);
        if (!fake || (offset & 3u) != 0 || offset / 4u >= fake->control.size()) {
            return rt::XdmaDriverResult::invalid_value;
        }
        fake->control[offset / 4u] = value;
        fake->control_writes.fetch_add(1, std::memory_order_relaxed);
        return rt::XdmaDriverResult::success;
    }

    static rt::XdmaUserEventResult wait_user_event(
        void* user_data,
        std::uint32_t index,
        std::uint64_t timeout_ns) noexcept {
        auto* fake = self(user_data);
        rt::XdmaUserEventResult output{};
        if (!fake || index >= rt::xdma_user_event_capacity || timeout_ns == 0) {
            output.result = rt::XdmaDriverResult::invalid_value;
            return output;
        }
        fake->event_waits.fetch_add(1, std::memory_order_relaxed);
        fake->event_waiting.store(true, std::memory_order_release);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!fake->event_ready.load(std::memory_order_acquire) &&
               fake->blocked.load(std::memory_order_acquire)) {
            if (std::chrono::steady_clock::now() >= deadline) break;
            std::this_thread::yield();
        }
        fake->event_waiting.store(false, std::memory_order_release);
        if (!fake->event_ready.exchange(false, std::memory_order_acq_rel)) {
            output.result = rt::XdmaDriverResult::timeout;
            return output;
        }
        output.result = rt::XdmaDriverResult::success;
        output.value = fake->event_value.load(std::memory_order_acquire);
        return output;
    }

    static rt::XdmaDriverResult request_stop(void* user_data) noexcept {
        auto* fake = self(user_data);
        if (!fake) {
            return rt::XdmaDriverResult::invalid_value;
        }
        fake->stop_requests.fetch_add(1, std::memory_order_relaxed);
        fake->blocked.store(false, std::memory_order_release);
        fake->blocked.notify_all();
        return rt::XdmaDriverResult::success;
    }

    rt::XdmaDriverApi api() noexcept {
        rt::XdmaDriverApi output{};
        output.user_data = this;
        output.initialize = &initialize;
        output.transfer = &transfer;
        output.reset = &reset;
        output.shutdown = &shutdown;
        output.monotonic_time_ns = &monotonic;
        return output;
    }

    rt::XdmaDriverApi api_v2() noexcept {
        auto output = api();
        output.struct_size = sizeof(output);
        output.api_version = rt::xdma_driver_api_version_2;
        output.control_read32 = &control_read32;
        output.control_write32 = &control_write32;
        output.wait_user_event = &wait_user_event;
        output.request_stop = &request_stop;
        return output;
    }
};

}
