#include <rt/sdk.hpp>
#include <rt/mock_device.hpp>
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <thread>
#include "../cuda_physics/allocation_guard.hpp"

namespace sdk = rt::sdk;
#define CHECK(x) do { if (!(x)) { std::cerr << __func__ << ':' << __LINE__ << " failed: " #x << '\n'; return false; } } while (false)
struct State {
    int produced = 0, consumed = 0;
    rt::CallbackResult produce(const rt::CallbackContext&) noexcept { ++produced; return rt::CallbackResult::ok; }
    rt::CallbackResult consume(const rt::CallbackContext&) noexcept {
        if (produced != consumed + 1) return rt::CallbackResult::error;
        consumed = produced; return rt::CallbackResult::ok;
    }
    rt::CallbackResult throwing(const rt::CallbackContext&) { return rt::CallbackResult::ok; }
    bool wrong(const rt::CallbackContext&) noexcept { return true; }
};
rt::CallbackResult free_callback(State& state, const rt::CallbackContext& context) noexcept { return state.produce(context); }
rt::CallbackResult raw_produce(void* user, const rt::CallbackContext& context) noexcept { return static_cast<State*>(user)->produce(context); }
rt::CallbackResult raw_consume(void* user, const rt::CallbackContext& context) noexcept { return static_cast<State*>(user)->consume(context); }
rt::CallbackResult fail_callback(State&, const rt::CallbackContext&) noexcept { return rt::CallbackResult::error; }
static_assert(sdk::Callback<&State::produce, State> && sdk::Callback<&free_callback, State>);
static_assert(!sdk::Callback<&State::throwing, State> && !sdk::Callback<&State::wrong, State>);
static_assert(!sdk::Callback<&State::produce, const State>);
static_assert(!sdk::Callback<static_cast<decltype(&free_callback)>(nullptr), State>);
static_assert(!sdk::Callback<static_cast<decltype(&State::produce)>(nullptr), State>);
static_assert(!std::is_copy_constructible_v<sdk::CheckedStopGuard> && !std::is_move_constructible_v<sdk::CheckedStopGuard>);
template<typename T> concept AcceptsTemporary = requires { sdk::callback<&State::produce>("temporary", T{}); };
static_assert(!AcceptsTemporary<State>);

rt::RuntimeConfig config() {
    rt::RuntimeConfig c;
    c.callback_capacity = 2; c.worker_count = 2; c.executor_queue_capacity = 4;
    c.scratch_bytes = 17; c.task_scratch_bytes = 65; c.task_scratch_slots = 3; c.trace_capacity = 16;
    return c;
}

bool prepare(rt::Runtime& runtime, State& state, bool typed) {
    const auto c = config();
    sdk::ConfigBuilder builder(c);
    CHECK((typed ? builder.apply(runtime) : runtime.configure(c)) == rt::Status::ok);
    rt::PhaseHandle producer, consumer;
    rt::ResourceHandle resource;
    sdk::GraphBuilder graph(runtime);
    if (typed) {
        CHECK(graph.phase<&free_callback>("produce", state, producer) == rt::Status::ok);
        CHECK(graph.phase<&State::consume>("consume", state, consumer) == rt::Status::ok);
        CHECK(graph.resource("state", resource) == rt::Status::ok);
        CHECK(graph.depends_on(consumer, producer) == rt::Status::ok);
        CHECK(graph.access(producer, resource, rt::ResourceAccess::write) == rt::Status::ok);
        CHECK(graph.access(consumer, resource, rt::ResourceAccess::write) == rt::Status::ok);
        CHECK(graph.finalize() == rt::Status::ok);
    } else {
        CHECK(runtime.register_callback({"produce", raw_produce, &state}, producer) == rt::Status::ok);
        CHECK(runtime.register_callback({"consume", raw_consume, &state}, consumer) == rt::Status::ok);
        CHECK(runtime.register_resource("state", resource) == rt::Status::ok);
        CHECK(runtime.add_dependency(producer, consumer) == rt::Status::ok);
        CHECK(runtime.declare_resource_access(producer, resource, rt::ResourceAccess::write) == rt::Status::ok);
        CHECK(runtime.declare_resource_access(consumer, resource, rt::ResourceAccess::write) == rt::Status::ok);
        CHECK(runtime.finalize() == rt::Status::ok);
    }
    CHECK(runtime.dependency_count() == 1 && runtime.resource_count() == 1 && runtime.resource_access_count() == 2);
    rt::PhaseHandle first, second;
    CHECK(runtime.compiled_phase_at(0, first) && runtime.compiled_phase_at(1, second));
    CHECK(first == producer && second == consumer && !runtime.compiled_phase_at(2, first));
    return true;
}

bool parity_and_allocation() {
    State a, b; rt::Runtime raw, typed;
    CHECK(prepare(raw, a, false) && prepare(typed, b, true));
    sdk::CheckedStopGuard guard_raw(raw), guard_typed(typed);
    rt::MemoryPlan x{}, y{};
    CHECK(raw.memory_plan(x) && typed.memory_plan(y));
    // Compare every public plan field without reading indeterminate struct padding.
#define PLAN_FIELD(field) CHECK(x.field == y.field)
#include "plan_fields.inc"
#undef PLAN_FIELD
    sdk::NativeStorageEstimate estimate;
    CHECK(sdk::estimate_native_storage(config(), 2, estimate) == rt::Status::ok);
    CHECK(estimate.phase_scratch.stride == 64 && estimate.phase_scratch.total_bytes == 128);
    CHECK(estimate.task_scratch.stride == 128 && estimate.task_scratch.total_bytes == 384);
    CHECK(estimate.queue_slots == 8);
    CHECK(x.phase_scratch_total_bytes == estimate.phase_scratch.total_bytes && x.task_scratch_total_bytes == estimate.task_scratch.total_bytes && x.queue_slots == estimate.queue_slots);
    CHECK(raw.start() == rt::Status::ok && typed.start() == rt::Status::ok);
    bool success = true;
    rtfw_physics_allocation::begin();
    for (std::uint64_t i = 0; i < 100; ++i) {
        success = (raw.step({i, std::chrono::milliseconds(1)}) == rt::Status::ok) && success;
        success = (typed.step({i, std::chrono::milliseconds(1)}) == rt::Status::ok) && success;
        std::array<char, 256> diagnostic{};
        success = !sdk::render_error(diagnostic, "frame", rt::Status::ok).truncated && success;
        success = (sdk::estimate_native_storage(config(), 2, estimate) == rt::Status::ok) && success;
    }
    const auto allocations = rtfw_physics_allocation::end();
    CHECK(success && allocations == 0 && a.produced == 100 && a.consumed == 100 && b.produced == 100 && b.consumed == 100);
    CHECK(guard_raw.close() == rt::Status::ok && guard_typed.close() == rt::Status::ok);
    CHECK(!guard_typed.armed() && guard_typed.close() == rt::Status::ok);
    return true;
}

bool errors() {
    State s; rt::Runtime raw, typed; sdk::GraphBuilder graph(typed);
    auto c = config(); c.worker_count = 0;
    sdk::ConfigBuilder invalid(c);
    CHECK(invalid.apply(typed) == raw.configure(c) && invalid.apply(typed) == rt::Status::invalid_config);
    sdk::ConfigBuilder builder(config());
    auto expected = config();
    for (const auto value : {"3", "4", "-1", "4garbage"}) {
        CHECK(builder.set("executor_queue_capacity", value) == rt::set_runtime_config_value(expected, "executor_queue_capacity", value));
        CHECK(builder.value().executor_queue_capacity == expected.executor_queue_capacity);
    }
    const auto unknown_key = builder.set("unknown", "1");
    CHECK(unknown_key != rt::Status::ok && unknown_key == rt::set_runtime_config_value(expected, "unknown", "1"));
    CHECK(builder.apply(typed) == rt::Status::ok);
    rt::PhaseHandle p, q, extra; rt::ResourceHandle resource;
    CHECK(graph.phase<&State::produce>("p", s, p) == rt::Status::ok);
    CHECK(graph.phase<&State::produce>("p", s, extra) == rt::Status::invalid_argument);
    CHECK(graph.phase<&State::consume>("q", s, q) == rt::Status::ok);
    CHECK(graph.phase<&State::produce>("extra", s, extra) == rt::Status::capacity_exceeded);
    CHECK(graph.depends_on(q, {}) == rt::Status::invalid_handle);
    CHECK(graph.resource("state", resource) == rt::Status::ok);
    CHECK(graph.access(p, resource, rt::ResourceAccess::write) == rt::Status::ok);
    CHECK(graph.access(q, resource, rt::ResourceAccess::write) == rt::Status::ok);
    CHECK(graph.finalize() == rt::Status::resource_conflict);
    CHECK(graph.depends_on(q, p) == rt::Status::ok);
    CHECK(graph.depends_on(q, p) == rt::Status::invalid_argument);
    CHECK(graph.finalize() == rt::Status::ok); // Failed finalize remains repairable.
    sdk::CheckedStopGuard stop(typed);
    CHECK(graph.resource("late", resource) == rt::Status::invalid_state);
    CHECK(stop.close() == rt::Status::ok);
    rt::Runtime cyclic; sdk::GraphBuilder cycle(cyclic);
    CHECK(cyclic.configure(config()) == rt::Status::ok);
    CHECK(cycle.phase<&State::produce>("p", s, p) == rt::Status::ok && cycle.phase<&State::consume>("q", s, q) == rt::Status::ok);
    CHECK(cycle.depends_on(p, q) == rt::Status::ok && cycle.depends_on(q, p) == rt::Status::ok);
    CHECK(cycle.finalize() == rt::Status::graph_cycle);
    rt::Runtime failed; sdk::GraphBuilder fail(failed);
    CHECK(failed.configure(config()) == rt::Status::ok && fail.phase<&fail_callback>("fail", s, p) == rt::Status::ok && fail.finalize() == rt::Status::ok);
    sdk::CheckedStopGuard failed_stop(failed);
    CHECK(failed.start() == rt::Status::ok && failed.step({0, std::chrono::milliseconds(1)}) == rt::Status::callback_failed);
    CHECK(failed_stop.close() == rt::Status::ok);
    return true;
}

bool failure_status_parity() {
    // Run the same invalid declarations through independent raw/helper graphs.
    auto exercise = [](bool typed) {
        std::array<rt::Status, 16> results{};
        std::size_t n = 0;
        State state; rt::Runtime runtime, other;
        sdk::GraphBuilder graph(runtime);
        auto c = config(); c.memory_budget_bytes = 1;
        results[n++] = typed ? sdk::ConfigBuilder(c).apply(runtime) : runtime.configure(c);
        rt::PhaseHandle p, q, spare, foreign;
        (void)other.register_callback({"foreign", raw_produce, &state}, foreign);
        auto phase = [&](std::string_view name, rt::PhaseHandle& out) {
            return typed ? graph.phase<&State::produce>(name, state, out)
                         : runtime.register_callback({name, raw_produce, &state}, out);
        };
        auto edge = [&](rt::PhaseHandle dependent, rt::PhaseHandle prerequisite) {
            return typed ? graph.depends_on(dependent, prerequisite) : runtime.add_dependency(prerequisite, dependent);
        };
        results[n++] = phase("", spare);
        results[n++] = phase("p", p);
        results[n++] = phase("p", spare);
        results[n++] = phase("q", q);
        results[n++] = phase("extra", spare);
        results[n++] = edge(q, foreign);
        results[n++] = edge(q, {});
        results[n++] = edge(q, p);
        results[n++] = edge(q, p);
        rt::ResourceHandle resource;
        results[n++] = typed ? graph.resource("resource", resource) : runtime.register_resource("resource", resource);
        results[n++] = typed ? graph.access(p, resource, static_cast<rt::ResourceAccess>(99))
                             : runtime.declare_resource_access(p, resource, static_cast<rt::ResourceAccess>(99));
        results[n++] = typed ? graph.finalize() : runtime.finalize(); // Budget failure.
        c.memory_budget_bytes = config().memory_budget_bytes;
        results[n++] = typed ? sdk::ConfigBuilder(c).apply(runtime) : runtime.configure(c);
        results[n++] = edge(p, q);
        results[n++] = typed ? graph.finalize() : runtime.finalize(); // Cycle failure.
        return results;
    };
    const auto raw = exercise(false), typed = exercise(true);
    CHECK(raw == typed);
    CHECK(typed[6] == rt::Status::invalid_handle && typed[12] != rt::Status::ok && typed[15] == rt::Status::graph_cycle);
    return true;
}

bool capacities_and_diagnostics() {
    sdk::AlignedCapacity out{7,9};
    CHECK(sdk::aligned_capacity(1, 3, 1, out) == rt::Status::invalid_argument && out.stride == 7 && out.total_bytes == 9);
    CHECK(sdk::aligned_capacity(1, 0, 1, out) == rt::Status::invalid_argument);
    constexpr auto maximum = std::numeric_limits<std::size_t>::max();
    CHECK(sdk::aligned_capacity(maximum, 2, 1, out) == rt::Status::capacity_exceeded);
    CHECK(sdk::aligned_capacity(maximum, 1, 2, out) == rt::Status::capacity_exceeded);
    CHECK(sdk::aligned_capacity(maximum, 1, 1, out) == rt::Status::ok && out.total_bytes == maximum);
    CHECK(sdk::aligned_capacity(0, 64, maximum, out) == rt::Status::ok && out.total_bytes == 0);
    sdk::NativeStorageEstimate estimate; estimate.queue_slots = 77;
    auto c = config();
    CHECK(sdk::estimate_native_storage(c, 3, estimate) == rt::Status::capacity_exceeded && estimate.queue_slots == 77);
    c.executor_policy = rt::ExecutorPolicy::host_adapter;
    CHECK(sdk::estimate_native_storage(c, 2, estimate) == rt::Status::invalid_config);
    c = config(); c.worker_count = maximum;
    CHECK(sdk::estimate_native_storage(c, 2, estimate) == rt::Status::capacity_exceeded && estimate.queue_slots == 77);
    std::array<char, 512> full{};
    const auto rendered = sdk::render_error(full, "finalize", rt::Status::resource_conflict, "conflicting phases");
    const std::string_view text(full.data());
    CHECK(!rendered.truncated && rendered.required_bytes == text.size() + 1 && rendered.written_bytes == text.size());
    CHECK(text.starts_with("finalize: ") && text.find("(-10): conflicting phases") != text.npos && text.find("dependency") != text.npos);
    for (std::size_t size = 0; size <= rendered.required_bytes + 1; ++size) {
        std::array<char, 512> buffer{}; buffer.fill('!');
        const auto result = sdk::render_error(std::span(buffer).first(size), "finalize", rt::Status::resource_conflict, "conflicting phases");
        CHECK(result.required_bytes == rendered.required_bytes && result.truncated == (size < rendered.required_bytes));
        CHECK(result.written_bytes == (size == 0 ? 0 : std::min(size - 1, text.size())));
        if (size) CHECK(buffer[result.written_bytes] == '\0' && std::string_view(buffer.data()) == text.substr(0, result.written_bytes));
        CHECK(buffer[size] == '!'); // No write past caller span.
    }
    const auto unknown = sdk::render_error(full, "unknown", static_cast<rt::Status>(-2147483647 - 1));
    CHECK(!unknown.truncated && std::string_view(full.data()).find("(-2147483648)") != text.npos);
    for (int status = 0; status >= -23; --status) CHECK(!sdk::recovery_hint(static_cast<rt::Status>(status)).empty());
    return true;
}

// Test-only shutdown fault injection; original instance and all other callbacks
// stay unchanged. This fixture is exercised serially, outside concurrent cases.
rtfw_device_shutdown_fn original_shutdown = nullptr;
int shutdown_failures = 0;
int shutdown_calls = 0;
rtfw_device_status shutdown(void* instance) {
    ++shutdown_calls;
    if (shutdown_failures > 0) { --shutdown_failures; return RTFW_DEVICE_STATUS_RESET_REQUIRED; }
    return original_shutdown(instance);
}
bool cleanup(bool death = false) {
    rt::MockDeviceBackend backend;
    State state; rt::Runtime runtime;
    CHECK(prepare(runtime, state, true));
    { sdk::CheckedStopGuard fallback(runtime); CHECK(fallback.armed());
      CHECK(runtime.start() == rt::Status::ok); }
    CHECK(runtime.state() == rt::RuntimeState::stopped);
    rt::Runtime device;
    CHECK(device.configure(config()) == rt::Status::ok);
    auto api = backend.api(); original_shutdown = api.shutdown; api.shutdown = &shutdown;
    rt::DeviceBackendHandle handle;
    CHECK(device.register_device_backend({"cleanup", api}, handle) == rt::Status::ok);
    CHECK(device.finalize() == rt::Status::ok);
    sdk::CheckedStopGuard stop(device);
    CHECK(device.start() == rt::Status::ok);
    shutdown_calls = 0; shutdown_failures = death ? 100 : 1;
    if (death) return true; // Destructor must terminate after exactly one failed attempt.
    CHECK(stop.close() == rt::Status::device_reset_required && stop.armed() && shutdown_calls == 1);
    CHECK(stop.close() == rt::Status::ok && !stop.armed() && shutdown_calls == 2);
    CHECK(stop.close() == rt::Status::ok && shutdown_calls == 2);
    return true;
}

bool isolation() {
    State a, b; rt::Runtime first, second;
    CHECK(prepare(first, a, true) && prepare(second, b, true));
    sdk::CheckedStopGuard stop_a(first), stop_b(second);
    rt::PhaseHandle foreign;
    CHECK(first.compiled_phase_at(0, foreign));
    CHECK(foreign.owner() != [&] { rt::PhaseHandle p; (void)second.compiled_phase_at(0,p); return p.owner(); }());
    CHECK(first.start() == rt::Status::ok && second.start() == rt::Status::ok);
    auto run = [](rt::Runtime& runtime, std::uint64_t count, bool& ok) {
        ok = true;
        for (std::uint64_t i = 0; i < count; ++i)
            if (runtime.step({i, std::chrono::milliseconds(1)}) != rt::Status::ok) ok = false;
    };
    bool good_a = false, good_b = false;
    std::thread x(run, std::ref(first), 17, std::ref(good_a));
    std::thread y(run, std::ref(second), 29, std::ref(good_b));
    x.join(); y.join();
    CHECK(good_a && good_b && a.consumed == 17 && b.consumed == 29);
    CHECK(stop_a.close() == rt::Status::ok && stop_b.close() == rt::Status::ok);
    return true;
}

int main(int argc, char**) {
    if (argc > 1) {
        std::set_terminate([] { std::_Exit(shutdown_calls == 1 ? 73 : 74); });
        return cleanup(true) ? 2 : 1;
    }
    if (!parity_and_allocation() || !errors() || !failure_status_parity() || !capacities_and_diagnostics() || !cleanup() || !isolation()) return 1;
    std::cout << "typed SDK: parity, errors, capacities, diagnostics, allocation, cleanup and isolation passed\n";
}
