#include "../../samples/backend_authoring/backend.hpp"
#include "../../samples/backend_authoring/conformance.hpp"
#include "../../samples/backend_authoring/runtime_example.hpp"
#include <iostream>
#include <thread>
#include "../cuda_physics/allocation_guard.hpp"

namespace kit = backend_kit;
using S = kit::Status;
#define CHECK(x) do { if (!(x)) { std::cerr << __func__ << ':' << __LINE__ << " failed: " #x << '\n'; return false; } } while (false)
kit::Profile profile(kit::Backend& b) {
    return {kit::copy_opcode, &b,
        [](void* p, kit::Fault f) noexcept { static_cast<kit::Backend*>(p)->inject(f); },
        [](void* p) noexcept { return static_cast<kit::Backend*>(p)->owns_setup(); },
        [](void* p) noexcept { return static_cast<kit::Backend*>(p)->registered(); }};
}
bool baseline() {
    kit::Backend b; kit::Storage s;
    const auto r = kit::run(b.api(), profile(b), s);
    for (std::size_t i = 0; i < r.count; ++i) if (!r.checks[i].passed) std::cerr << r.checks[i].name << '\n';
    CHECK(r.passed()); CHECK(r.count > 60 && r.count <= 128);
    return true;
}
// Independent protocol corrupter: each defect changes one observable contract.
// Stored real tokens allow test teardown without pretending malformed output is safe.
enum class Defect { capabilities, capacity, token, identity, count, content,
                    failure, reset, unregister_success, shutdown_success };
struct Broken {
    kit::Backend backend;
    Defect defect;
    std::array<std::uint64_t, 8> acquired{};
    std::byte* destination = nullptr;
    explicit Broken(Defect d) : defect(d) {}
    static Broken& get(void* p) { return *static_cast<Broken*>(p); }
    rt::HalV2BackendApi api() {
        rt::HalV2BackendApi a; a.instance = this;
        a.get_capabilities = [](void* p, rt::HalV2Capabilities* c) {
            auto& b = get(p); auto x = b.backend.api(); auto result = x.get_capabilities(x.instance, c);
            if (b.defect == Defect::capabilities) c->reserved[0] = 1;
            return result;
        };
        a.initialize = [](void* p, const rt::HalV2InitializeConfig* c) {
            auto& b = get(p); auto x = b.backend.api(); auto changed = *c;
            if (b.defect == Defect::capacity) changed.requested_registered_buffers = 2;
            return x.initialize(x.instance, &changed);
        };
        a.register_buffer = [](void* p, const rt::HalV2BufferRegistration* r, std::uint64_t* token) {
            auto& b = get(p); auto x = b.backend.api(); auto result = x.register_buffer(x.instance, r, token);
            if (result == S::ok) {
                if (b.backend.registered() == 2) b.destination = static_cast<std::byte*>(r->data);
                for (auto& saved : b.acquired) if (!saved) { saved = *token; break; }
                if (b.defect == Defect::token) *token = 0;
            }
            return result;
        };
        a.unregister_buffer = [](void* p, std::uint64_t token) {
            auto& b = get(p); auto x = b.backend.api();
            if (b.defect == Defect::unregister_success) return S::ok;
            return x.unregister_buffer(x.instance, token);
        };
        a.submit = [](void* p, const rt::HalV2Submission* c) { auto x = get(p).backend.api(); return x.submit(x.instance, c); };
        a.poll = [](void* p, rt::HalV2Completion* c, std::uint64_t cap, std::uint64_t* count) {
            auto& b = get(p); auto x = b.backend.api(); const auto result = x.poll(x.instance, c, cap, count);
            if (*count) {
                if (b.defect == Defect::identity) ++c->submission_id;
                if (b.defect == Defect::count) *count = cap + 1;
                if (b.defect == Defect::content && b.destination) b.destination[0] ^= std::byte{1};
                if (b.defect == Defect::failure) c->status = 0;
            }
            return result;
        };
        a.cancel = [](void* p, std::uint64_t id) { auto x = get(p).backend.api(); return x.cancel(x.instance, id); };
        a.get_health = [](void* p, rt::HalV2Health* h) { auto x = get(p).backend.api(); return x.get_health(x.instance, h); };
        a.reset = [](void* p) {
            auto& b = get(p); auto x = b.backend.api(); rt::HalV2Health h;
            (void)x.get_health(x.instance, &h);
            return b.defect == Defect::reset && h.outstanding == 0 ? S::ok : x.reset(x.instance);
        };
        a.shutdown = [](void* p) { auto& b = get(p); auto x = b.backend.api(); return b.defect == Defect::shutdown_success ? S::ok : x.shutdown(x.instance); };
        return a;
    }
    bool repair_cleanup() {
        backend.inject(kit::Fault::none);
        auto a = backend.api();
        for (auto token : acquired) if (token) (void)a.unregister_buffer(a.instance, token);
        if (backend.owns_setup()) (void)a.shutdown(a.instance);
        return !backend.owns_setup() && backend.registered() == 0;
    }
};
bool negative_controls() {
    for (auto d : {Defect::capabilities, Defect::capacity, Defect::token, Defect::identity, Defect::count,
                   Defect::content, Defect::failure, Defect::reset, Defect::unregister_success, Defect::shutdown_success}) {
        Broken b(d); kit::Storage s;
        const auto r = kit::run(b.api(), profile(b.backend), s);
        CHECK(!r.passed() && r.failures > 0);
        if (d == Defect::token || d == Defect::unregister_success || d == Defect::shutdown_success)
            CHECK(!r.cleanup_complete);
        CHECK(b.repair_cleanup()); // Storage still alive during fixture repair.
    }
    kit::Backend b; kit::Storage s; auto a = b.api(); a.poll = nullptr;
    CHECK(!kit::run(a, profile(b), s).passed());
    return true;
}
bool setup(kit::Backend& b, kit::Storage& s) {
    const auto a = b.api(); rt::HalV2InitializeConfig c;
    c.requested_in_flight = 1; c.requested_registered_buffers = 2;
    s.setup_attempted = true; CHECK(a.initialize(a.instance, &c) == S::ok);
    auto src = kit::registration(s.source.data()); auto dst = kit::registration(s.destination.data());
    CHECK(a.register_buffer(a.instance, &src, &s.tokens[0]) == S::ok);
    CHECK(a.register_buffer(a.instance, &dst, &s.tokens[1]) == S::ok);
    s.source.fill(std::byte{0x69}); return true;
}
bool registration_boundaries() {
    kit::Backend b; const auto a = b.api();
    rt::HalV2InitializeConfig c; c.requested_in_flight = 1; c.requested_registered_buffers = 2;
    auto invalid_config = c; invalid_config.requested_in_flight = 2;
    CHECK(a.initialize(a.instance, &invalid_config) == S::invalid_argument && !b.owns_setup());
    CHECK(a.initialize(a.instance, &c) == S::ok);
    std::array<std::byte, 256> source{}, destination{};
    auto r = kit::registration(source.data(), source.size());
    std::uint64_t token = 99;
    for (unsigned kind = 0; kind < 8; ++kind) {
        auto bad = r;
        switch (kind) {
        case 0: bad.struct_size = 0; break;
        case 1: bad.bytes = 257; break;
        case 2: bad.bytes = 0; break;
        case 3: bad.data = nullptr; break;
        case 4: bad.flags = 16; break;
        case 5: bad.name.fill('x'); break;
        case 6: bad.name.fill('\0'); break;
        default: bad.reserved[3] = 1; break;
        }
        CHECK(a.register_buffer(a.instance, &bad, &token) == S::invalid_argument && token == 0 && b.registered() == 0);
    }
    r.flags = RTFW_DEVICE_BUFFER_HOST_READ;
    CHECK(a.register_buffer(a.instance, &r, &token) == S::ok);
    auto d = kit::registration(destination.data(), destination.size()); std::uint64_t dst = 0;
    CHECK(a.register_buffer(a.instance, &d, &dst) == S::ok);
    kit::Storage handles; handles.tokens[0] = token; handles.tokens[1] = dst;
    auto command = kit::submission(handles, kit::copy_opcode);
    CHECK(a.submit(a.instance, &command) == S::invalid_argument); // Device permission absent.
    CHECK(a.unregister_buffer(a.instance, token) == S::ok);
    r.flags = 15; CHECK(a.register_buffer(a.instance, &r, &token) == S::ok);
    command.buffers[0].buffer_token = token;
    command.buffers[0].bytes = 256; command.buffers[1].bytes = 256;
    for (std::size_t i = 0; i < source.size(); ++i) source[i] = static_cast<std::byte>(i);
    CHECK(a.submit(a.instance, &command) == S::ok);
    rt::HalV2Completion out; std::uint64_t count = 0;
    CHECK(a.poll(a.instance, &out, 1, &count) == S::ok && count == 1 && out.value == 256 && source == destination);
    CHECK(a.unregister_buffer(a.instance, token) == S::ok);
    CHECK(a.unregister_buffer(a.instance, dst) == S::ok);
    CHECK(a.shutdown(a.instance) == S::ok);
    return true;
}
bool boundaries_and_allocations() {
    kit::Backend b; kit::Storage s; CHECK(setup(b, s)); auto a = b.api();
    auto command = kit::submission(s, kit::copy_opcode);
    auto invalid = command;
    invalid.struct_size = 0; CHECK(a.submit(a.instance, &invalid) == S::invalid_argument);
    invalid = command; invalid.api_version = 1; CHECK(a.submit(a.instance, &invalid) == S::invalid_argument);
    invalid = command; invalid.timeout_ns = 0; CHECK(a.submit(a.instance, &invalid) == S::invalid_argument);
    invalid = command; invalid.submission_id = 0; CHECK(a.submit(a.instance, &invalid) == S::invalid_argument);
    invalid = command; invalid.buffer_count = 9; CHECK(a.submit(a.instance, &invalid) == S::invalid_argument);
    invalid = command; invalid.payload_size = 129; CHECK(a.submit(a.instance, &invalid) == S::invalid_argument);
    invalid = command; invalid.buffers[1].bytes = 33; CHECK(a.submit(a.instance, &invalid) == S::invalid_argument);
    invalid = command; invalid.buffers[1].reserved0 = 1; CHECK(a.submit(a.instance, &invalid) == S::invalid_argument);
    rt::HalV2Completion out; std::uint64_t count = 99;
    CHECK(a.poll(a.instance, &out, 0, &count) == S::invalid_argument && count == 0);
    CHECK(a.submit(nullptr, &command) == S::invalid_argument);
    CHECK(a.submit(a.instance, nullptr) == S::invalid_argument);
    bool success = true;
    rtfw_physics_allocation::begin();
    for (std::uint64_t id = 1; id <= 1000; ++id) {
        command.submission_id = id;
        success = success && a.submit(a.instance, &command) == S::ok &&
            a.poll(a.instance, &out, 1, &count) == S::ok && count == 1 && out.submission_id == id && out.status == 0;
    }
    const auto allocations = rtfw_physics_allocation::end();
    CHECK(success && allocations == 0 && s.source == s.destination);
    const auto old = s.tokens;
    CHECK(kit::cleanup(a, profile(b), s));
    for (unsigned generation = 0; generation < 32; ++generation) {
        CHECK(setup(b, s)); CHECK(s.tokens[0] > old[1]);
        invalid = kit::submission(s, kit::copy_opcode); invalid.buffers[0].buffer_token = old[0];
        CHECK(a.submit(a.instance, &invalid) == S::invalid_argument);
        CHECK(kit::cleanup(a, profile(b), s));
    }
    return true;
}
bool concurrency() {
    kit::Backend b; kit::Storage s; CHECK(setup(b, s)); const auto a = b.api();
    constexpr std::uint64_t total = 10000;
    std::atomic<bool> failed{false}, submitted{false};
    std::uint64_t accepted = 0, completed = 0;
    std::thread producer([&] {
        auto c = kit::submission(s, kit::copy_opcode);
        for (std::uint64_t attempt = 0; attempt < 5'000'000 && accepted < total; ++attempt) {
            c.submission_id = accepted + 1; const auto result = a.submit(a.instance, &c);
            if (result == S::ok) ++accepted;
            else if (result != S::queue_full) { failed.store(true); break; }
            // Fixture scheduling only: give the peer a turn on saturation.
            if (result == S::queue_full) std::this_thread::yield();
        }
        if (accepted != total) failed.store(true);
        submitted.store(true);
    });
    std::thread consumer([&] {
        std::array<bool, total + 1> seen{};
        for (std::uint64_t attempt = 0; attempt < 5'000'000 && completed < total; ++attempt) {
            rt::HalV2Completion c; std::uint64_t count = 0;
            if (a.poll(a.instance, &c, 1, &count) != S::ok || count > 1) { failed.store(true); break; }
            if (count) {
                if (c.submission_id == 0 || c.submission_id > total || seen[c.submission_id] || c.status != 0) { failed.store(true); break; }
                seen[c.submission_id] = true; ++completed;
            }
            if (submitted.load() && failed.load()) break;
            if (count == 0) std::this_thread::yield();
        }
    });
    producer.join(); consumer.join();
    if (failed.load() || accepted != total || completed != total)
        std::cerr << "concurrency accepted=" << accepted << " completed=" << completed
                  << " failed=" << failed.load() << '\n';
    CHECK(!failed.load() && accepted == total && completed == total && s.source == s.destination);
    rt::HalV2Health h; CHECK(a.get_health(a.instance, &h) == S::ok);
    CHECK(h.outstanding == 0 && h.submissions == total && h.completions == total);
    CHECK(kit::cleanup(a, profile(b), s)); return true;
}
bool cancel_race() {
    kit::Backend b; kit::Storage s; CHECK(setup(b, s)); const auto a = b.api();
    for (std::uint64_t id = 1; id <= 128; ++id) {
        s.destination.fill(std::byte{0});
        auto c = kit::submission(s, kit::copy_opcode, id);
        CHECK(a.submit(a.instance, &c) == S::ok);
        rt::HalV2Completion out; std::uint64_t count = 0;
        S polled = S::error;
        std::thread poller([&] { polled = a.poll(a.instance, &out, 1, &count); });
        const auto canceled = a.cancel(a.instance, id);
        poller.join();
        CHECK(polled == S::ok);
        if (canceled == S::ok) {
            CHECK(count == 0 && std::all_of(s.destination.begin(), s.destination.end(), [](auto x) { return x == std::byte{0}; }));
        } else {
            CHECK(canceled == S::invalid_state && count == 1 && out.submission_id == id && out.status == 0 && s.destination == s.source);
        }
        const auto bytes = s.destination;
        CHECK(a.poll(a.instance, &out, 1, &count) == S::ok && count == 0 && bytes == s.destination);
    }
    CHECK(kit::cleanup(a, profile(b), s)); return true;
}
bool runtime_case(kit::Fault fault = kit::Fault::none, rt::Status expected = rt::Status::ok, bool measure = true) {
    kit::Backend native; rt::MockDeviceBackend legacy({1, 1, 1, 1000});
    kit::RuntimeState state; rt::Runtime runtime;
    CHECK(kit::configure(runtime, native, legacy, state) == rt::Status::ok);
    rt::sdk::CheckedStopGuard stop(runtime);
    CHECK(runtime.start() == rt::Status::ok);
    native.inject(fault);
    CHECK(runtime.step({0, std::chrono::milliseconds(1)}) == expected);
    CHECK(state.verified == (expected == rt::Status::ok ? 1u : 0u));
    if (expected == rt::Status::ok) {
        bool success = true; if (measure) rtfw_physics_allocation::begin();
        for (std::uint64_t i = 1; i < 100; ++i)
            success = success && runtime.step({i, std::chrono::milliseconds(1)}) == rt::Status::ok;
        const auto count = measure ? rtfw_physics_allocation::end() : 0;
        CHECK(success && count == 0 && state.verified == 100);
    }
    native.inject(kit::Fault::unregister);
    CHECK(stop.close() != rt::Status::ok);
    CHECK(native.owns_setup() && native.registered() != 0);
    native.inject(kit::Fault::shutdown);
    CHECK(stop.close() != rt::Status::ok);
    CHECK(native.owns_setup() && native.registered() == 0);
    CHECK(stop.close() == rt::Status::ok);
    CHECK(!native.owns_setup() && native.registered() == 0);
    return true;
}
bool start_failure(kit::Fault fault) {
    kit::Backend native; rt::MockDeviceBackend legacy({1, 1, 1, 1000});
    kit::RuntimeState state; rt::Runtime runtime;
    CHECK(kit::configure(runtime, native, legacy, state) == rt::Status::ok);
    rt::sdk::CheckedStopGuard stop(runtime); native.inject(fault);
    CHECK(runtime.start() != rt::Status::ok);
    CHECK(stop.close() == rt::Status::ok);
    CHECK(!native.owns_setup() && native.registered() == 0);
    return true;
}
int main() {
    if (!baseline() || !negative_controls() || !registration_boundaries() || !boundaries_and_allocations() || !concurrency() || !cancel_race() ||
        !runtime_case() || !runtime_case(kit::Fault::completion_error, rt::Status::device_error) ||
        !runtime_case(kit::Fault::completion_timeout, rt::Status::device_timeout) ||
        !runtime_case(kit::Fault::completion_loss, rt::Status::device_lost) ||
        !start_failure(kit::Fault::initialize) || !start_failure(kit::Fault::registration)) return 1;
    bool first = false, second = false;
    std::thread a([&] { first = runtime_case(kit::Fault::none, rt::Status::ok, false); });
    std::thread b([&] { second = runtime_case(kit::Fault::none, rt::Status::ok, false); });
    a.join(); b.join();
    if (!first || !second) return 1;
    std::cout << "backend conformance, 10 negative controls, boundaries, generations, concurrency, Runtime, ownership and allocation passed\n";
}
