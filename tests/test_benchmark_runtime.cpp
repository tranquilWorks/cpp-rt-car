#include <gtest/gtest.h>
#include "runtime_provider.hpp"
#include <atomic>
#include <thread>

namespace b=rtfw::benchmark;
namespace {
bool tick(void* opaque,std::uint64_t& value){auto& n=*static_cast<std::uint64_t*>(opaque);value=n;n+=10;return true;}
}
TEST(BenchmarkRuntime, MetadataAndLifecycle) {
    b::runtime::Provider provider;b::Runner runner;b::ProviderHandle handle;
    ASSERT_EQ(runner.register_provider(provider.table(),handle),b::Status::ok);
    ASSERT_FALSE(provider.prepared());
    ASSERT_LE(b::runtime::cases().size(),96u);
    for(const auto& c:b::runtime::cases()) {
        b::Descriptor d;ASSERT_EQ(runner.describe("rtfw.runtime",c.id,d),b::Status::ok);
        EXPECT_EQ(d.case_id,c.id);EXPECT_EQ(d.configuration,c.scope);
        EXPECT_EQ(d.parameters.size(),5u);EXPECT_LE(d.counters.size(),16u);
        EXPECT_FALSE(provider.prepared());
    }
    EXPECT_EQ(provider.prepare("unknown"),b::Status::not_found);
    EXPECT_FALSE(provider.prepared());
    ASSERT_EQ(provider.prepare("rate-dispatch-8-d1-s1"),b::Status::ok);
    EXPECT_EQ(provider.prepare("rate-dispatch-8-d1-s1"),b::Status::busy);
    EXPECT_EQ(provider.finish(),b::Status::ok);EXPECT_EQ(provider.finish(),b::Status::ok);
    EXPECT_EQ(runner.unregister_provider(handle),b::Status::ok);
}
TEST(BenchmarkRuntime, EveryCasePreservesWarmupAndMeasuredBoundaries) {
    for(const auto& c:b::runtime::cases()) {
        SCOPED_TRACE(c.id);
        b::runtime::Provider provider;b::Runner runner;b::ProviderHandle handle;
        ASSERT_EQ(runner.register_provider(provider.table(),handle),b::Status::ok);
        ASSERT_EQ(provider.prepare(c.id),b::Status::ok);
        std::uint64_t time=0;auto clock=b::steady_clock();clock.kind=b::ClockKind::fake;clock.user=&time;clock.read_ns=tick;
        const auto result=runner.run("rtfw.runtime",c.id,clock,b::Identity{});
        ASSERT_EQ(provider.finish(),b::Status::ok);
        EXPECT_EQ(result.status,b::Status::ok);
        if(result.status!=b::Status::ok) continue;
        ASSERT_EQ(result.samples.size(),5u);
        EXPECT_EQ(provider.invocations(),7u);
        for(const auto& sample:result.samples) {EXPECT_TRUE(sample.observation.correct);EXPECT_EQ(sample.end_ns-sample.start_ns,10u);}
        EXPECT_EQ(runner.unregister_provider(handle),b::Status::ok);
    }
}
TEST(BenchmarkRuntime, ConcurrentInstancesOwnIndependentState) {
    std::atomic<unsigned> failures{};
    const auto run=[&] {
        try {
            b::runtime::Provider p;
            if(p.prepare("rate-dispatch-64-d4-s4")!=b::Status::ok){++failures;return;}
            auto table=p.table();b::Observation out;out.counters.reserve(b::max_counters);
            for(std::uint64_t i=0;i<7;++i) {
                if(table.invoke(table.user,"rate-dispatch-64-d4-s4",i,out)!=b::Status::ok ||
                   !out.correct || out.counters[0]!=1024) ++failures;
            }
            if(p.finish()!=b::Status::ok) ++failures;
        } catch(...) {++failures;}
    };
    std::thread first(run),second(run);first.join();second.join();EXPECT_EQ(failures,0u);
}
