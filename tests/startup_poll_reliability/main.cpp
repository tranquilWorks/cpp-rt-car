// Public API ordering controls; all predecessor fixtures remain unchanged.
#define RTFW_DEVICE_CAPACITY_NO_MAIN
#include "../package_consumer/device_capacity_consumer.cpp"
#define RTFW_SAMPLED_SIMULATION_NO_MAIN
#include "../package_consumer/sampled_io_simulation_consumer.cpp"
#include "../golden_cuda/allocation.hpp"
#include <cstdlib>
#include <new>

namespace {
using rt::Status;
namespace allocation = rtfw_physics_allocation;
void require(bool value, const char* message) {
    if (!value) { std::fprintf(stderr, "FAIL %s\n", message); std::exit(1); }
}
void check(Status status) { require(status == Status::ok, "setup/cleanup status"); }
struct Gate { std::atomic<bool> open{false}, expired{false}; };
struct Backend : device_capacity::Backend {
    Gate* gate = nullptr;
    unsigned index = 0;
    bool wait_for_submit = false;
    std::atomic<bool> armed{false}, consumed{false}, idle_consumed{false};
    rt::HalV2Status fault = rt::HalV2Status::ok; // ok denotes count overflow
    rt::HalV2CommandTimelineExtension original{};
    rt::HalV2CommandTimelineExtension wrapped() {
        original = commands();
        auto extension = original;
        extension.poll = [](void* p, rt::HalV2BatchCompletion* out,
                            std::uint64_t capacity, std::uint64_t* count) {
            auto& self = *static_cast<Backend*>(
                static_cast<device_capacity::Backend*>(p));
            if (self.wait_for_submit && self.armed.load() &&
                self.submitted.load() == 0) {
                *count = 0;
                return rt::HalV2Status::ok;
            }
            if ((!self.wait_for_submit || self.submitted.load() != 0) &&
                self.armed.exchange(false)) {
                self.idle_consumed = !self.gate->open.load();
                self.consumed = true;
                *count = self.fault == rt::HalV2Status::ok ? capacity + 1 : 0;
                return self.fault;
            }
            const auto status = self.original.poll(p, out, capacity, count);
            // On the baseline, the shared service lane has already polled
            // backend0 with no published batch when backend1 opens this gate.
            if (self.index == 1 && *count != 0) self.gate->open = true;
            return status;
        };
        return extension;
    }
};
struct Provider {
    Gate* gate = nullptr;
    unsigned index = 0;
    rt::DeviceCommandBatch batch{};
    static rt::CallbackResult call(void* p, const rt::DeviceCallbackContext&,
                                   rt::DeviceCommandBatch& out) {
        auto& self = *static_cast<Provider*>(p);
        const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (self.index == 0 && !self.gate->open.load()) {
            if (std::chrono::steady_clock::now() >= limit) {
                self.gate->expired = true;
                return rt::CallbackResult::error;
            }
            std::this_thread::yield();
        }
        out = self.batch;
        out.timeout_ns = 5'000'000'000;
        out.signals[0].value = 1;
        return rt::CallbackResult::ok;
    }
};
struct PollFixture {
    Gate gate;
    std::array<Backend, 2> backends;
    std::array<Provider, 2> providers;
    rt::Runtime runtime;
    explicit PollFixture(bool uniform = false) {
        rt::RuntimeConfig cfg;
        cfg.worker_count = 2; cfg.callback_capacity = 2;
        cfg.device_backend_capacity = 2;
        cfg.device_outstanding_capacity = cfg.device_completion_batch = 2;
        check(runtime.configure(cfg));
        if (!uniform) check(runtime.set_device_capacity_policy(
            rt::DeviceCapacityPolicy::native_per_backend));
        for (unsigned i = 0; i != 2; ++i) {
            auto& backend = backends[i];
            backend.pending_capacity = uniform ? 2u : 1u;
            backend.gate = &gate; backend.index = i;
            auto memory = backend.memory(); auto commands = backend.wrapped();
            rt::DeviceBackendHandle device;
            const char* name = i == 0 ? "first" : "second";
            check(runtime.register_device_backend({name, backend.core(), &memory, &commands}, device));
            rt::DeviceTimelineHandle timeline;
            check(runtime.register_device_timeline({name, device, 0}, timeline));
            auto& provider = providers[i]; provider.gate = &gate; provider.index = i;
            auto& batch = provider.batch;
            batch.command_count = batch.signal_count = 1;
            batch.commands[0].kind = static_cast<std::uint32_t>(rt::HalV2CommandKind::dispatch);
            batch.commands[0].opcode = 2642;
            batch.signals[0].timeline_handle = timeline.value;
            rt::PhaseHandle phase;
            check(runtime.register_device_batch_phase({name, device, Provider::call, &provider, batch}, phase));
        }
        check(runtime.finalize()); check(runtime.start());
    }
    Status run(bool measured = true) {
        if (measured) allocation::begin();
        const auto status = runtime.step({0, std::chrono::milliseconds(1)});
        const auto stopped = runtime.stop();
        const auto repeated = runtime.stop();
        const auto allocations = measured ? allocation::end() : 0;
        require(allocations == 0, "poll/checked stop allocated");
        check(stopped); check(repeated);
        require(!gate.expired && gate.open, "ordering gate did not complete");
        for (auto& backend : backends) {
            require(backend.initialized == 1 && backend.shutdowns == 1,
                    "backend lifetime imbalance");
            require(backend.registered == backend.unregistered, "buffer ownership imbalance");
        }
        return status;
    }
};
Status mapped(rt::HalV2Status status) {
    switch (status) {
    case rt::HalV2Status::lost: return Status::device_lost;
    case rt::HalV2Status::reset_required: return Status::device_reset_required;
    case rt::HalV2Status::canceled: return Status::device_canceled;
    case rt::HalV2Status::timeout: return Status::device_timeout;
    default: return Status::device_error;
    }
}
void poll_fault(rt::HalV2Status fault, bool active, bool uniform) {
    PollFixture fixture(uniform);
    fixture.backends[0].fault = fault;
    fixture.backends[0].wait_for_submit = active;
    fixture.backends[0].armed = true;
    const auto status = fixture.run();
    std::printf("poll fault=%d active=%d uniform=%d status=%d consumed=%d idle=%d\n",
        int(fault), active, uniform, int(status), fixture.backends[0].consumed.load(),
        fixture.backends[0].idle_consumed.load());
    require(status == mapped(fault), "one-shot poll fault lost or remapped");
    require(fixture.backends[0].consumed && !fixture.backends[0].idle_consumed,
            "poll fault consumed without a published owner");
}
void pending_startup() {
    sampled_simulation::Fixture fixture;
    fixture.simulation = rt::DeviceRateSimulationPolicy{100'000'000};
    auto api = fixture.backend.card.api();
    // Nonblocking cancellation request does not promise terminal native work.
    // Keep the card callback active until the test releases it after two stops.
    api.request_stop = [](void*) noexcept { return rt::XdmaDriverResult::success; };
    rt::XdmaBackendConfig config;
    config.queue_capacity = 4; config.buffer_capacity = 1; config.worker_count = 1;
    config.h2c_channel_count = config.c2h_channel_count = 1;
    config.max_buffer_bytes = 4096; config.max_transfer_bytes = 128;
    config.transfer_alignment = 1; config.control_aperture_bytes = 4;
    config.user_event_count = 1;
    fixture.backend.native = std::make_unique<rt::XdmaDeviceBackend>(api, config);
    check(fixture.configure()); check(fixture.runtime.finalize());
    fixture.backend.card.hold = true;
    std::atomic<bool> entered{false};
    std::thread clock_owner([&] {
        const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!fixture.backend.card.entered && std::chrono::steady_clock::now() < limit)
            std::this_thread::yield();
        entered = fixture.backend.card.entered.load();
        if (entered) fixture.clock.now = 8'001'001;
    });
    const auto start = fixture.runtime.start(); clock_owner.join();
    rt::SampledIoChannelStatus info;
    const bool found = fixture.runtime.sampled_io_channel_status(fixture.output, info);
    const auto pending = fixture.runtime.stop();
    const auto pending_again = fixture.runtime.stop();
    const bool retained = fixture.backend.card.live;
    fixture.backend.card.hold = false;
    const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    Status stopped = Status::invalid_state;
    allocation::begin();
    do {
        stopped = fixture.runtime.stop();
        if (stopped == Status::ok) break;
        std::this_thread::yield();
    } while (std::chrono::steady_clock::now() < limit);
    const auto allocations = allocation::end();
    std::printf("startup=%d channel=%d pending=%d repeated=%d live-before=%d cleanup=%d live-after=%d\n",
        int(start), int(info.last_status), int(pending), int(pending_again), retained,
        int(stopped), fixture.backend.card.live.load());
    require(entered && fixture.backend.card.event_timeout == 8'000'000, "safe8ms native event not entered");
    require(start == Status::invalid_state && pending == Status::invalid_state &&
            pending_again == Status::invalid_state && retained, "pending startup ownership lost");
    require(found && info.last_status == Status::device_timeout &&
            info.safety_state == rt::SampledIoSafetyState::unknown, "original timeout/safety lost");
    require(stopped == Status::ok && !fixture.backend.card.live && allocations == 0,
            "terminal checked cleanup failed/allocated");
    check(fixture.runtime.stop());
}
} // namespace
int main(int argc, char** argv) {
    allocation::begin();
    void* positive = ::operator new(32);
    const auto count = allocation::end(); ::operator delete(positive);
    require(count != 0, "allocation positive control");
    const bool startup_only = argc == 2 && std::strcmp(argv[1], "startup") == 0;
    if (!startup_only) {
        for (bool uniform : {false, true}) for (bool active : {false, true})
            for (auto fault : {rt::HalV2Status::ok, rt::HalV2Status::error,
                               rt::HalV2Status::lost, rt::HalV2Status::reset_required,
                               rt::HalV2Status::canceled, rt::HalV2Status::timeout})
                poll_fault(fault, active, uniform);
        for (unsigned iteration = 0; iteration != 24; ++iteration) {
            PollFixture fixture; check(fixture.run());
        }
        PollFixture first, second;
        first.backends[0].armed = true;
        Status first_status = Status::ok;
        std::thread worker([&] { first_status = first.run(false); });
        const auto second_status = second.run(false); worker.join();
        require(first_status == Status::device_error && second_status == Status::ok,
                "independent owners contaminated");
    }
    for (unsigned repeat = 0; repeat != 4; ++repeat) pending_startup();
    // Preserve the prior exact timeout, cancellation, missing ACK and delayed
    // completion oracles. These selected simulator cases control host ordering.
    for (unsigned id : {0u, 1u, 2u, 6u, 8u, 10u})
        require(sampled_simulation::run_case(id), "preserved sampled lifecycle oracle");
    std::puts("PASS startup/poll ordering, fault conservation, checked ownership and allocation");
    return 0;
}
