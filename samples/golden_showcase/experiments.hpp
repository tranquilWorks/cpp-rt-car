#pragma once
#include "../golden_system/session.hpp"
#include <string>
namespace golden::showcase {
struct Experiment {
  std::string lever, scope;
  std::uint64_t requested = 0, effective = 0, runtime_id = 0, config_id = 0,
                operations = 0, checks = 0, accepted = 0, rejected = 0,
                replaced = 0, gaps = 0, bytes = 0, scratch = 0, capacity = 0,
                calls = 0, checksum = 0;
  std::int32_t status = 0;
  StateBytes state{};
  bool has_state = false, cleanup = false, correct = false;
};
bool finite_value(std::string_view, std::uint64_t) noexcept;
bool experiment(Experiment&);
} // namespace golden::showcase
