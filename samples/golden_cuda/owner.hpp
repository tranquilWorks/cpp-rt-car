#pragma once
#include "../golden_system/session.hpp"
#include "physics.hpp"

namespace golden::cuda {
enum class Dispatch { cpu, kernel, graph };

// The owner keeps borrowed memory, jobs, staging and injected driver resources
// alive through checked Runtime teardown. A recovered run constructs a new
// owner. Real Driver API resources, when supplied, remain owned by the caller.
struct Owner {
  Options options;
  Dispatch dispatch;
  std::unique_ptr<Memory> memory = std::make_unique<Memory>();
  std::unique_ptr<Jobs> jobs = std::make_unique<Jobs>();
  std::unique_ptr<SimulatedDriver> driver;
  std::unique_ptr<CudaPhysics> physics;
  std::unique_ptr<Session> session;
  bool jobs_started = false;
  Owner(Options o, Dispatch d, const Resources *native = nullptr)
      : options(o), dispatch(d) {
    if (d != Dispatch::cpu) {
      if (native)
        physics = std::make_unique<CudaPhysics>(*native, d == Dispatch::graph);
      else {
        driver = std::make_unique<SimulatedDriver>(o.count);
        physics = std::make_unique<CudaPhysics>(*driver, d == Dispatch::graph);
      }
    }
    session = std::make_unique<Session>(o, o.host ? jobs.get() : nullptr,
                                        *memory, physics.get());
  }
  Owner(const Owner &) = delete;
  Owner &operator=(const Owner &) = delete;
  ~Owner() {
    if (close() != rt::Status::ok)
      std::terminate();
  }
  rt::Status prepare(const rt::CpuMemoryPolicy *cpu_policy = nullptr) noexcept {
    if (!session)
      return rt::Status::invalid_state;
    if (options.host && !jobs_started) {
      const auto status = jobs->start(options.workers);
      if (status != rt::Status::ok)
        return status;
      jobs_started = true;
    }
    return session->prepare(fixed::action_capacity, cpu_policy);
  }
  rt::Status close() noexcept {
    if (session) {
      const auto status = session->close();
      if (status != rt::Status::ok)
        return status;
      session.reset();
    }
    physics.reset();
    if (driver && !driver->close())
      return rt::Status::device_error;
    if (jobs_started) {
      const auto status = jobs->close();
      if (status != rt::Status::ok)
        return status;
      jobs_started = false;
    }
    return memory->live_count() == 0 && !memory->violations
               ? rt::Status::ok
               : rt::Status::internal_error;
  }
};
} // namespace golden::cuda
