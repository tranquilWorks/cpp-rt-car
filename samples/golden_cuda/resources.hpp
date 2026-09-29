#pragma once
#include "../golden_system/physics.hpp"
#include <rt/cuda_backend.hpp>
#include <atomic>
#include <cstring>
#include <memory>
namespace golden::cuda {
using State = std::array<std::int32_t, 9 * fixed::capacity>;
struct alignas(64) Storage { std::array<State, 2> values{}; };
static_assert(sizeof(Storage) == 2 * sizeof(Plant));
struct Resources {
  rt::CudaDriverApi driver{};
  rt::CudaContext context{};
  rt::CudaStream stream{};
  rt::CudaFunction function{};
  rt::CudaGraphExec graph{};
  std::array<rt::CudaDeviceAddress, 2> addresses{};
};
inline constexpr std::uint64_t completion_ns = fixed::budgets[0] / 3;
inline constexpr std::array<std::string_view, 2> buffer_names{"golden.cuda.input", "golden.cuda.output"};
} // namespace golden::cuda
