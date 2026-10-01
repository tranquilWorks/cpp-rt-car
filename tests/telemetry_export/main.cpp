#include "telemetry.hpp"
#include <atomic>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace {
void need(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
void ok(rt::Status s) { need(s == rt::Status::ok, rt::status_message(s)); }
class Clock final : public rt::RuntimeClock {
public:
    std::uint64_t now_ns() noexcept override { return now_.fetch_add(1, std::memory_order_relaxed); }
private:
    std::atomic<std::uint64_t> now_{1000};
};
rt::CallbackResult callback(void*, const rt::CallbackContext&) { return rt::CallbackResult::ok; }
struct Owner {
    Clock clock;
    rt::Runtime runtime{clock};
    explicit Owner(std::size_t capacity = 8) {
        rt::RuntimeConfig c;
        c.trace_capacity = capacity;
        ok(runtime.configure(c));
        ok(runtime.register_callback({"test.telemetry", callback, nullptr}));
        ok(runtime.finalize()); ok(runtime.start());
    }
    ~Owner() { (void)runtime.stop(); }
    void frame(std::uint64_t n) { ok(runtime.step({n, std::chrono::milliseconds(1), {}})); }
};
rtfw_telemetry::Context context(const char* session = "test-owner") {
    return {session, "synthetic", 1000, 1700000000000000000ULL, 17};
}
class FailedStream final : public std::streambuf {
    std::streamsize xsputn(const char*, std::streamsize) override { return 0; }
    int_type overflow(int_type) override { return traits_type::eof(); }
};
void lifecycle() {
    Owner owner, foreign;
    rtfw_telemetry::Queue q(context(), 1);
    q.set_enabled(false);
    need(q.capture(owner.runtime) == rt::Status::invalid_state, "disabled capture");
    q.set_enabled(true); ok(q.capture(owner.runtime));
    need(q.capture(owner.runtime) == rt::Status::queue_full, "full capture");
    rtfw_telemetry::Snapshot first;
    need(!q.drain_one([&](const auto& s) { first = s; return false; }), "failed sink");
    FailedStream buffer; std::ostream failed(&buffer);
    need(!q.drain_one([&](const auto& s) { return rtfw_telemetry::write_json(s, failed); }), "failed stream");
    try { q.drain_one([](const auto&) -> bool { throw std::runtime_error("sink"); }); }
    catch (const std::runtime_error&) {}
    need(q.statistics().pending == 1 && q.statistics().sink_failures == 3, "failed writes retain snapshot");
    std::ostringstream output; output << std::hex;
    need(q.drain_one([&](const auto& s) {
        need(s.trace.next_sequence == first.trace.next_sequence && s.batch_sequence == 1, "retry identity");
        return rtfw_telemetry::write_json(s, output);
    }), "retry succeeds");
    need(output.str().find("1700000000000000000") != std::string::npos, "locale independent decimal");
    need(q.capture(foreign.runtime) != rt::Status::ok, "foreign Runtime rejected");
    for (std::uint64_t i = 0; i < 40; ++i) owner.frame(i);
    ok(q.capture(owner.runtime));
    need(q.drain_one([&](const auto& s) {
        need(s.batch_sequence == 2 && s.queue_full == 1, "accepted sequence/full count");
        need(s.metrics.samples[0].value == 40, "full/foreign capture did not consume interval cursor");
        need(s.trace.lost_events > 0 && s.trace.events_read == 8, "actual Runtime overwrite accounted");
        auto bad = s; bad.events[0].record_size = 63;
        need(!rtfw_telemetry::valid_snapshot(bad), "malformed record rejected");
        bad = s; bad.events[0].reserved1 = 1;
        need(!rtfw_telemetry::valid_snapshot(bad), "reserved bytes rejected");
        bad = s; bad.metrics.samples[1].id = rt::RuntimeMetricId::frames_started;
        need(!rtfw_telemetry::valid_snapshot(bad), "metric identity rejected");
        return true;
    }), "drain overwritten range");
    owner.frame(40); ok(q.capture(owner.runtime));
    q.close(); need(q.capture(owner.runtime) == rt::Status::invalid_state, "closed capture");
    need(q.statistics().pending == 1, "close keeps pending");
    q.close(true); q.close(true);
    const auto stats = q.statistics();
    need(stats.pending == 0 && stats.closed && stats.discarded_batches == 1 && stats.discarded_events == 4,
         "discard accounting/idempotence");
    need(stats.disabled == 1 && stats.full == 1 && stats.capture_failures == 1 && stats.drained == 2,
         "independent counters");
}
void bounds() {
    for (const auto capacity : {std::size_t{0}, std::size_t{65}}) {
        bool rejected = false;
        try { rtfw_telemetry::Queue q(context(), capacity); }
        catch (const std::invalid_argument&) { rejected = true; }
        need(rejected, "queue capacity bounds");
    }
    auto c = context(); c.session = "bad\"identity";
    need(!rtfw_telemetry::valid_context(c), "invalid session");
    std::uint64_t mapped = 0;
    c = {"x", "y", 100, 10, 0};
    need(!rtfw_telemetry::map_timestamp(c, 1, mapped), "timestamp underflow");
    c = {"x", "y", 1, std::numeric_limits<std::uint64_t>::max(), 0};
    need(!rtfw_telemetry::map_timestamp(c, 2, mapped), "timestamp overflow");
    Owner owner(512);
    rtfw_telemetry::Queue q(context(), 2);
    for (std::uint64_t i = 0; i < 100; ++i) owner.frame(i);
    ok(q.capture(owner.runtime));
    std::uint64_t next = 0;
    need(q.drain_one([&](const auto& s) {
        need(s.trace.events_read == 256 && s.trace.remaining_sequence_count > 0, "bounded partial read");
        next = s.trace.next_sequence;
        return true;
    }), "first partial read");
    ok(q.capture(owner.runtime));
    need(q.drain_one([&](const auto& s) {
        need(s.trace.events_read > 0 && s.events[0].sequence == next && s.trace.remaining_sequence_count == 0,
             "partial read resumes without duplication");
        return true;
    }), "second partial read");
}
void concurrent(const char* session) {
    Owner owner(64);
    rtfw_telemetry::Queue q(context(session), 2);
    std::atomic<bool> done{false};
    std::atomic<unsigned> consumed{0};
    std::atomic<bool> valid{true};
    std::thread consumer([&] {
        while (!done.load() || q.statistics().pending != 0) {
            q.drain_one([&](const auto& s) {
                if (!rtfw_telemetry::valid_snapshot(s) || s.context.session != session) valid.store(false);
                consumed.fetch_add(1); return true;
            });
            std::this_thread::yield();
        }
    });
    for (std::uint64_t n = 0; n < 100; ++n) {
        owner.frame(n);
        rt::Status result;
        do { result = q.capture(owner.runtime); std::this_thread::yield(); } while (result == rt::Status::queue_full);
        if (result != rt::Status::ok) valid.store(false);
    }
    q.close(); done.store(true); consumer.join();
    need(valid.load() && consumed.load() == 100, "concurrent consumer/owner isolation");
}
}
int main() {
    try {
        lifecycle(); bounds();
        std::exception_ptr failure;
        std::thread other([&] { try { concurrent("second-owner"); } catch (...) { failure = std::current_exception(); } });
        concurrent("first-owner"); other.join();
        if (failure) std::rethrow_exception(failure);
        std::cout << "PASS bounded queue, cursor conservation, loss, malformed records, concurrent owners and shutdown\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
