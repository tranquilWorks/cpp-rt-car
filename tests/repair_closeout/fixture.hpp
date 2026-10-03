#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <rt/loopback_backend.hpp>

inline void require(bool condition, const char* message) {
    if (!condition) { std::fprintf(stderr, "FAIL %s\n", message); std::abort(); }
}
using H = rt::HalV2Status;
template<class Backend> struct Fixture {
    Backend backend{rt::SampledIoLoopbackConfig{1, 2}};
    rt::HalV2BackendRegistration registration = backend.hal_v2_registration();
    const rt::HalV2CommandTimelineExtension& commands = *registration.command_timeline;
    Fixture() { initialize(); }
    void initialize() {
        rt::HalV2InitializeConfig init{};
        init.requested_in_flight = 1;
        init.requested_registered_buffers = 2;
        require(registration.api.initialize(registration.api.instance, &init) == H::ok, "initialize");
    }
    // Empty command lists isolate completion ownership from caller buffer access.
    // The original backend suite continues to test real sampled-frame transfers.
    H submit(std::uint64_t id) {
        rt::DeviceCommandBatch batch{};
        batch.batch_id = id;
        batch.signal_count = 1;
        batch.signals[0].timeline_handle = 55;
        batch.signals[0].value = id;
        return commands.submit(commands.instance, &batch);
    }
    H cancel(std::uint64_t id) { return commands.cancel(commands.instance, id); }
    std::uint64_t poll(rt::HalV2BatchCompletion& completion) {
        std::uint64_t count = 99;
        require(commands.poll(commands.instance, &completion, 1, &count) == H::ok, "poll status");
        return count;
    }
    void completed(std::uint64_t id, H status) {
        rt::HalV2BatchCompletion completion{};
        require(poll(completion) == 1, "one completion");
        require(completion.batch_id == id && completion.signal_count == 1 &&
                completion.signals[0].value == id && completion.signals[0].timeline_handle == 55,
                "completion identity and signal");
        require(completion.status == static_cast<std::int32_t>(status), "completion status");
        require(poll(completion) == 0, "no duplicate completion");
    }
    void stop() { require(commands.request_stop(commands.instance) == H::ok, "stop"); }
    void shutdown() { require(registration.api.shutdown(registration.api.instance) == H::ok, "shutdown"); }
};
