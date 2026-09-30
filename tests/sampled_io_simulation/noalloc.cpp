#include "fixture.hpp"
#include <algorithm>
#include <cstddef>
// The preserved guard requires standard alignment declarations first.
#include "../cuda_physics/allocation_guard.hpp"
int main() {
  using namespace rtfw_physics_allocation;
  begin();
  auto *positive = ::operator new(17);
  *static_cast<volatile unsigned char *>(positive) = 17;
  ::operator delete(positive);
  const bool detected = count.load() != 0;
  tracking = false;
  sampled_simulation::Fixture f;
  if (!detected || f.configure() != rt::Status::ok ||
      f.runtime.finalize() != rt::Status::ok)
    return 1;
  begin();
  bool good = f.runtime.start() == rt::Status::ok;
  // Startup's existing host thread allocation is outside the declared RT lane.
  // Record it for the unchanged-baseline comparison, then measure steady work
  // and actual acknowledged stop, including all native worker threads.
  const auto startup_allocations = count.load();
  begin();
  for (unsigned i = 0; good && i < 16; ++i)
    good = f.step(i) == rt::Status::ok && f.valid(i);
  good = f.runtime.stop() == rt::Status::ok && good;
  good = f.runtime.stop() == rt::Status::ok && good;
  tracking = false;
  const auto allocations = count.load();
  std::printf("sampled simulation steady/stop allocations=%zu "
              "positive_control=%d result=%d startup_allocations=%zu\n",
              allocations, detected, good, startup_allocations);
  return good && allocations == 0 && !f.backend.card.live ? 0 : 2;
}
