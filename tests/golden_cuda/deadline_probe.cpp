// Diagnostic only: a nonzero exit is retained evidence, never an accepted gate.
#include "../../samples/golden_cuda/physics.hpp"
#include "../../samples/golden_system/oracle.hpp"
#include "../../samples/golden_system/replay.hpp"
#include <iostream>
#include <string_view>

namespace {
class ProbePhysics final : public golden::Physics {
  golden::cuda::CudaPhysics implementation_;
  bool park_;

public:
  ProbePhysics(golden::cuda::SimulatedDriver &driver, bool park)
      : implementation_(driver, true), park_(park) {}
  void limits(rt::RuntimeConfig &c) const noexcept override {
    implementation_.limits(c);
  }
  bool replay_enabled() const noexcept override { return true; }
  rt::Status configure(rt::Runtime &r, golden::World &w) noexcept override {
    if (park_) {
      auto policy = golden::Memory::policy();
      policy.thread_policy_count = 1;
      policy.thread_policies[0].role = rt::thread_role_executor_worker;
      policy.thread_policies[0].policy.wait_strategy = rt::WaitStrategy::park;
      const auto status = r.set_cpu_memory_policy(policy);
      if (status != rt::Status::ok)
        return status;
    }
    return implementation_.configure(r, w);
  }
  rt::Status register_phase(rt::Runtime &r, rt::RateDomainHandle rate,
                            rt::PhaseHandle &phase) noexcept override {
    return implementation_.register_phase(r, rate, phase);
  }
  bool input(const rt::CallbackContext &c) noexcept override {
    return implementation_.input(c);
  }
  bool complete(const rt::CallbackContext &c) noexcept override {
    return implementation_.complete(c);
  }
  bool plan(const rt::MemoryPlan &p) const noexcept override {
    return implementation_.plan(p);
  }
};
} // namespace

int main(int argc, char **argv) {
  const bool park = argc == 2 && std::string_view(argv[1]) == "--park";
  if (argc != 1 && !park)
    return 64;
  for (unsigned attempt = 0; attempt < 100; ++attempt) {
    golden::Options options;
    auto memory = std::make_unique<golden::Memory>();
    golden::cuda::SimulatedDriver driver(options.count);
    ProbePhysics physics(driver, park);
    golden::Session session(options, nullptr, *memory, &physics);
    const auto prepared = session.prepare();
    if (prepared != rt::Status::ok) {
      std::cerr << "prepare=" << int(prepared) << ' '
                << session.runtime->last_error() << '\n';
      return 1;
    }
    golden::Replay replay(options.ticks);
    golden::Oracle oracle(options);
    if (!replay.begin(session) || !session.controls())
      return 2;
    for (std::size_t tick = 0; tick < options.ticks; ++tick) {
      replay.record(session, tick);
      const auto status = session.step(tick);
      if (status != rt::Status::ok || !oracle.step(tick, session.world)) {
        std::cerr << "attempt=" << attempt << " tick=" << tick
                  << " status=" << int(status) << ' '
                  << session.runtime->last_error() << " logical_ns="
                  << session.clock.now.load() << " physics_calls="
                  << session.world.calls[1] << " stage_calls="
                  << session.world.calls[2] << '\n';
        return 3;
      }
    }
    if (!replay.seal(session) || !replay.verify(session)) {
      std::cerr << "attempt=" << attempt << " replay failed: "
                << session.runtime->last_error() << '\n';
      return 4;
    }
    if (session.close() != rt::Status::ok || !driver.clean())
      return 5;
    std::cout << "attempt=" << attempt << " PASS graph_operations="
              << driver.graphs << std::endl;
  }
}
