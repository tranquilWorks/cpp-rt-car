#include <algorithm>
#include <cstddef>

#include "../cuda_physics/allocation_guard.hpp"
#include "fixture.hpp"
#include <iostream>
int main() {
  using namespace simulation_test;
  Fixture f;
  if (f.configure(true) != rt::Status::ok)
    return 1;
  auto b = f.binding();
  b.simulation = rt::DeviceRateSimulationPolicy{watchdog};
  if (f.runtime.bind_device_phase_to_rate_domain(b) != rt::Status::ok ||
      f.runtime.finalize() != rt::Status::ok ||
      f.runtime.start() != rt::Status::ok)
    return 2;
  std::size_t n = 0;
  rt::ArtifactWriteResult write;
  if (f.runtime.checkpoint_size(n) != rt::Status::ok)
    return 3;
  std::vector<std::byte> initial(n), artifact(65536);
  if (f.runtime.write_checkpoint(0, initial, write) != rt::Status::ok)
    return 4;
  std::array<rt::ReplayInputRecord, 4> inputs;
  for (unsigned i = 0; i < inputs.size(); ++i) {
    inputs[i] = {f.frame(i), 1, {}};
    if (f.step(i) != rt::Status::ok)
      return 5;
  }
  if (f.runtime.write_active_replay_artifact(initial, inputs, artifact,
                                             write) != rt::Status::ok)
    return 6;
  artifact.resize(write.bytes_written);
  const auto expected = f.state;
  rtfw_physics_allocation::count = 0;
  rtfw_physics_allocation::tracking = true;
  const auto replay = f.runtime.replay_active(
      artifact,
      [](void *, const rt::ReplayInputView &) {
        return rt::CallbackResult::ok;
      },
      nullptr);
  bool good = replay == rt::Status::ok && f.state == expected;
  for (unsigned i = 4; good && i < 36; ++i)
    good = f.step(i) == rt::Status::ok;
  rtfw_physics_allocation::tracking = false;
  const auto allocations = rtfw_physics_allocation::count.load();
  good =
      good && allocations == 0 && f.completed() == 40 && f.publications == 40;
  good = f.runtime.stop() == rt::Status::ok && good;
  std::cout << "simulation allocations=" << allocations << " result=" << good
            << '\n';
  return good ? 0 : 7;
}
