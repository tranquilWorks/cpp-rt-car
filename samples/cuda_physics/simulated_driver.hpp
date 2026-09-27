#pragma once
#include "scenario.hpp"
#include <algorithm>
#include <atomic>
#include <cstring>

namespace rtfw::cuda_physics {
// Fixed storage, actual copied bytes, and an iterative kernel independent of
// matches_oracle. This proves the driver protocol, never NVIDIA execution.
class SimulatedDriver {
    using R = rt::CudaDriverResult;
    static constexpr auto ok = R::success;
    static constexpr auto invalid = R::invalid_value;
    static constexpr rt::CudaContext context = 0xc001;
    static constexpr rt::CudaStream stream = 0x5001;
    static constexpr rt::CudaFunction function = 0xf001;
    struct Event { std::atomic<bool> allocated{}, recorded{}; };
    std::array<Event, 8> events_{};
    std::array<Particle, max_particles> device_{};
    std::size_t bytes_{};
    void* host_{};
    unsigned phase_{};
    Event* event(rt::CudaEvent e) noexcept {
        return e > 0 && e <= events_.size() && events_[e-1].allocated.load()
            ? &events_[e-1] : nullptr;
    }
    rt::CudaDeviceAddress address() const noexcept {
        return static_cast<rt::CudaDeviceAddress>(reinterpret_cast<std::uintptr_t>(device_.data()));
    }
    bool range(rt::CudaDeviceAddress a, std::uint64_t bytes) const noexcept {
        return allocated.load() && a == address() && bytes > 0 && bytes <= bytes_;
    }
public:
    std::atomic<bool> hold_completion{}, fail_launch{}, fail_upload{}, fail_download{},
        corrupt_output{}, fail_free{}, allocated{}, registered{}, protocol_ok{true};
    std::atomic<std::uint64_t> uploads{}, launches{}, downloads{}, upload_bytes{}, download_bytes{},
        not_ready_polls{}, allocations{}, frees{}, registrations{}, unregistrations{}, records{}, stream_syncs{};

    Session session() noexcept {
        rt::CudaDriverApi api;
        api.struct_size = rt::cuda_driver_api_v2_struct_size;
        api.api_version = rt::cuda_driver_api_version_2;
        // HAL v2 requires the v2 driver table. This sample declares no Graph.
        api.graph_launch = [](void*, rt::CudaGraphExec, rt::CudaStream) noexcept { return invalid; };
        api.user_data = this;
        api.push_context = [](void*, rt::CudaContext c) noexcept { return c == context ? ok : invalid; };
        api.pop_context = [](void*, rt::CudaContext* c) noexcept {
            if (!c) return invalid;
            *c = context; return ok;
        };
        api.event_create = [](void* p, rt::CudaEvent* out) noexcept {
            if (!out) return invalid;
            auto& s = *static_cast<SimulatedDriver*>(p);
            for (std::size_t i = 0; i < s.events_.size(); ++i) {
                if (!s.events_[i].allocated.exchange(true)) {
                    s.events_[i].recorded.store(false); *out = i+1; return ok;
                }
            }
            return R::out_of_memory;
        };
        api.event_destroy = [](void* p, rt::CudaEvent e) noexcept {
            auto* v = static_cast<SimulatedDriver*>(p)->event(e);
            if (!v) return invalid;
            v->recorded.store(false); v->allocated.store(false); return ok;
        };
        api.event_record = [](void* p, rt::CudaEvent e, rt::CudaStream st) noexcept {
            auto& s = *static_cast<SimulatedDriver*>(p);
            auto* v = s.event(e);
            if (!v || st != stream) return invalid;
            ++s.records; v->recorded.store(true); return ok;
        };
        api.event_query = [](void* p, rt::CudaEvent e) noexcept {
            auto& s = *static_cast<SimulatedDriver*>(p);
            auto* v = s.event(e);
            if (!v || !v->recorded.load()) return invalid;
            if (s.hold_completion.load()) { ++s.not_ready_polls; return R::not_ready; }
            return ok;
        };
        api.event_synchronize = [](void* p, rt::CudaEvent e) noexcept {
            auto& s = *static_cast<SimulatedDriver*>(p);
            if (!s.event(e)) return invalid;
            s.hold_completion.store(false); return ok;
        };
        api.stream_synchronize = [](void* p, rt::CudaStream st) noexcept {
            if (st != stream) return invalid;
            auto& s = *static_cast<SimulatedDriver*>(p);
            s.hold_completion.store(false); ++s.stream_syncs; return ok;
        };
        api.mem_alloc = [](void* p, std::uint64_t bytes, rt::CudaDeviceAddress* out) noexcept {
            auto& s = *static_cast<SimulatedDriver*>(p);
            if (!out || bytes == 0 || bytes > sizeof(s.device_) || s.allocated.exchange(true)) return invalid;
            s.bytes_ = static_cast<std::size_t>(bytes); *out = s.address(); ++s.allocations; return ok;
        };
        api.mem_free = [](void* p, rt::CudaDeviceAddress a) noexcept {
            auto& s = *static_cast<SimulatedDriver*>(p);
            if (!s.range(a, 1)) return invalid;
            if (s.fail_free.exchange(false)) return R::error;
            s.allocated.store(false); ++s.frees; return ok;
        };
        api.host_register = [](void* p, void* host, std::uint64_t bytes) noexcept {
            auto& s = *static_cast<SimulatedDriver*>(p);
            if (!host || !bytes || bytes > sizeof(s.device_) || s.registered.exchange(true)) return invalid;
            s.host_ = host; ++s.registrations; return ok;
        };
        api.host_unregister = [](void* p, void* host) noexcept {
            auto& s = *static_cast<SimulatedDriver*>(p);
            if (host != s.host_ || !s.registered.load()) return invalid;
            s.registered.store(false); ++s.unregistrations; return ok;
        };
        api.memcpy_host_to_device_async = [](void* p, rt::CudaDeviceAddress a, const void* host,
                                             std::uint64_t bytes, rt::CudaStream st) noexcept {
            auto& s = *static_cast<SimulatedDriver*>(p);
            if (st != stream || !s.range(a, bytes) || host != s.host_ || !s.registered.load()) return invalid;
            if (s.fail_upload.exchange(false)) return R::error;
            if (s.phase_ != 0) { s.protocol_ok.store(false); return invalid; }
            std::memcpy(s.device_.data(), host, static_cast<std::size_t>(bytes));
            s.phase_ = 1; ++s.uploads; s.upload_bytes += bytes; return ok;
        };
        api.memcpy_device_to_host_async = [](void* p, void* host, rt::CudaDeviceAddress a,
                                             std::uint64_t bytes, rt::CudaStream st) noexcept {
            auto& s = *static_cast<SimulatedDriver*>(p);
            if (st != stream || !s.range(a, bytes) || host != s.host_ || !s.registered.load()) return invalid;
            if (s.fail_download.exchange(false)) return R::error;
            if (s.phase_ != 2) { s.protocol_ok.store(false); return invalid; }
            std::memcpy(host, s.device_.data(), static_cast<std::size_t>(bytes));
            if (s.corrupt_output.exchange(false)) static_cast<Particle*>(host)[0].position[0] ^= 1;
            s.phase_ = 0; ++s.downloads; s.download_bytes += bytes; return ok;
        };
        api.memcpy_device_to_device_async = [](void* p, rt::CudaDeviceAddress dst, rt::CudaDeviceAddress src,
                                               std::uint64_t bytes, rt::CudaStream st) noexcept {
            auto& s = *static_cast<SimulatedDriver*>(p);
            return st == stream && s.range(dst, bytes) && s.range(src, bytes) ? ok : invalid;
        };
        api.memset_d8_async = [](void* p, rt::CudaDeviceAddress dst, std::uint8_t value,
                                 std::uint64_t bytes, rt::CudaStream st) noexcept {
            auto& s = *static_cast<SimulatedDriver*>(p);
            if (st != stream || !s.range(dst, bytes)) return invalid;
            auto storage = std::as_writable_bytes(std::span(s.device_));
            std::fill_n(storage.begin(), static_cast<std::size_t>(bytes), static_cast<std::byte>(value)); return ok;
        };
        api.launch_kernel = [](void* p, rt::CudaFunction f, std::uint32_t gx, std::uint32_t gy,
            std::uint32_t gz, std::uint32_t bx, std::uint32_t by, std::uint32_t bz,
            std::uint32_t shared, rt::CudaStream st, void* const* args) noexcept {
            auto& s = *static_cast<SimulatedDriver*>(p);
            if (f != function || st != stream || !args || !args[0] || !args[1] ||
                gy != 1 || gz != 1 || bx != 128 || by != 1 || bz != 1 || shared != 0) return invalid;
            rt::CudaDeviceAddress a{}; std::uint32_t count{};
            std::memcpy(&a, args[0], sizeof(a)); std::memcpy(&count, args[1], sizeof(count));
            if (!count || count > max_particles || gx != (count+127)/128 ||
                !s.range(a, static_cast<std::uint64_t>(count)*sizeof(Particle))) return invalid;
            if (s.fail_launch.exchange(false)) return R::launch_failure;
            if (s.phase_ != 1) { s.protocol_ok.store(false); return invalid; }
            for (std::uint32_t i = 0; i < count; ++i) {
                for (unsigned axis = 0; axis < 3; ++axis) {
                    auto& particle = s.device_[i];
                    particle.velocity[axis] += particle.acceleration[axis];
                    particle.position[axis] += particle.velocity[axis];
                }
            }
            s.phase_ = 2; ++s.launches; return ok;
        };
        api.monotonic_time_ns = [](void*) noexcept -> std::uint64_t {
            return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        };
        return {api, context, stream, function};
    }
    bool clean() const noexcept {
        for (const auto& e : events_) if (e.allocated.load()) return false;
        return !allocated.load() && !registered.load();
    }
};
} // namespace rtfw::cuda_physics
