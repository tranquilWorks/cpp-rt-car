#include "benchmark_fixtures/provider.hpp"
#include <gtest/gtest.h>
#include <thread>
using namespace benchmark_test;

TEST(BenchmarkRunner, ExactFakeClockWarmupSamplesAndCanonicalBytes) {
    Fixture f,g;
    const auto r=result(f), s=result(g);
    ASSERT_EQ(r.status,b::Status::ok); ASSERT_EQ(r.samples.size(),5U);
    EXPECT_EQ(f.calls,7U); EXPECT_EQ(f.clock_calls,14U); EXPECT_EQ(r.warmup_completed,2U);
    EXPECT_EQ(r.samples[0].start_ns,40U); EXPECT_EQ(r.samples[4].end_ns,130U);
    const auto encoded=b::encode(r), repeated=b::encode(s);
    EXPECT_EQ(encoded.descriptor,repeated.descriptor); EXPECT_EQ(encoded.raw,repeated.raw); EXPECT_EQ(encoded.summary,repeated.summary);
    EXPECT_NE(encoded.summary.find("\"operations\":5"),std::string::npos);
    EXPECT_NE(encoded.summary.find("\"p95_ns\":10"),std::string::npos);
    EXPECT_NE(encoded.summary.find("\"total_ns\":50"),std::string::npos);
}
TEST(BenchmarkRunner, ZeroWarmupAndMaximumRepetitionBound) {
    Fixture f; f.descriptor.warmup=0; f.descriptor.repetitions=1;
    auto r=result(f); ASSERT_EQ(r.status,b::Status::ok); EXPECT_EQ(f.calls,1U); EXPECT_EQ(r.samples[0].start_ns,0U);
    Fixture g; g.descriptor.warmup=b::max_warmup; g.descriptor.repetitions=b::max_repetitions;
    r=result(g); EXPECT_EQ(r.status,b::Status::ok); EXPECT_EQ(g.calls,11000U); EXPECT_EQ(r.samples.size(),10000U);
    EXPECT_NO_THROW((void)b::encode(r));
}
TEST(BenchmarkRunner, CallbackClockAndInvariantFailureRemainFailed) {
    Fixture f; f.throw_invoke=true; auto r=result(f);
    EXPECT_EQ(r.status,b::Status::provider_error); EXPECT_TRUE(r.samples.empty());
    EXPECT_EQ(b::encode(r).summary.find("secret"),std::string::npos);
    Fixture g; g.wrong=true; r=result(g); EXPECT_EQ(r.status,b::Status::invariant_failed);
    EXPECT_NE(b::encode(r).summary.find("\"statistics\":null"),std::string::npos);
    Fixture h; h.unavailable=true; r=result(h); EXPECT_EQ(r.status,b::Status::not_run);
    EXPECT_NO_THROW((void)b::encode(r));
    Fixture k; k.throw_clock=true; r=result(k); EXPECT_EQ(r.status,b::Status::clock_error); EXPECT_EQ(k.calls,0U);
    Fixture l; l.ticks=100; l.backward=true; r=result(l); EXPECT_EQ(r.status,b::Status::clock_error);
}
TEST(BenchmarkRunner, MissingSelectionAndInvalidIdentityNeverInvoke) {
    Fixture f; b::Runner r; b::ProviderHandle h;
    ASSERT_EQ(r.register_provider(f.table(),h),b::Status::ok);
    EXPECT_EQ(r.run("missing","case",f.clock(),{}).status,b::Status::not_found);
    b::Identity bad; bad.backend="../../device";
    EXPECT_EQ(r.run("fixture","case",f.clock(),bad).status,b::Status::invalid);
    auto c=f.clock(); c.version=0;
    EXPECT_EQ(r.run("fixture","case",c,{}).status,b::Status::invalid);
    EXPECT_EQ(f.calls,0U);
}
TEST(BenchmarkRunner, ReentrancyAndIndependentInstances) {
    Fixture f; b::Runner r; b::ProviderHandle h; f.reenter=&r;
    ASSERT_EQ(r.register_provider(f.table(),h),b::Status::ok);
    EXPECT_EQ(r.run("fixture","case",f.clock(),{}).status,b::Status::ok);
    EXPECT_EQ(r.unregister_provider(h),b::Status::ok);
    b::Status statuses[2]{b::Status::invalid,b::Status::invalid};
    std::thread one([&]{Fixture local; statuses[0]=result(local).status;});
    std::thread two([&]{Fixture local; statuses[1]=result(local).status;});
    one.join(); two.join(); EXPECT_EQ(statuses[0],b::Status::ok); EXPECT_EQ(statuses[1],b::Status::ok);
}
TEST(BenchmarkRunner, RealClockIsCharacterizationOnly) {
    Fixture f; b::Runner r; b::ProviderHandle h;
    ASSERT_EQ(r.register_provider(f.table(),h),b::Status::ok);
    const auto output=r.run("fixture","case",b::steady_clock(),b::capture_identity());
    ASSERT_EQ(output.status,b::Status::ok);
    std::uint64_t elapsed=0;
    for (const auto& s:output.samples) { EXPECT_GE(s.end_ns,s.start_ns); elapsed+=s.end_ns-s.start_ns; }
    EXPECT_GT(elapsed,0U);  // observation, not a performance threshold
    EXPECT_NE(b::encode(output).summary.find("portable_characterization"),std::string::npos);
    EXPECT_NE(b::encode(output).summary.find("\"qualification\":\"none\""),std::string::npos);
}
TEST(BenchmarkRunner, TamperedResultCannotBeSerialized) {
    Fixture f; const auto original=result(f);
    auto r=original; r.samples[0].index=5; EXPECT_THROW((void)b::encode(r),std::invalid_argument);
    r=original; r.samples.pop_back(); EXPECT_THROW((void)b::encode(r),std::invalid_argument);
    r=original; r.samples[0].end_ns=0; EXPECT_THROW((void)b::encode(r),std::invalid_argument);
    r=original; r.samples[0].observation.counters[0]=2; EXPECT_THROW((void)b::encode(r),std::invalid_argument);
    r=original; r.clock=static_cast<b::ClockKind>(8); EXPECT_THROW((void)b::encode(r),std::invalid_argument);
}
