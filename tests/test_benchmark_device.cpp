#include <gtest/gtest.h>
#include "device_provider.hpp"
#include "device_cases/fake_cuda.hpp"

namespace b = rtfw::benchmark;
namespace {
bool device_tick(void* user, std::uint64_t& value) {
    auto& time = *static_cast<std::uint64_t*>(user);
    value = time; time += 10; return true;
}
}
TEST(BenchmarkDevice, CatalogExecutesOrReportsMissingRealSession) {
    for (const auto& spec : b::device::cases()) {
        SCOPED_TRACE(spec.id);
        b::device::Provider provider;
        b::Runner runner; b::ProviderHandle handle;
        ASSERT_EQ(runner.register_provider(provider.table(),handle),b::Status::ok);
        ASSERT_EQ(provider.prepare(spec.id),spec.real ? b::Status::not_run : b::Status::ok);
        std::uint64_t time=0; auto clock=b::steady_clock();
        clock.kind=b::ClockKind::fake; clock.user=&time; clock.read_ns=device_tick;
        const auto result=runner.run("rtfw.device",spec.id,clock,b::Identity{});
        EXPECT_EQ(result.status,spec.real ? b::Status::not_run : b::Status::ok);
        EXPECT_EQ(result.samples.size(),spec.real ? 0u : 5u);
        EXPECT_EQ(result.warmup_completed,spec.real ? 0u : 2u);
        ASSERT_EQ(provider.finish(),b::Status::ok);
        EXPECT_EQ(provider.finish(),b::Status::ok);
    }
}
TEST(BenchmarkDevice, SuppliedSessionActuallyExecutesItsDriverAndKernel) {
    b::device::detail::FakeCudaDriver driver;
    driver.complete_on_record.store(true);
    b::device::CudaSession session{driver.api(),driver.context,0x51u,driver.add_one_function};
    b::device::Provider provider(&session);
    ASSERT_EQ(provider.prepare("real-cuda-kernel-4096"),b::Status::ok);
    auto api=provider.table(); b::Observation out; out.counters.reserve(b::max_counters);
    for(std::uint64_t n=0;n<7;++n) {
        ASSERT_EQ(api.invoke(api.user,"real-cuda-kernel-4096",n,out),b::Status::ok);
        EXPECT_EQ(out.counters[0],3u); EXPECT_EQ(out.counters[1],3u);
        EXPECT_EQ(out.counters[4],8192u); EXPECT_EQ(out.counters[5],1u);
        EXPECT_EQ(out.counters[6],1024u);
    }
    EXPECT_EQ(driver.launches.load(),7u);
    ASSERT_EQ(provider.finish(),b::Status::ok);
    EXPECT_EQ(driver.allocations.load(),driver.frees.load());
    EXPECT_EQ(driver.host_registrations.load(),driver.host_unregistrations.load());
}
TEST(BenchmarkDevice, DepthIsObservedAndRejectedWorkIsSeparate) {
    b::device::Provider provider;
    ASSERT_EQ(provider.prepare("cuda-depth-4"),b::Status::ok);
    auto api=provider.table(); b::Observation out;
    ASSERT_EQ(api.invoke(api.user,"cuda-depth-4",0,out),b::Status::ok);
    EXPECT_EQ(out.counters[0],5u); EXPECT_EQ(out.counters[1],5u);
    EXPECT_EQ(out.counters[3],1u); EXPECT_EQ(out.counters[7],4u);
    EXPECT_EQ(provider.finish(),b::Status::ok);
}
TEST(BenchmarkDevice, MalformedSuppliedDriverIsAnErrorAndCanBeCleaned) {
    b::device::CudaSession session;
    session.context=1; session.stream=1;
    b::device::Provider provider(&session);
    EXPECT_EQ(provider.prepare("real-cuda-roundtrip-64"),b::Status::provider_error);
    EXPECT_EQ(provider.finish(),b::Status::ok);
}
