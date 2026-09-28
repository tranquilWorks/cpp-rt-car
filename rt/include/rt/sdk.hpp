#pragma once

// Optional source helpers. No compiled ABI, owning callback wrapper or registry.
#include <rt/runtime.hpp>

#include <array>
#include <charconv>
#include <concepts>
#include <exception>
#include <functional>
#include <limits>
#include <memory>
#include <span>
#include <string_view>
#include <type_traits>

namespace rt::sdk {

template <auto Function, typename State>
concept Callback =
    (std::is_member_function_pointer_v<decltype(Function)> ||
     (std::is_pointer_v<decltype(Function)> && std::is_function_v<std::remove_pointer_t<decltype(Function)>>)) &&
    !std::is_same_v<std::integral_constant<decltype(Function), Function>,
                    std::integral_constant<decltype(Function), nullptr>> &&
    !std::is_const_v<State> && !std::is_volatile_v<State> &&
    requires(State& state, const CallbackContext& context) {
    { std::invoke(Function, state, context) } noexcept -> std::same_as<CallbackResult>;
};

// State must remain at the same address until checked stop; temporaries cannot bind.
// Both free functions and member functions are supported. No callable is stored.
template <auto Function, typename State> requires Callback<Function, State>
[[nodiscard]] CallbackRegistration callback(std::string_view name, State& state) noexcept {
    return {name, [](void* user, const CallbackContext& context) noexcept {
        return std::invoke(Function, *static_cast<State*>(user), context);
    }, std::addressof(state)};
}

class ConfigBuilder final {
public:
    explicit ConfigBuilder(RuntimeConfig config = {}) noexcept : config_(config) {}
    [[nodiscard]] const RuntimeConfig& value() const noexcept { return config_; }
    // The public parser preserves its transactional, strict-key semantics.
    [[nodiscard]] Status set(std::string_view key, std::string_view value) noexcept {
        return set_runtime_config_value(config_, key, value);
    }
    // Full validation belongs to Runtime, including already-registered capacities.
    [[nodiscard]] Status apply(Runtime& runtime) const noexcept { return runtime.configure(config_); }
private:
    RuntimeConfig config_;
};

// A borrowed configuration-time view. Each call is checked separately; successful
// prior declarations are not rolled back if a later call fails. No auto-finalize.
class GraphBuilder final {
public:
    explicit GraphBuilder(Runtime& runtime) noexcept : runtime_(runtime) {}
    template <auto Function, typename State> requires Callback<Function, State>
    [[nodiscard]] Status phase(std::string_view name, State& state, PhaseHandle& out) noexcept {
        return runtime_.register_callback(callback<Function>(name, state), out);
    }
    [[nodiscard]] Status resource(std::string_view name, ResourceHandle& out) noexcept {
        return runtime_.register_resource(name, out);
    }
    [[nodiscard]] Status depends_on(PhaseHandle dependent, PhaseHandle prerequisite) noexcept {
        return runtime_.add_dependency(prerequisite, dependent);
    }
    [[nodiscard]] Status access(PhaseHandle phase, ResourceHandle resource, ResourceAccess mode) noexcept {
        return runtime_.declare_resource_access(phase, resource, mode);
    }
    [[nodiscard]] Status finalize() noexcept { return runtime_.finalize(); }
private:
    Runtime& runtime_;
};

struct AlignedCapacity {
    std::size_t stride = 0;
    std::size_t total_bytes = 0;
};

// Pure arithmetic, not Runtime configuration validation. Output is unchanged on
// failure. Zero bytes/count are valid; alignment must be a nonzero power of two.
[[nodiscard]] constexpr Status aligned_capacity(std::size_t bytes, std::size_t alignment,
                                                std::size_t count, AlignedCapacity& out) noexcept {
    if (alignment == 0 || (alignment & (alignment - 1)) != 0) return Status::invalid_argument;
    constexpr auto maximum = std::numeric_limits<std::size_t>::max();
    if (bytes > maximum - (alignment - 1)) return Status::capacity_exceeded;
    const auto stride = (bytes + alignment - 1) & ~(alignment - 1);
    if (count != 0 && stride > maximum / count) return Status::capacity_exceeded;
    out = {stride, stride * count};
    return Status::ok;
}

struct NativeStorageEstimate {
    AlignedCapacity phase_scratch{};
    AlignedCapacity task_scratch{};
    std::size_t queue_slots = 0;
};

// Partial storage arithmetic only: excludes trace and all control bytes. Always
// configure/finalize and inspect MemoryPlan for the authoritative budget result.
// Host-adapter queues have different ownership and are deliberately unsupported.
[[nodiscard]] constexpr Status estimate_native_storage(const RuntimeConfig& config,
    std::size_t phases, NativeStorageEstimate& out) noexcept {
    if (config.executor_policy != ExecutorPolicy::static_deterministic &&
        config.executor_policy != ExecutorPolicy::bounded_throughput) return Status::invalid_config;
    if (phases > config.callback_capacity || phases > config.task_scratch_slots)
        return Status::capacity_exceeded;
    NativeStorageEstimate candidate;
    auto status = aligned_capacity(config.scratch_bytes, config.scratch_alignment, phases,
                                   candidate.phase_scratch);
    if (status != Status::ok) return status;
    status = aligned_capacity(config.task_scratch_bytes, config.scratch_alignment,
                              config.task_scratch_slots, candidate.task_scratch);
    if (status != Status::ok) return status;
    if (config.worker_count == 0 || config.executor_queue_capacity == 0) return Status::invalid_config;
    if (config.executor_queue_capacity > std::numeric_limits<std::size_t>::max() / config.worker_count)
        return Status::capacity_exceeded;
    candidate.queue_slots = config.worker_count * config.executor_queue_capacity;
    out = candidate;
    return Status::ok;
}

// Construct after successful finalize, before start. Runtime and every borrowed
// owner must outlive this guard and remain at the same address. Control-thread use
// only. Prefer explicit close(); a failed close keeps the guard armed for retry.
// Destruction attempts stop once, then terminates on failure instead of permitting
// borrowed owners to be destroyed while Runtime still retains them.
class CheckedStopGuard final {
public:
    explicit CheckedStopGuard(Runtime& runtime) noexcept : runtime_(runtime) {}
    CheckedStopGuard(const CheckedStopGuard&) = delete;
    CheckedStopGuard& operator=(const CheckedStopGuard&) = delete;
    CheckedStopGuard(CheckedStopGuard&&) = delete;
    CheckedStopGuard& operator=(CheckedStopGuard&&) = delete;
    ~CheckedStopGuard() noexcept {
        if (close() != Status::ok) std::terminate();
    }
    [[nodiscard]] Status close() noexcept {
        if (!armed_) return Status::ok;
        const auto result = runtime_.stop();
        if (result == Status::ok) armed_ = false;
        return result;
    }
    [[nodiscard]] bool armed() const noexcept { return armed_; }
private:
    Runtime& runtime_;
    bool armed_ = true;
};

[[nodiscard]] constexpr std::string_view recovery_hint(Status status) noexcept {
    switch (status) {
    case Status::ok: return "no action required";
    case Status::invalid_argument: return "check names, arguments and enum values";
    case Status::invalid_state: return "check configure/finalize/start/step/stop order";
    case Status::invalid_config: return "check explicit configuration limits and supported policies";
    case Status::capacity_exceeded: return "check declared capacities before finalization; do not resize a running graph";
    case Status::callback_failed: return "inspect callback result and application state before another frame";
    case Status::resource_exhausted: return "inspect finalized memory budget and provider availability";
    case Status::invalid_handle: return "use a current handle from this Runtime and of the correct kind";
    case Status::graph_cycle: return "rebuild the configuring graph without the dependency cycle";
    case Status::resource_conflict: return "order conflicting resource accesses with a dependency before finalization";
    case Status::queue_full: return "bound submissions to the declared queue capacity";
    case Status::scratch_exhausted: return "check task scratch bytes and simultaneous slot demand";
    case Status::platform_preflight_failed: return "inspect the preflight report for this host";
    case Status::clock_failure: return "inspect the host clock and timestamp contract";
    case Status::invalid_artifact: return "validate artifact extent, format and checksum";
    case Status::incompatible_artifact: return "use an artifact matching the finalized graph and configuration";
    case Status::incompatible_abi: return "check the requested ABI version and complete table sizes";
    case Status::device_queue_full: return "bound outstanding device work to admitted capacity";
    case Status::device_timeout:
    case Status::device_error:
    case Status::device_lost:
    case Status::device_canceled:
    case Status::device_reset_required: return "inspect device health; retain borrowed owners until checked stop succeeds";
    case Status::internal_error: return "retain diagnostics and stop safely before investigating the failure";
    default: return "unknown status; retain its numeric value and inspect the producer contract";
    }
}

struct DiagnosticResult {
    std::size_t required_bytes = 0; // Includes the terminating NUL, even for empty output.
    std::size_t written_bytes = 0;  // Excludes NUL.
    bool truncated = false;
};

// Inputs must not overlap output. Render immediately: Runtime::last_error() is a
// borrowed view that can change on the next Runtime operation. No allocation/I/O.
[[nodiscard]] inline DiagnosticResult render_error(std::span<char> output, std::string_view operation,
    Status status, std::string_view detail = {}) noexcept {
    DiagnosticResult result;
    auto append = [&](std::string_view text) noexcept {
        for (char character : text) {
            if (!output.empty() && result.written_bytes < output.size() - 1)
                output[result.written_bytes++] = character;
            ++result.required_bytes;
        }
    };
    std::array<char, 12> number{};
    const auto converted = std::to_chars(number.data(), number.data() + number.size(),
                                         static_cast<std::int32_t>(status));
    append(operation); append(": "); append(status_message(status)); append(" (");
    append({number.data(), static_cast<std::size_t>(converted.ptr - number.data())}); append(")");
    if (!detail.empty()) { append(": "); append(detail); }
    append("; "); append(recovery_hint(status));
    ++result.required_bytes;
    if (!output.empty()) output[result.written_bytes] = '\0';
    result.truncated = result.required_bytes > output.size();
    return result;
}

} // namespace rt::sdk
