#include "variants.hpp"
#include "../golden_cuda/runner.hpp"
#include "../golden_xdma/runner.hpp"
#include <charconv>
namespace golden::showcase {
namespace {
// Only bridge freshly produced scalar fields from the unchanged runners.
// Independent Python validation checks the complete JSON/state/transcripts.
std::uint64_t field(std::string_view json,std::string_view key) {
  std::string token("\"");
  token.append(key);
  token.append("\":");
  const auto at = json.find(token);
  if (at == std::string_view::npos || json.find(token,at + token.size()) != std::string_view::npos)
    throw std::runtime_error("missing or duplicate runner field");
  const auto begin = json.data() + at + token.size();
  std::uint64_t value = 0;
  const auto parsed = std::from_chars(begin,json.data() + json.size(),value);
  if (parsed.ec != std::errc{} || parsed.ptr == json.data() + json.size() ||
      (*parsed.ptr != ',' && *parsed.ptr != '}')) throw std::runtime_error("invalid runner scalar");
  return value;
}
}
bool variant_run(std::string_view variant,std::string_view dispatch,Options options,
                 const std::filesystem::path &output,Invocation &v) {
  if (output.empty() || options.external) return false;
  int status = 1;
  if (variant == "sim_cuda" && (dispatch == "kernel" || dispatch == "graph")) {
    cuda::Arguments a; a.scenario = options; a.dispatch = dispatch; a.output = output;
    status = cuda::execute(a);
  } else if ((variant == "sim_xdma" && dispatch == "cpu") ||
             (variant == "sim_combined" && (dispatch == "kernel" || dispatch == "graph"))) {
    xdma::Arguments a; a.scenario = options; a.dispatch = dispatch; a.output = output;
    status = xdma::execute(a);
  } else return false;
  if (status) return false;
  std::ifstream json_file(output / "execution.json");
  const std::string json{std::istreambuf_iterator<char>(json_file),{}};
  std::ifstream state_file(output / "state.bin",std::ios::binary);
  const std::string state{std::istreambuf_iterator<char>(state_file),{}};
  if (state.size() != state_bytes) return false;
  v.checksum = digest(std::as_bytes(std::span(state)));
  v.operations = options.ticks;
  for (auto period : std::array<std::size_t,8>{1,1,1,2,3,3,6,6})
    v.phase_calls += (options.ticks + period - 1) / period;
  v.entity_checks = options.ticks * 3 * fixed::capacity;
  v.runtime_id = field(json,"runtime_id"); v.planned_bytes = field(json,"runtime_planned_bytes");
  v.trace_events = field(json,"trace_events");
  v.actions = field(json,"rate_actions") + field(json,"mixed_actions") + field(json,"control_actions");
  v.replay_frames = field(json,"replay_frames");
  v.accepted = field(json,"control_accepted"); v.replaced = field(json,"control_replaced");
  v.jobs = field(json,"jobs_accepted"); v.memory_acquired = field(json,"memory_acquired");
  v.memory_released = field(json,"memory_released");
  v.gaps = field(json,"trace_lost") + field(json,"action_gaps");
  return v.replay_frames == options.ticks && !v.gaps && v.memory_acquired == v.memory_released;
}
}
