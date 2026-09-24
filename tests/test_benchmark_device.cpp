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

#include "device_cases/fake_xdma.hpp"
#include <future>
TEST(BenchmarkDevice, SuppliedXdmaSessionUsesConfirmedOffsetAndActualDriver) {
    auto driver=std::make_unique<b::device::detail::FakeXdmaDriver>();
    b::device::XdmaSession session;
    session.driver=driver->api(); session.device_offset=4096; session.confirmed_window_bytes=4096;
    session.config.queue_capacity=1; session.config.buffer_capacity=1; session.config.worker_count=1;
    session.config.max_transfer_bytes=4096; session.config.max_buffer_bytes=4096;
    b::device::Provider provider(nullptr,&session);
    ASSERT_EQ(provider.prepare("real-xdma-roundtrip-4096"),b::Status::ok);
    auto api=provider.table(); b::Observation out;
    for(std::uint64_t n=0;n<7;++n) {
        ASSERT_EQ(api.invoke(api.user,"real-xdma-roundtrip-4096",n,out),b::Status::ok);
        EXPECT_EQ(out.counters[0],2u); EXPECT_EQ(out.counters[1],2u);
        EXPECT_EQ(out.counters[4],8192u); EXPECT_EQ(out.counters[6],4096u);
        for(std::size_t i=0;i<4096;++i) {
            ASSERT_EQ(driver->device[i],std::byte{0});
            ASSERT_EQ(driver->device[4096+i],static_cast<std::byte>((n*7+i*3)%251));
        }
    }
    EXPECT_EQ(driver->transfers.load(),14u);
    ASSERT_EQ(provider.finish(),b::Status::ok);
    EXPECT_FALSE(driver->initialized.load());
}
TEST(BenchmarkDevice, InvalidSuppliedXdmaResourcesAreErrorsAndDoNotAccessDriver) {
    auto driver=std::make_unique<b::device::detail::FakeXdmaDriver>();
    b::device::XdmaSession session;
    session.driver=driver->api(); session.confirmed_window_bytes=32;
    b::device::Provider provider(nullptr,&session);
    EXPECT_EQ(provider.prepare("real-xdma-roundtrip-64"),b::Status::provider_error);
    auto api=provider.table(); b::Observation out;
    EXPECT_EQ(api.invoke(api.user,"real-xdma-roundtrip-64",0,out),b::Status::provider_error);
    EXPECT_FALSE(driver->initialized.load());
    EXPECT_EQ(provider.finish(),b::Status::ok);
    session.confirmed_window_bytes=64; session.driver={};
    EXPECT_EQ(provider.prepare("real-xdma-roundtrip-64"),b::Status::provider_error);
    EXPECT_EQ(provider.finish(),b::Status::ok);
}
TEST(BenchmarkDevice, SuppliedGraphExecutesBoundStorageAndRejectsCorruptOutput) {
    auto driver=std::make_unique<b::device::detail::FakeCudaDriver>();
    std::array<std::int32_t,16> storage{};
    driver->complete_on_record.store(true); driver->graph_values=storage.data(); driver->graph_elements=storage.size();
    b::device::CudaSession session;
    session.driver=driver->api_v2(); session.context=driver->context; session.stream=0x51u;
    session.increment_graph=driver->graph; session.graph_buffer=reinterpret_cast<rt::CudaDeviceAddress>(storage.data()); session.graph_bytes=64;
    b::device::Provider provider(&session);
    ASSERT_EQ(provider.prepare("real-cuda-graph-64"),b::Status::ok);
    auto api=provider.table(); b::Observation out;
    ASSERT_EQ(api.invoke(api.user,"real-cuda-graph-64",0,out),b::Status::ok);
    EXPECT_EQ(driver->graph_launches.load(),1u);
    EXPECT_EQ(driver->allocations.load(),0u);
    EXPECT_EQ(out.checksum,376u);
    driver->graph_elements=1; // Deliberately incomplete caller graph must fail its output oracle.
    EXPECT_EQ(api.invoke(api.user,"real-cuda-graph-64",1,out),b::Status::invariant_failed);
    EXPECT_FALSE(out.correct);
    driver->graph_elements=16;
    EXPECT_EQ(api.invoke(api.user,"real-cuda-graph-64",1,out),b::Status::provider_error);
    EXPECT_EQ(provider.finish(),b::Status::ok);
    EXPECT_EQ(driver->frees.load(),0u);
    EXPECT_EQ(driver->host_registrations.load(),driver->host_unregistrations.load());
}
TEST(BenchmarkDevice, ConcurrentOwnersDoNotShareDriverOrOrdinalState) {
    auto exercise=[](const char* id) {
        b::device::Provider provider;
        if(provider.prepare(id)!=b::Status::ok) return false;
        auto api=provider.table(); b::Observation out; out.counters.reserve(b::max_counters);
        for(std::uint64_t n=0;n<32;++n)
            if(api.invoke(api.user,id,n,out)!=b::Status::ok || !out.correct) return false;
        return provider.finish()==b::Status::ok;
    };
    auto first=std::async(std::launch::async,exercise,"xdma-roundtrip-4096");
    auto second=std::async(std::launch::async,exercise,"cuda-graph-4096");
    EXPECT_TRUE(first.get()); EXPECT_TRUE(second.get());
}
