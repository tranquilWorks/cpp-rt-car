#include "../device_rate_simulation/fixture.hpp"
#include <type_traits>
static_assert(std::is_aggregate_v<rt::DeviceRatePhaseBinding>);
int main() {
  simulation_test::Fixture f;
  if (f.configure() != rt::Status::ok)
    return 1;
  auto old_style = f.binding();
  if (old_style.simulation)
    return 2;
  old_style.simulation = rt::DeviceRateSimulationPolicy{};
  if (f.runtime.bind_device_phase_to_rate_domain(old_style) !=
      rt::Status::invalid_argument)
    return 3;
  old_style.simulation =
      rt::DeviceRateSimulationPolicy{simulation_test::watchdog};
  if (f.runtime.bind_device_phase_to_rate_domain(old_style) != rt::Status::ok ||
      f.runtime.finalize() != rt::Status::ok)
    return 4;
  rt::CompiledDeviceRatePhase phase;
  if (!f.runtime.compiled_device_rate_phase_at(0, phase) || !phase.simulation ||
      phase.simulation->host_watchdog_ns != simulation_test::watchdog ||
      phase.completion_budget_ns != simulation_test::budget)
    return 5;
  if (f.runtime.start() != rt::Status::ok || f.step() != rt::Status::ok ||
      f.publications != 1 || f.completed() != 1)
    return 6;
  return f.runtime.stop() == rt::Status::ok ? 0 : 7;
}
