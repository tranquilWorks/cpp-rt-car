#pragma once
#include "../golden_cuda/physics.hpp"
#include "session.hpp"
namespace golden::xdma {
enum class Dispatch { cpu, kernel, graph };
// All borrowed storage and both native worker drivers outlive checked Runtime
// stop. A failed stop leaves this complete owner available for bounded retry.
struct Owner {
  Options options;
  Dispatch dispatch;
  std::unique_ptr<Memory> memory = std::make_unique<Memory>();
  std::unique_ptr<Jobs> jobs = std::make_unique<Jobs>();
  std::unique_ptr<cuda::SimulatedDriver> cuda_driver;
  std::unique_ptr<cuda::CudaPhysics> physics;
  std::unique_ptr<SimulatedDriver> driver;
  std::unique_ptr<Io> io;
  std::unique_ptr<Session> session;
  bool jobs_started = false;
  Owner(Options o, Dispatch d = Dispatch::cpu) : options(o), dispatch(d) {
    if (d != Dispatch::cpu) {
      cuda_driver = std::make_unique<cuda::SimulatedDriver>(o.count);
      physics = std::make_unique<cuda::CudaPhysics>(*cuda_driver,
                                                    d == Dispatch::graph);
    }
    driver = std::make_unique<SimulatedDriver>(o.count);
    io = std::make_unique<Io>(*driver);
    session = std::make_unique<Session>(o, o.host ? jobs.get() : nullptr,
                                        *memory, *io, physics.get());
  }
  Owner(const Owner &) = delete;
  Owner &operator=(const Owner &) = delete;
  ~Owner() {
    if (close() != rt::Status::ok)
      std::terminate();
  }
  rt::Status prepare() noexcept {
    if (!session)
      return rt::Status::invalid_state;
    if (options.host && !jobs_started) {
      const auto s = jobs->start(options.workers);
      if (s != rt::Status::ok)
        return s;
      jobs_started = true;
    }
    return session->prepare();
  }
  rt::Status close() noexcept {
    if (session) {
      const auto s = session->close();
      if (s != rt::Status::ok)
        return s;
      session.reset();
    }
    io.reset();
    physics.reset();
    if (cuda_driver && !cuda_driver->close())
      return rt::Status::device_error;
    if (jobs_started) {
      const auto s = jobs->close();
      if (s != rt::Status::ok)
        return s;
      jobs_started = false;
    }
    return !driver->live && memory->live_count() == 0 && !memory->violations &&
                   jobs->accepted == jobs->completed
               ? rt::Status::ok
               : rt::Status::internal_error;
  }
};
} // namespace golden::xdma
