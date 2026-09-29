#include <algorithm>
#include <cstddef>
#include "../cuda_physics/allocation_guard.hpp"
#include "fixture.hpp"
#include <cstdio>
int main() {
  using namespace rtfw_physics_allocation;
  begin();
  auto *control = ::operator new(17);
  ::operator delete(control);
  const bool detected = count.load() != 0;
  tracking = false;
  if (!detected)
    return 1;
  sampled_storage::Fixture f;
  if (f.configure() != rt::Status::ok || f.start() != rt::Status::ok)
    return 2;
  const auto initial = f.checkpoint();
  for (unsigned i = 0; i < 32; ++i)
    if (f.step(i) != rt::Status::ok || !f.valid(i))
      return 3;
  const auto expected = f.state;
  const auto artifact = f.artifact(initial, 32);
  if (artifact.empty())
    return 4;
  begin();
  bool good =
      f.runtime.replay_active(artifact, decltype(f)::apply) == rt::Status::ok &&
      f.state == expected;
  for (unsigned i = 32; good && i < 64; ++i)
    good = f.step(i) == rt::Status::ok && f.valid(i);
  tracking = false;
  const auto allocations = count.load();
  good = f.runtime.stop() == rt::Status::ok && good;
  std::printf(
      "four-slot steady/replay allocations=%zu positive_control=%d result=%d\n",
      allocations, detected, good);
  return good && allocations == 0 ? 0 : 5;
}
