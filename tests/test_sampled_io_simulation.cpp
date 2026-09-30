#include "sampled_io_simulation/fixture.hpp"
#include <gtest/gtest.h>
namespace {
TEST(CommandBatch, SampledSimulationStartup) {
  EXPECT_TRUE(sampled_simulation::run_case(0));
}
TEST(CommandBatch, SampledSimulationShutdown) {
  EXPECT_TRUE(sampled_simulation::run_case(1));
}
TEST(CommandBatch, SampledSimulationFailureStop) {
  EXPECT_TRUE(sampled_simulation::run_case(2));
}
TEST(CommandBatch, SampledSimulationRegularRelease) {
  EXPECT_TRUE(sampled_simulation::run_case(3));
}
TEST(CommandBatch, SampledSimulationUnselectedMock) {
  EXPECT_TRUE(sampled_simulation::run_case(4));
}
TEST(CommandBatch, SampledSimulationUnselectedNative) {
  EXPECT_TRUE(sampled_simulation::run_case(5));
}
TEST(CommandBatch, SampledSimulationLogicalExpiry) {
  EXPECT_TRUE(sampled_simulation::run_case(6));
}
TEST(CommandBatch, SampledSimulationHostExpiry) {
  EXPECT_TRUE(sampled_simulation::run_case(7));
}
TEST(CommandBatch, SampledSimulationMissingAck) {
  EXPECT_TRUE(sampled_simulation::run_case(8));
}
TEST(CommandBatch, SampledSimulationNativeOptInRejected) {
  EXPECT_TRUE(sampled_simulation::run_case(9));
}
TEST(CommandBatch, SampledSimulationLateCompletionCleanup) {
  EXPECT_TRUE(sampled_simulation::run_case(10));
}
TEST(CommandBatch, SampledSimulationDefaultLateCompletionCleanup) {
  EXPECT_TRUE(sampled_simulation::run_case(11));
}
TEST(CommandBatch, SampledSimulationOwnersRemainIndependent) {
  bool first = false, second = false;
  std::thread a([&] { first = sampled_simulation::run_case(0); });
  std::thread b([&] { second = sampled_simulation::run_case(1); });
  a.join();
  b.join();
  EXPECT_TRUE(first);
  EXPECT_TRUE(second);
}
} // namespace
