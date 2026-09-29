#pragma once
#include "resources.hpp"
namespace golden::cuda {
// Derived from M24's checked injected Driver API pattern; this driver owns a
// fixed SoA simulator. No real CUDA driver is loaded or probed by this class.
class SimulatedDriver {
  using R = rt::CudaDriverResult;
  static constexpr auto ok = R::success, bad = R::invalid_value;
  static constexpr rt::CudaContext context = 0xc2603;
  static constexpr rt::CudaStream stream = 0x52603;
  static constexpr rt::CudaFunction function = 0xf2603;
  static constexpr rt::CudaGraphExec graph = 0x72603;
  std::unique_ptr<Storage> storage_ = std::make_unique<Storage>();
  std::array<void *, 2> host_{};
  std::array<bool, 2> registered_{};
  struct Event {
    std::atomic<bool> allocated{}, recorded{};
  };
  std::array<Event, 2> events_{};
  inline static thread_local const SimulatedDriver *current_{};
  std::size_t count_;
  unsigned phase_ = 0;
  bool closed_ = false;
  std::atomic<bool> live_{};
  std::atomic<std::uint64_t> logical_{1000}, offset_{};
  rt::CudaDeviceAddress address(std::size_t i) const noexcept {
    return static_cast<rt::CudaDeviceAddress>(
        reinterpret_cast<std::uintptr_t>(storage_->values[i].data()));
  }
  int buffer(rt::CudaDeviceAddress a, std::uint64_t bytes) const noexcept {
    if (bytes != sizeof(State))
      return -1;
    for (std::size_t i = 0; i < 2; ++i)
      if (a == address(i))
        return static_cast<int>(i);
    return -1;
  }
  Event *event(rt::CudaEvent e) noexcept {
    return e && e <= events_.size() && events_[e - 1].allocated.load()
               ? &events_[e - 1]
               : nullptr;
  }
  bool current() noexcept {
    if (current_ == this && !closed_)
      return true;
    protocol_ok = false;
    return false;
  }
  R integrate(rt::CudaStream st) noexcept {
    if (!current() || st != stream || phase_ != 3)
      return bad;
    auto &values = storage_->values[1];
    for (std::size_t a = 0; a < 3; ++a)
      for (std::size_t i = 0; i < fixed::capacity; ++i) {
        const auto x = a * fixed::capacity + i, v = x + 3 * fixed::capacity,
                   effort = x + 6 * fixed::capacity;
        if (i >= count_) {
          if (values[x] || values[v] || values[effort])
            return bad;
          continue;
        }
        const auto speed = std::int64_t(values[v]) + values[effort];
        const auto position = std::int64_t(values[x]) + speed;
        if (values[effort] < -4 || values[effort] > 4 || speed < -4160 ||
            speed > 4160 || position < -2165760 || position > 2165760)
          return bad;
        values[v] = static_cast<std::int32_t>(speed);
        values[x] = static_cast<std::int32_t>(position);
      }
    phase_ = 4;
    return ok;
  }

public:
  std::atomic<bool> protocol_ok{true}, fail_kernel{}, lose_query{},
      fail_query{}, fail_stream_sync{}, fail_event_sync{}, fail_unregister{},
      fail_destroy{}, corrupt_output{}, hold{};
  std::atomic<unsigned> delayed_queries{}, fail_registration_at{};
  std::atomic<bool> fail_event_create{};
  std::atomic<std::uint64_t> uploads{}, copies{}, downloads{}, kernels{},
      graphs{}, records{}, queries{}, not_ready{}, faults{}, registrations{},
      unregistrations{}, event_creates{}, event_destroys{}, stream_syncs{},
      event_syncs{}, backend_allocations{}, backend_frees{};
  explicit SimulatedDriver(std::size_t count) : count_(count) {}
  ~SimulatedDriver() {
    if (!close())
      std::terminate();
  }
  void time(std::uint64_t nominal) noexcept {
    logical_ = nominal;
    offset_ = 0;
  }
  bool clean() const noexcept {
    if (live_.load())
      return false;
    for (const auto &e : events_)
      if (e.allocated.load())
        return false;
    for (auto registered : registered_)
      if (registered)
        return false;
    return true;
  }
  bool close() noexcept {
    if (!clean())
      return false;
    closed_ = true;
    return true;
  }
  Resources resources() noexcept {
    rt::CudaDriverApi a;
    a.api_version = rt::cuda_driver_api_version_2;
    a.struct_size = rt::cuda_driver_api_v2_struct_size;
    a.user_data = this;
    a.push_context = [](void *p, rt::CudaContext c) noexcept {
      auto &s = *static_cast<SimulatedDriver *>(p);
      if (c != context || current_ || s.closed_)
        return bad;
      current_ = &s;
      return ok;
    };
    a.pop_context = [](void *p, rt::CudaContext *c) noexcept {
      auto &s = *static_cast<SimulatedDriver *>(p);
      if (!c || !s.current())
        return bad;
      *c = context;
      current_ = nullptr;
      return ok;
    };
    a.event_create = [](void *p, rt::CudaEvent *out) noexcept {
      auto &s = *static_cast<SimulatedDriver *>(p);
      if (!s.current() || !out)
        return bad;
      if (s.fail_event_create.exchange(false)) {
        ++s.faults;
        return R::out_of_memory;
      }
      for (std::size_t i = 0; i < s.events_.size(); ++i)
        if (!s.events_[i].allocated.exchange(true)) {
          s.events_[i].recorded = false;
          ++s.event_creates;
          *out = i + 1;
          return ok;
        }
      return R::out_of_memory;
    };
    a.event_destroy = [](void *p, rt::CudaEvent e) noexcept {
      auto &s = *static_cast<SimulatedDriver *>(p);
      auto *v = s.event(e);
      if (!s.current() || !v)
        return bad;
      if (s.fail_destroy.exchange(false)) {
        ++s.faults;
        return R::error;
      }
      v->recorded = false;
      v->allocated = false;
      ++s.event_destroys;
      return ok;
    };
    a.event_record = [](void *p, rt::CudaEvent e, rt::CudaStream st) noexcept {
      auto &s = *static_cast<SimulatedDriver *>(p);
      auto *v = s.event(e);
      if (!s.current() || !v || st != stream || s.phase_ != 0 ||
          s.live_.exchange(true))
        return bad;
      v->recorded = true;
      ++s.records;
      return ok;
    };
    a.event_query = [](void *p, rt::CudaEvent e) noexcept {
      auto &s = *static_cast<SimulatedDriver *>(p);
      auto *v = s.event(e);
      if (!s.current() || !v || !v->recorded.load())
        return bad;
      ++s.queries;
      if (s.lose_query.exchange(false)) {
        ++s.faults;
        return R::context_lost;
      }
      if (s.fail_query.exchange(false)) {
        ++s.faults;
        return R::error;
      }
      if (s.hold.load()) {
        ++s.not_ready;
        return R::not_ready;
      }
      if (s.delayed_queries.load()) {
        --s.delayed_queries;
        ++s.not_ready;
        return R::not_ready;
      }
      s.live_ = false;
      return ok;
    };
    a.event_synchronize = [](void *p, rt::CudaEvent e) noexcept {
      auto &s = *static_cast<SimulatedDriver *>(p);
      if (!s.current() || !s.event(e))
        return bad;
      ++s.event_syncs;
      if (s.fail_event_sync.exchange(false)) {
        ++s.faults;
        return R::error;
      }
      s.live_ = false;
      return ok;
    };
    a.stream_synchronize = [](void *p, rt::CudaStream st) noexcept {
      auto &s = *static_cast<SimulatedDriver *>(p);
      if (!s.current() || st != stream)
        return bad;
      ++s.stream_syncs;
      if (s.fail_stream_sync.exchange(false)) {
        ++s.faults;
        return R::error;
      }
      s.live_ = false;
      s.phase_ = 0;
      return ok;
    };
    a.host_register = [](void *p, void *host, std::uint64_t bytes) noexcept {
      auto &s = *static_cast<SimulatedDriver *>(p);
      if (!s.current() || !host || bytes != sizeof(State))
        return bad;
      if (s.fail_registration_at &&
          s.registrations + 1 == s.fail_registration_at) {
        s.fail_registration_at = 0;
        ++s.faults;
        return R::error;
      }
      for (std::size_t i = 0; i < 2; ++i)
        if (!s.registered_[i]) {
          s.host_[i] = host;
          s.registered_[i] = true;
          ++s.registrations;
          return ok;
        }
      return bad;
    };
    a.host_unregister = [](void *p, void *host) noexcept {
      auto &s = *static_cast<SimulatedDriver *>(p);
      if (!s.current())
        return bad;
      if (s.fail_unregister.exchange(false)) {
        ++s.faults;
        return R::error;
      }
      for (std::size_t i = 0; i < 2; ++i)
        if (s.registered_[i] && s.host_[i] == host) {
          s.registered_[i] = false;
          ++s.unregistrations;
          return ok;
        }
      return bad;
    };
    a.mem_alloc = [](void *p, std::uint64_t, rt::CudaDeviceAddress *) noexcept {
      ++static_cast<SimulatedDriver *>(p)->backend_allocations;
      return bad;
    };
    a.mem_free = [](void *p, rt::CudaDeviceAddress) noexcept {
      ++static_cast<SimulatedDriver *>(p)->backend_frees;
      return bad;
    };
    a.memcpy_host_to_device_async = [](void *p, rt::CudaDeviceAddress to,
                                       const void *from, std::uint64_t bytes,
                                       rt::CudaStream st) noexcept {
      auto &s = *static_cast<SimulatedDriver *>(p);
      const auto b = s.buffer(to, bytes);
      if (!s.current() || st != stream || b < 0 ||
          from != s.host_[static_cast<std::size_t>(b)] ||
          !s.registered_[static_cast<std::size_t>(b)] || s.live_.load() ||
          s.phase_ != static_cast<unsigned>(b))
        return bad;
      std::memcpy(s.storage_->values[static_cast<std::size_t>(b)].data(), from,
                  sizeof(State));
      ++s.phase_;
      ++s.uploads;
      return ok;
    };
    a.memcpy_device_to_device_async =
        [](void *p, rt::CudaDeviceAddress to, rt::CudaDeviceAddress from,
           std::uint64_t bytes, rt::CudaStream st) noexcept {
          auto &s = *static_cast<SimulatedDriver *>(p);
          if (!s.current() || st != stream || s.buffer(from, bytes) != 0 ||
              s.buffer(to, bytes) != 1 || s.phase_ != 2)
            return bad;
          s.storage_->values[1] = s.storage_->values[0];
          ++s.phase_;
          ++s.copies;
          return ok;
        };
    a.memcpy_device_to_host_async =
        [](void *p, void *to, rt::CudaDeviceAddress from, std::uint64_t bytes,
           rt::CudaStream st) noexcept {
          auto &s = *static_cast<SimulatedDriver *>(p);
          if (!s.current() || st != stream || s.buffer(from, bytes) != 1 ||
              to != s.host_[1] || !s.registered_[1] || s.phase_ != 4)
            return bad;
          std::memcpy(to, s.storage_->values[1].data(), sizeof(State));
          if (s.corrupt_output.exchange(false)) {
            static_cast<std::int32_t *>(to)[0] ^= 1;
            ++s.faults;
          }
          s.phase_ = 0;
          ++s.downloads;
          return ok;
        };
    a.memset_d8_async = [](void *, rt::CudaDeviceAddress, std::uint8_t,
                           std::uint64_t,
                           rt::CudaStream) noexcept { return bad; };
    a.launch_kernel = [](void *p, rt::CudaFunction f, std::uint32_t gx,
                         std::uint32_t gy, std::uint32_t gz, std::uint32_t bx,
                         std::uint32_t by, std::uint32_t bz,
                         std::uint32_t shared, rt::CudaStream st,
                         void *const *arguments) noexcept {
      auto &s = *static_cast<SimulatedDriver *>(p);
      if (f != function || gx != (s.count_ + 127) / 128 || gy != 1 || gz != 1 ||
          bx != 128 || by != 1 || bz != 1 || shared || !arguments ||
          !arguments[0] || !arguments[1])
        return bad;
      rt::CudaDeviceAddress address{};
      std::uint32_t count{};
      std::memcpy(&address, arguments[0], sizeof(address));
      std::memcpy(&count, arguments[1], sizeof(count));
      if (address != s.address(1) || count != s.count_)
        return bad;
      if (s.fail_kernel.exchange(false)) {
        ++s.faults;
        return R::launch_failure;
      }
      const auto result = s.integrate(st);
      if (result == ok)
        ++s.kernels;
      return result;
    };
    a.graph_launch = [](void *p, rt::CudaGraphExec g,
                        rt::CudaStream st) noexcept {
      auto &s = *static_cast<SimulatedDriver *>(p);
      if (g != graph)
        return bad;
      if (s.fail_kernel.exchange(false)) {
        ++s.faults;
        return R::launch_failure;
      }
      const auto result = s.integrate(st);
      if (result == ok)
        ++s.graphs;
      return result;
    };
    a.monotonic_time_ns = [](void *p) noexcept -> std::uint64_t {
      auto &s = *static_cast<SimulatedDriver *>(p);
      return s.logical_.load() + (s.hold.load()
                                      ? s.offset_.fetch_add(completion_ns / 4)
                                      : s.offset_.load());
    };
    return {a, context, stream, function, graph, {address(0), address(1)}};
  }
};
} // namespace golden::cuda
