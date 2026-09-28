#pragma once
#include "common.hpp"
#include <rt/sdk.hpp>
namespace cil {
inline constexpr std::size_t steps = 8;
struct Plant {
    Endpoint endpoint;
    Record published{};
    Code status = Code::ok;
    std::uint64_t now = 0, ttl = 0, applied = 0;
    std::int64_t position = 0, effort = 0;
    std::array<std::int64_t, steps> history{};
    bool bootstrap = true;
    rt::CallbackResult receive(const rt::CallbackContext&) noexcept {
        if (bootstrap) return rt::CallbackResult::ok;
        if (!endpoint.region || load(endpoint.region->plants.state) != 0) {
            status = endpoint.region ? Code::full : Code::closed;
            effort = 0; return rt::CallbackResult::ok;
        }
        Record command;
        status = endpoint.receive(now, command, &published);
        if (status == Code::ok) effort = command.value;
        else effort = 0; // Explicit hold/no-apply policy; never replay a prior command.
        return rt::CallbackResult::ok;
    }
    rt::CallbackResult integrate(const rt::CallbackContext&) noexcept {
        if (!bootstrap && status == Code::ok) {
            if (applied >= steps || position + effort < -1'000'000 || position + effort > 1'000'000) status = Code::invalid;
            else { position += effort; history[applied++] = position; }
        }
        return rt::CallbackResult::ok;
    }
    rt::CallbackResult publish(const rt::CallbackContext&) noexcept {
        if (status != Code::ok) return rt::CallbackResult::ok;
        if (!now || !ttl || ttl > max_ttl_ns || now > UINT64_MAX - ttl) { status = Code::invalid; return rt::CallbackResult::ok; }
        const Record next{Kind::plant, endpoint.generation, endpoint.sent.next, now, now + ttl, 0, applied, position};
        status = endpoint.send(next);
        if (status == Code::ok) { published = next; bootstrap = false; }
        return rt::CallbackResult::ok;
    }
};
inline rt::Status configure(rt::Runtime& runtime, Plant& plant) {
    rt::RuntimeConfig c; c.callback_capacity = 3; c.worker_count = 2;
    c.executor_queue_capacity = 4; c.task_scratch_slots = 4;
    auto result = runtime.configure(c); if (result != rt::Status::ok) return result;
    rt::sdk::GraphBuilder graph(runtime); rt::PhaseHandle ingress, integrate, publish;
    result = graph.phase<&Plant::receive>("cil.receive", plant, ingress); if (result != rt::Status::ok) return result;
    result = graph.phase<&Plant::integrate>("cil.integrate", plant, integrate); if (result != rt::Status::ok) return result;
    result = graph.phase<&Plant::publish>("cil.publish", plant, publish); if (result != rt::Status::ok) return result;
    result = graph.depends_on(integrate, ingress); if (result != rt::Status::ok) return result;
    result = graph.depends_on(publish, integrate); if (result != rt::Status::ok) return result;
    return graph.finalize();
}
} // namespace cil
