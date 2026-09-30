#include "../../samples/host_adapter/host.hpp"
#include "../cuda_physics/allocation_guard.hpp"
#include <iostream>
#include <memory>
#define CHECK(x) do { if (!(x)) { std::cerr << __func__ << ':' << __LINE__ << " failed: " #x << '\n'; return false; } } while (false)
using rt::Status;
using namespace host_example;
struct Probe {
    std::array<std::atomic<unsigned>, 4096> hits{};
    std::atomic<bool> bad{false};
    static void execute(void* a, void* b, std::uint64_t token, std::uint32_t worker) noexcept {
        auto& p = *static_cast<Probe*>(a);
        if (b != a || token >= p.hits.size() || worker >= Jobs::workers) { p.bad.store(true); return; }
        p.hits[static_cast<std::size_t>(token)].fetch_add(1);
    }
    rt::HostExecutorJob job(std::uint64_t id) noexcept { return {&execute, this, this, id, nullptr, 0}; }
};
bool queue_contract() {
    Jobs q; Probe p;
    CHECK(Jobs::submit(&q, p.job(0)) == Status::invalid_state);
    CHECK(q.start(false) == Status::ok);
    auto job = p.job(0);
    for (std::uint64_t i = 0; i < Jobs::capacity; ++i) { job.completion_token = i; CHECK(Jobs::submit(&q, job) == Status::ok); }
    job.execute = nullptr; job.completion_token = 999; // Accepted records must already be copied.
    CHECK(Jobs::submit(&q, p.job(100)) == Status::queue_full && q.rejected.load() == 1);
    CHECK(q.close() == Status::invalid_state); // Retain all accepted jobs.
    CHECK(q.attach() == Status::ok && q.detach() == Status::invalid_state);
    CHECK(Jobs::submit(nullptr, p.job(0)) == Status::invalid_argument);
    CHECK(Jobs::submit(&q, {}) == Status::invalid_argument);
    bool foreign_help = true;
    std::thread foreign([&] { foreign_help = Jobs::help(&q); }); foreign.join(); CHECK(!foreign_help);
    for (unsigned i = 0; i < Jobs::capacity; ++i) CHECK(Jobs::help(&q));
    CHECK(!Jobs::help(&q) && q.pending.load() == 0 && q.accepted.load() == 32 && q.completed.load() == 32);
    for (std::size_t i = 0; i < Jobs::capacity; ++i) CHECK(p.hits[i].load() == 1);
    CHECK(p.hits[100].load() == 0 && !p.bad.load());
    CHECK(q.close() == Status::invalid_state && q.owners() == 1);
    CHECK(q.detach() == Status::ok && q.close() == Status::ok);
    CHECK(q.start(true, true) == Status::resource_exhausted); // First worker is joined on partial start.
    CHECK(q.close() == Status::ok);
    return true;
}
bool queue_concurrency() {
    Jobs q; Probe p; CHECK(q.start() == Status::ok);
    std::atomic<bool> failed{false};
    auto produce = [&](std::size_t begin) {
        std::size_t done = 0;
        for (std::size_t attempts = 0; attempts < 5'000'000 && done < 2048; ++attempts) {
            auto s = Jobs::submit(&q, p.job(begin + done));
            if (s == Status::ok) ++done;
            else if (s != Status::queue_full) { failed.store(true); break; }
            else std::this_thread::yield();
        }
        if (done != 2048) failed.store(true);
    };
    std::thread a(produce, 0), b(produce, 2048); a.join(); b.join();
    CHECK(q.quiesce() == Status::ok && q.close() == Status::ok);
    CHECK(!failed.load() && !p.bad.load() && q.accepted.load() == 4096 && q.completed.load() == 4096);
    for (const auto& n : p.hits) CHECK(n.load() == 1);
    return true;
}
bool memory_contract() {
    auto m = std::make_unique<Memory>(); auto table = m->table();
    rt::MemoryProviderAcquireRequest r; r.region = rt::memory_region_phase_scratch; r.logical_bytes = 128;
    r.required_alignment = 64; r.page_rounding = rt::PageRounding::none; r.huge_pages = rt::HugePagePreference::disabled;
    r.rollback = rt::RollbackIntent::release;
    rt::MemoryProviderAllocation out;
    auto bad = r; bad.logical_bytes = 65537; CHECK(table.acquire(m.get(), bad, out) == Status::resource_exhausted && !out.token);
    bad = r; bad.required_alignment = 3; CHECK(table.acquire(m.get(), bad, out) == Status::invalid_config);
    bad = r; bad.region = {}; CHECK(table.acquire(m.get(), bad, out) == Status::invalid_config);
    bad = r; bad.guard_bytes_before = 4096; CHECK(table.acquire(m.get(), bad, out) == Status::invalid_config);
    bad = r; bad.huge_pages = rt::HugePagePreference::prefer; CHECK(table.acquire(m.get(), bad, out) == Status::invalid_config);
    CHECK(table.acquire(m.get(), r, out) == Status::ok && m->live_count() == 1 && out.usable_bytes == 128);
    const auto allocation = out;
    CHECK(table.acquire(m.get(), r, out) == Status::invalid_state && !out.token);
    const auto policy = Memory::policy().memory_policies[0].policy;
    rt::MemoryProviderObservation observation;
    CHECK(table.apply(m.get(), allocation.token, policy, observation) == Status::ok);
    CHECK(table.observe(m.get(), allocation.token, policy, observation) == Status::ok && observation.independently_observed);
    CHECK(observation.resident_bytes <= 128 && !observation.locked_bytes && !observation.pinned_bytes);
    table.release(m.get(), allocation.token, rt::RollbackIntent::release); // Must not release an applied token.
    CHECK(m->violations == 1 && m->live_count() == 1);
    m->fail_rollback = 1;
    CHECK(table.rollback(m.get(), allocation.token, policy, observation) == Status::internal_error && m->live_count() == 1);
    CHECK(table.rollback(m.get(), allocation.token, policy, observation) == Status::ok);
    table.release(m.get(), allocation.token, rt::RollbackIntent::release);
    CHECK(m->live_count() == 0 && m->acquisitions == 1 && m->releases == 1);
    table.release(m.get(), allocation.token, rt::RollbackIntent::release); CHECK(m->violations == 2);
    return true;
}
bool runtime_modes(bool adapter, bool cooperative = false) {
    auto host = std::make_unique<Host>();
    if (adapter) CHECK(host->jobs.start(!cooperative) == Status::ok);
    Session a(host->clock, adapter ? &host->jobs : nullptr, host->memory[0], 1);
    Session b(host->clock, adapter ? &host->jobs : nullptr, host->memory[1], 3);
    CHECK(a.prepare() == Status::ok && b.prepare() == Status::ok);
    CHECK(a.telemetry.runtime_id != b.telemetry.runtime_id);
    if (adapter) CHECK(host->jobs.close() == Status::invalid_state && host->jobs.owners() == 2);
    bool okay = true;
    rtfw_physics_allocation::begin();
    for (unsigned frame = 0; frame < 16; ++frame) okay = okay && a.step() == Status::ok && b.step() == Status::ok;
    const auto allocations = rtfw_physics_allocation::end();
    CHECK(okay && allocations == 0 && a.world.matches(16) && b.world.matches(16));
    CHECK(a.telemetry.begins == 16 && a.telemetry.ends == 16 && a.telemetry.callbacks == 32 && a.telemetry.lost == 0);
    CHECK(b.telemetry.begins == 16 && b.telemetry.ends == 16 && b.telemetry.callbacks == 32 && b.telemetry.lost == 0);
    CHECK(a.telemetry.metrics(*a.runtime, 16) && b.telemetry.metrics(*b.runtime, 16));
    auto wrong = a.telemetry.cursor; rt::RuntimeTraceReadResult read; std::array<rt::RuntimeTraceEvent, 2> buffer;
    CHECK(b.runtime->read_trace(wrong, buffer, read) != Status::ok);
    CHECK(a.close() == Status::ok && host->memory[0].live_count() == 0 && host->memory[1].live_count() == 3);
    CHECK(b.step() == Status::ok && b.world.matches(17)); // One owner can detach without affecting the other.
    CHECK(b.close() == Status::ok && host->jobs.close() == Status::ok);
    CHECK(host->jobs.pending.load() == 0 && host->jobs.accepted.load() == host->jobs.completed.load());
    CHECK(!adapter || (host->jobs.accepted.load() > 0 && (!cooperative || host->jobs.helped.load() > 0)));
    CHECK(host->memory[0].acquisitions == 3 && host->memory[0].releases == 3 && host->memory[1].releases == 3);
    return true;
}
bool failures() {
    for (unsigned fault = 0; fault < 5; ++fault) {
        auto host = std::make_unique<Host>(); CHECK(host->jobs.start() == Status::ok);
        if (fault == 0) host->memory[0].fail_acquire = 2;
        if (fault == 1) host->memory[0].fail_apply = 2;
        if (fault == 2) host->memory[0].fail_observe = 2;
        Session s(host->clock, &host->jobs, host->memory[0], 1);
        CHECK(s.prepare(fault == 4 ? 65536 : 256, fault == 3) != Status::ok);
        CHECK(s.close() == Status::ok && host->memory[0].live_count() == 0 && host->jobs.owners() == 0);
        CHECK(host->memory[0].acquisitions == host->memory[0].releases && host->memory[0].violations == 0);
        CHECK(host->jobs.close() == Status::ok);
    }
    auto host = std::make_unique<Host>(); CHECK(host->jobs.start() == Status::ok);
    Session s(host->clock, &host->jobs, host->memory[0], 1);
    CHECK(s.prepare() == Status::ok);
    host->clock.fail.store(true); CHECK(s.step() == Status::clock_failure && s.frames == 0 && s.world.checksum == 0);
    host->clock.fail.store(false); CHECK(s.step() == Status::ok);
    s.world.fail_callback = true; CHECK(s.step() == Status::callback_failed && s.frames == 1);
    host->memory[0].fail_rollback = 1;
    CHECK(s.close() == Status::internal_error && host->memory[0].live_count() > 0 && host->jobs.owners() == 1 && s.runtime.has_value());
    CHECK(host->jobs.close() == Status::invalid_state && s.step() == Status::invalid_state);
    CHECK(s.close() == Status::ok && !s.runtime.has_value() && host->memory[0].live_count() == 0);
    CHECK(s.close() == Status::ok && host->memory[0].releases == 3 && host->jobs.close() == Status::ok);
    return true;
}
bool telemetry_loss() {
    auto host = std::make_unique<Host>(); Session s(host->clock, nullptr, host->memory[0], 1);
    CHECK(s.prepare(8) == Status::ok);
    const auto before = s.telemetry.events;
    for (unsigned i = 0; i < 4; ++i) CHECK(s.step(false) == Status::ok);
    CHECK(s.telemetry.drain(*s.runtime, true) == Status::ok);
    CHECK(s.telemetry.events - before == 8 && s.telemetry.lost == 16); // Four frames * six events, eight retained.
    CHECK(s.close() == Status::ok);
    return true;
}
bool repeated_and_isolated() {
    auto host = std::make_unique<Host>();
    for (unsigned round = 0; round < 4; ++round) {
        CHECK(host->jobs.start() == Status::ok);
        Session s(host->clock, &host->jobs, host->memory[0], 2);
        CHECK(s.prepare() == Status::ok && s.step() == Status::ok && s.world.matches(1));
        CHECK(s.close() == Status::ok && host->jobs.close() == Status::ok);
    }
    CHECK(host->memory[0].acquisitions == 12 && host->memory[0].releases == 12);
    std::atomic<unsigned> ready{0}; std::atomic<bool> good{true};
    auto run = [&](std::int64_t multiplier) {
        auto h = std::make_unique<Host>();
        if (h->jobs.start() != Status::ok) { good.store(false); ready.fetch_add(1); return; }
        Session s(h->clock, &h->jobs, h->memory[0], multiplier);
        bool okay = s.prepare() == Status::ok;
        ready.fetch_add(1);
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (ready.load() != 2 && std::chrono::steady_clock::now() < end) std::this_thread::yield();
        okay = okay && ready.load() == 2;
        for (unsigned f = 0; f < 8 && okay; ++f) okay = s.step() == Status::ok && s.world.matches(f + 1);
        const auto stopped = s.close();
        okay = okay && stopped == Status::ok && h->memory[0].live_count() == 0 && h->jobs.close() == Status::ok;
        if (!okay) good.store(false);
    };
    std::thread a(run, 2), b(run, 5); a.join(); b.join(); CHECK(good.load());
    return true;
}
int main() {
    if (!queue_contract() || !queue_concurrency() || !memory_contract() || !runtime_modes(false) ||
        !runtime_modes(true) || !runtime_modes(true, true) || !failures() || !telemetry_loss() || !repeated_and_isolated()) return 1;
    std::cout << "host jobs, memory, clock, native/adapter Runtime, telemetry, ownership and allocation passed\n";
}
