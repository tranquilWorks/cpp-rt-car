#pragma once
#include "profile.hpp"
#include <array>
#include <cstring>

namespace backend_kit {
// This is a named copy-command profile, not universal HAL qualification.
struct Profile {
    std::uint32_t opcode = copy_opcode;
    void* fixture = nullptr;
    void (*inject)(void*, Fault) noexcept = nullptr;
    bool (*owns_setup)(void*) noexcept = nullptr;
    std::size_t (*registered)(void*) noexcept = nullptr;
};
struct Check { const char* name{}; bool passed{}; };
struct Report {
    std::array<Check, 128> checks{};
    std::size_t count = 0, failures = 0;
    bool cleanup_complete = false;
    void expect(const char* name, bool pass) noexcept {
        if (count < checks.size()) checks[count++] = {name, pass};
        else pass = false;
        if (!pass) ++failures;
    }
    bool passed() const noexcept { return failures == 0 && cleanup_complete; }
};
// KEEP ALIVE until cleanup_complete, including after run() returns failure.
// Never reuse with another instance while it holds tokens/setup ownership.
struct Storage {
    std::array<std::byte, 32> source{}, destination{};
    std::array<std::uint64_t, 3> tokens{}; // Third slot retains unexpected over-capacity acquisition.
    bool setup_attempted = false;
};
inline rt::HalV2BufferRegistration registration(void* data, std::uint64_t bytes = 32) noexcept {
    rt::HalV2BufferRegistration r; r.data = data; r.bytes = bytes; r.flags = 15;
    constexpr char name[] = "conformance.buffer";
    std::copy_n(name, sizeof(name), r.name.begin()); return r;
}
inline rt::HalV2Submission submission(const Storage& s, std::uint32_t opcode,
                                     std::uint64_t id = 41) noexcept {
    rt::HalV2Submission c; c.opcode = opcode; c.submission_id = id;
    c.timeout_ns = 10'000'000'000; c.buffer_count = 2;
    c.buffers[0] = {s.tokens[0], RTFW_DEVICE_ACCESS_READ, 0, 0, 32};
    c.buffers[1] = {s.tokens[1], RTFW_DEVICE_ACCESS_WRITE, 0, 0, 32}; return c;
}
inline bool valid_table(const rt::HalV2BackendApi& a) noexcept {
    return a.struct_size >= sizeof(a) && a.api_version == rt::hal_v2_api_version &&
        a.instance && zero(a.reserved) && a.get_capabilities && a.initialize &&
        a.register_buffer && a.unregister_buffer && a.submit && a.poll && a.cancel &&
        a.get_health && a.reset && a.shutdown;
}
// At most five calls per retry; no internal wait/retry loop. Caller can repair
// fixture/driver state and explicitly retry while preserving Storage and backend.
inline bool cleanup(const rt::HalV2BackendApi& a, const Profile& p, Storage& s) noexcept {
    if (!valid_table(a) || !p.fixture || !p.owns_setup || !p.registered) return false;
    for (auto& token : s.tokens) if (token != 0 && a.unregister_buffer(a.instance, token) == Status::ok) token = 0;
    if (!zero(s.tokens) || p.registered(p.fixture) != 0) return false;
    if (s.setup_attempted && p.owns_setup(p.fixture)) {
        if (a.shutdown(a.instance) != Status::ok) return false;
    }
    if (p.owns_setup(p.fixture)) return false;
    s.setup_attempted = false; return true;
}
// Fixed scenario list, <= 128 checks / 160 HAL calls. No allocations or threads.
inline Report run(const rt::HalV2BackendApi& a, const Profile& p, Storage& s) noexcept {
    Report r;
    r.expect("table", valid_table(a));
    r.expect("profile hooks", p.fixture && p.inject && p.owns_setup && p.registered);
    if (r.failures) return r;
    r.expect("fresh storage", !s.setup_attempted && zero(s.tokens) &&
             !p.owns_setup(p.fixture) && p.registered(p.fixture) == 0);
    if (r.failures) return r;
    const auto body = [&]() noexcept {
        rt::HalV2Capabilities c;
        r.expect("discovery", a.get_capabilities(a.instance, &c) == Status::ok);
        r.expect("capability record", c.struct_size >= sizeof(c) && c.api_version == rt::hal_v2_api_version &&
            zero(c.reserved) && c.reserved0 == 0 && identifier(c.backend_id) && c.control_storage_bytes != 0 &&
            c.inline_payload_capacity >= rt::hal_v2_inline_payload_capacity &&
            c.buffer_ref_capacity >= rt::hal_v2_buffer_ref_capacity);
        r.expect("copy profile capacities", c.max_in_flight == 1 && c.max_registered_buffers == 2 &&
            c.max_buffer_bytes == 256 && c.supports_cancel == 1 && c.supports_reset == 1 && c.deterministic_mock == 1);
        if (r.failures) return;
        rt::HalV2InitializeConfig config; config.requested_in_flight = 1; config.requested_registered_buffers = 2;
        auto bad = config; bad.reserved[0] = 1;
        r.expect("invalid init", a.initialize(a.instance, &bad) == Status::invalid_argument && !p.owns_setup(p.fixture));
        p.inject(p.fixture, Fault::initialize); s.setup_attempted = true;
        r.expect("partial start", a.initialize(a.instance, &config) == Status::error && p.owns_setup(p.fixture));
        p.inject(p.fixture, Fault::shutdown);
        r.expect("partial cleanup retained", !cleanup(a, p, s) && p.owns_setup(p.fixture));
        r.expect("partial cleanup retry", cleanup(a, p, s));
        if (r.failures) return;
        // Negotiated registry capacity must be observed even below the maximum.
        config.requested_registered_buffers = 1; s.setup_attempted = true;
        r.expect("negotiated init", a.initialize(a.instance, &config) == Status::ok);
        auto src = registration(s.source.data()); auto dst = registration(s.destination.data());
        r.expect("negotiated register", a.register_buffer(a.instance, &src, &s.tokens[0]) == Status::ok && s.tokens[0] != 0);
        r.expect("negotiated limit", a.register_buffer(a.instance, &dst, &s.tokens[1]) == Status::resource_exhausted && s.tokens[1] == 0);
        const auto stale = s.tokens[0];
        r.expect("negotiated cleanup", cleanup(a, p, s));
        if (r.failures) return;
        config.requested_registered_buffers = 2; s.setup_attempted = true;
        r.expect("initialize", a.initialize(a.instance, &config) == Status::ok);
        p.inject(p.fixture, Fault::registration);
        r.expect("failed registration atomic", a.register_buffer(a.instance, &src, &s.tokens[0]) == Status::error &&
            s.tokens[0] == 0 && p.registered(p.fixture) == 0);
        r.expect("register source", a.register_buffer(a.instance, &src, &s.tokens[0]) == Status::ok && s.tokens[0] != 0 && s.tokens[0] != stale);
        r.expect("register destination", a.register_buffer(a.instance, &dst, &s.tokens[1]) == Status::ok &&
            s.tokens[1] != 0 && s.tokens[1] != s.tokens[0]);
        r.expect("registry full", a.register_buffer(a.instance, &dst, &s.tokens[2]) == Status::resource_exhausted && s.tokens[2] == 0 && p.registered(p.fixture) == 2);
        if (r.failures) return;
        for (std::size_t i = 0; i < s.source.size(); ++i) s.source[i] = static_cast<std::byte>(i * 7 + 3);
        auto command = submission(s, p.opcode);
        auto invalid = command; invalid.buffers[0].buffer_token = stale;
        r.expect("stale token", a.submit(a.instance, &invalid) == Status::invalid_argument);
        invalid = command; invalid.buffers[1].offset = UINT64_MAX;
        r.expect("overflow range", a.submit(a.instance, &invalid) == Status::invalid_argument);
        invalid = command; invalid.buffers[0].access = RTFW_DEVICE_ACCESS_WRITE;
        r.expect("access", a.submit(a.instance, &invalid) == Status::invalid_argument);
        invalid = command; invalid.reserved[0] = 1;
        r.expect("reserved input", a.submit(a.instance, &invalid) == Status::invalid_argument);
        r.expect("submit", a.submit(a.instance, &command) == Status::ok);
        r.expect("queue saturation", a.submit(a.instance, &command) == Status::queue_full);
        r.expect("pending buffer retained", a.unregister_buffer(a.instance, s.tokens[0]) == Status::invalid_state && p.registered(p.fixture) == 2);
        r.expect("pending reset rejected", a.reset(a.instance) == Status::invalid_state);
        r.expect("pending shutdown rejected", a.shutdown(a.instance) == Status::invalid_state && p.owns_setup(p.fixture));
        // Two output slots physically allocated: defective count can be diagnosed safely.
        std::array<rt::HalV2Completion, 2> out{}; std::uint64_t count = 0;
        r.expect("poll", a.poll(a.instance, out.data(), 1, &count) == Status::ok);
        r.expect("completion record", count == 1 && out[0].struct_size >= sizeof(out[0]) && zero(out[0].reserved) &&
            out[0].submission_id == 41 && out[0].status == 0 && out[0].value == 32);
        r.expect("copy contents", s.destination == s.source);
        r.expect("exactly once", a.poll(a.instance, out.data(), 1, &count) == Status::ok && count == 0);
        rt::HalV2Health h;
        r.expect("health conservation", a.get_health(a.instance, &h) == Status::ok && h.struct_size >= sizeof(h) &&
            zero(h.reserved) && h.reserved0 == 0 && h.outstanding == 0 && h.submissions == 1 &&
            h.completions == 1 && h.queue_rejections == 1 && h.generation == 2);
        if (r.failures) return;
        s.destination.fill(std::byte{0}); command.submission_id = 42;
        r.expect("cancel submit", a.submit(a.instance, &command) == Status::ok);
        r.expect("wrong cancel identity", a.cancel(a.instance, 43) == Status::invalid_argument);
        r.expect("cancel", a.cancel(a.instance, 42) == Status::ok);
        r.expect("cancel retired", a.poll(a.instance, out.data(), 1, &count) == Status::ok && count == 0 &&
            std::all_of(s.destination.begin(), s.destination.end(), [](auto x) { return x == std::byte{0}; }));
        constexpr std::array faults{Fault::completion_error, Fault::completion_timeout, Fault::completion_loss};
        constexpr std::array statuses{Status::error, Status::timeout, Status::lost};
        for (std::size_t i = 0; i < faults.size(); ++i) {
            command.submission_id = 50 + i; p.inject(p.fixture, faults[i]);
            r.expect("fault submit", a.submit(a.instance, &command) == Status::ok);
            r.expect("fault completion", a.poll(a.instance, out.data(), 1, &count) == Status::ok && count == 1 &&
                out[0].submission_id == command.submission_id && out[0].status == static_cast<std::int32_t>(statuses[i]) && out[0].value == 0);
            r.expect("fault no write", std::all_of(s.destination.begin(), s.destination.end(), [](auto x) { return x == std::byte{0}; }));
            r.expect("fault health", a.get_health(a.instance, &h) == Status::ok && h.outstanding == 0 &&
                h.last_status == static_cast<std::int32_t>(statuses[i]) && h.state == static_cast<std::uint32_t>(
                    i == 2 ? rt::HalV2HealthState::lost : rt::HalV2HealthState::reset_required));
            r.expect("reset required", a.submit(a.instance, &command) == Status::reset_required);
            const auto generation = h.generation;
            r.expect("reset", a.reset(a.instance) == Status::ok);
            r.expect("reset health", a.get_health(a.instance, &h) == Status::ok && h.generation == generation + 1 &&
                h.state == static_cast<std::uint32_t>(rt::HalV2HealthState::healthy) && h.last_status == 0);
        }
        r.expect("fault accounting", h.errors == 1 && h.timeouts == 1 && h.losses == 1 && h.cancellations == 1 &&
            h.resets == 3 && h.submissions == 5 && h.completions == 4);
        command.submission_id = 60;
        r.expect("recovery submit", a.submit(a.instance, &command) == Status::ok);
        r.expect("recovery completion", a.poll(a.instance, out.data(), 1, &count) == Status::ok && count == 1 &&
            out[0].submission_id == 60 && out[0].status == 0 && s.destination == s.source);
        p.inject(p.fixture, Fault::unregister);
        r.expect("failed unregister retains", a.unregister_buffer(a.instance, s.tokens[0]) == Status::error && p.registered(p.fixture) == 2);
        const auto retired = a.unregister_buffer(a.instance, s.tokens[0]);
        r.expect("unregister retry", retired == Status::ok);
        if (retired == Status::ok) s.tokens[0] = 0;
        p.inject(p.fixture, Fault::shutdown);
        r.expect("failed shutdown retains", !cleanup(a, p, s) && p.owns_setup(p.fixture));
        r.expect("shutdown retry", cleanup(a, p, s));
    };
    body();
    r.cleanup_complete = cleanup(a, p, s);
    r.expect("final ownership", r.cleanup_complete);
    return r;
}
} // namespace backend_kit
