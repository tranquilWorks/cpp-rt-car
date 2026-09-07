#include "cpu_provider.hpp"
#include <gtest/gtest.h>
#include <algorithm>
#include <set>
#include <thread>

namespace b = rtfw::benchmark;
namespace cpu = b::cpu;
namespace {
bool ticks(void* p, std::uint64_t& t) { auto& v = *static_cast<std::uint64_t*>(p); t = v; v += 10; return true; }
b::Result run(cpu::Provider& p, std::string_view id, b::ClockKind kind = b::ClockKind::fake) {
    b::Runner runner; b::ProviderHandle h;
    if (runner.register_provider(p.table(), h) != b::Status::ok) return {};
    const auto prep = p.prepare(id);
    if (prep != b::Status::ok && prep != b::Status::not_run) return {};
    std::uint64_t t = 0; auto clock = b::steady_clock();
    if (kind == b::ClockKind::fake) { clock.kind = kind; clock.user = &t; clock.read_ns = ticks; }
    return runner.run("rtfw.cpu", id, clock, {});
}
std::uint64_t counter(const b::Result& r, std::size_t sample, std::string_view name) {
    const auto found = std::find_if(r.descriptor.counters.begin(), r.descriptor.counters.end(),
                                  [&](const auto& c) { return c.name == name; });
    if (found == r.descriptor.counters.end()) return b::max_integer;
    return r.samples.at(sample).observation.counters.at(static_cast<std::size_t>(found - r.descriptor.counters.begin()));
}
}
TEST(BenchmarkCpu, MetadataIsPureAndPreparationIsExplicit) {
    cpu::Provider p; b::Runner r; b::ProviderHandle h;
    ASSERT_EQ(r.register_provider(p.table(), h), b::Status::ok);
    EXPECT_FALSE(p.prepared()); EXPECT_EQ(p.invocations(), 0U);
    EXPECT_EQ(r.list().size(), cpu::cases().size());
    std::set<std::string> names;
    for (const auto& c : cpu::cases()) {
        b::Descriptor d; ASSERT_EQ(r.describe("rtfw.cpu", c.id, d), b::Status::ok);
        EXPECT_EQ(d.warmup, 2U); EXPECT_EQ(d.repetitions, 5U);
        EXPECT_TRUE(names.insert(d.case_id).second); EXPECT_FALSE(p.prepared());
    }
    EXPECT_EQ(p.prepare("missing"), b::Status::not_found); EXPECT_FALSE(p.prepared());
    b::Observation o; auto table = p.table();
    EXPECT_EQ(table.invoke(table.user, "host-adapter-64", 0, o), b::Status::provider_error);
    ASSERT_EQ(p.prepare("host-adapter-64"), b::Status::ok);
    EXPECT_EQ(p.prepare("queue-2"), b::Status::busy);
    EXPECT_EQ(table.invoke(table.user, "queue-2", 0, o), b::Status::provider_error);
    EXPECT_EQ(table.invoke(table.user, "host-adapter-64", 1, o), b::Status::provider_error);
    EXPECT_EQ(p.invocations(), 0U);
    EXPECT_EQ(table.invoke(table.user, "host-adapter-64", 0, o), b::Status::ok);
    EXPECT_EQ(p.invocations(), 1U);
    EXPECT_EQ(p.finish(), b::Status::ok); EXPECT_EQ(p.finish(), b::Status::ok);
}
TEST(BenchmarkCpu, EveryCaseExecutesItsStructuralContract) {
    for (const auto& c : cpu::cases()) {
        SCOPED_TRACE(c.id); cpu::Provider p;
        const auto r = run(p, c.id);
        if (c.memory == cpu::Memory::native && r.status == b::Status::not_run) {
            EXPECT_TRUE(r.samples.empty()); EXPECT_EQ(r.warmup_completed, 0U);
        } else {
            ASSERT_EQ(r.status, b::Status::ok);
            EXPECT_EQ(r.samples.size(), 5U); EXPECT_EQ(r.warmup_completed, 2U);
            EXPECT_EQ(p.invocations(), 7U);
            for (const auto& s : r.samples) EXPECT_EQ(s.end_ns - s.start_ns, 10U);
        }
        ASSERT_EQ(p.finish(), b::Status::ok);
        EXPECT_FALSE(p.prepared()); EXPECT_NO_THROW((void)b::encode(r));
    }
}
TEST(BenchmarkCpu, RejectionIsAnAcceptedPrefixAndThenRecovers) {
    for (const auto* id : {"queue-2", "queue-8"}) {
        cpu::Provider p; const auto r = run(p, id); ASSERT_EQ(r.status, b::Status::ok);
        const auto expected = std::string_view(id) == "queue-2" ? 2U : 8U;
        for (std::size_t i = 0; i < 5; ++i) {
            EXPECT_EQ(counter(r, i, "operations"), expected);
            EXPECT_EQ(counter(r, i, "rejected"), 1U);
            EXPECT_EQ(counter(r, i, "submitted"), expected + 2U);
        }
        EXPECT_EQ(p.finish(), b::Status::ok);
    }
}
TEST(BenchmarkCpu, MemoryFailureRollbackAndInactivePathsAreObserved) {
    for (const auto* id : {"memory-simulated-64", "memory-acquire_failure-64", "memory-apply_failure-64",
                           "memory-rollback_retry-64", "memory-inactive"}) {
        SCOPED_TRACE(id); cpu::Provider p; const auto r = run(p, id); ASSERT_EQ(r.status, b::Status::ok);
        for (std::size_t i = 0; i < 5; ++i) {
            EXPECT_EQ(counter(r,i,"acquired"), counter(r,i,"released"));
            EXPECT_EQ(counter(r,i,"applied"), counter(r,i,"rolled_back"));
            if (std::string_view(id) == "memory-inactive") { EXPECT_EQ(counter(r,i,"acquired"), 0U); }
            else { EXPECT_GT(counter(r,i,"acquired"), 0U); }
            if (std::string_view(id).find("failure") != std::string_view::npos) {
                EXPECT_EQ(counter(r,i,"operations"), 0U);
            }
        }
        EXPECT_EQ(p.finish(), b::Status::ok);
    }
}
TEST(BenchmarkCpu, NestedReductionTraceAndScalingHaveIndependentOracles) {
    for (const auto* id : {"nested-reduce-1", "nested-reduce-2", "host-adapter-64", "trace-0", "trace-256"}) {
        cpu::Provider p; const auto r = run(p,id); ASSERT_EQ(r.status,b::Status::ok);
        const auto traced = std::string_view(id).starts_with("trace-");
        for (const auto& s : r.samples) EXPECT_EQ(s.observation.checksum, traced ? 24448U : 6112U);
        EXPECT_EQ(p.finish(),b::Status::ok);
    }
    cpu::Provider p; const auto large=run(p,"entities-4096"); ASSERT_EQ(large.status,b::Status::ok);
    EXPECT_EQ(counter(large,0,"operations"),4096U);
    EXPECT_EQ(large.samples[0].observation.checksum,25163776U);
    EXPECT_EQ(p.finish(),b::Status::ok);
}
TEST(BenchmarkCpu, RepeatedRunsAndConcurrentOwnersStayIsolated) {
    cpu::Provider p; const auto a=run(p,"host-adapter-64"); ASSERT_EQ(a.status,b::Status::ok);
    ASSERT_EQ(p.finish(),b::Status::ok);
    const auto z=run(p,"host-adapter-64"); ASSERT_EQ(z.status,b::Status::ok);
    ASSERT_EQ(p.finish(),b::Status::ok);
    EXPECT_EQ(b::encode(a).raw,b::encode(z).raw);
    b::Result results[2];
    std::thread one([&]{cpu::Provider local;results[0]=run(local,"multiple-64");EXPECT_EQ(local.finish(),b::Status::ok);});
    std::thread two([&]{cpu::Provider local;results[1]=run(local,"multiple-1024");EXPECT_EQ(local.finish(),b::Status::ok);});
    one.join();two.join();
    EXPECT_EQ(results[0].status,b::Status::ok);EXPECT_EQ(results[1].status,b::Status::ok);
}
TEST(BenchmarkCpu, ClockFailureStillAllowsCheckedCleanupAndRerun) {
    cpu::Provider p; b::Runner r; b::ProviderHandle h;
    ASSERT_EQ(r.register_provider(p.table(),h),b::Status::ok);
    ASSERT_EQ(p.prepare("workers-throughput-4"),b::Status::ok);
    auto clock=b::steady_clock();clock.read_ns=[](void*,std::uint64_t&){return false;};
    EXPECT_EQ(r.run("rtfw.cpu","workers-throughput-4",clock,{}).status,b::Status::clock_error);
    EXPECT_EQ(p.finish(),b::Status::ok);
    EXPECT_EQ(run(p,"host-adapter-64",b::ClockKind::steady).status,b::Status::ok);
    EXPECT_EQ(p.finish(),b::Status::ok);
}
