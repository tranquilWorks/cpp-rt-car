#include <gtest/gtest.h>
#include <simcore/SimCore.hpp>

#include <array>
#include <atomic>
#include <barrier>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace {
// Integer-valued doubles stay exactly representable throughout this workload.
// The independent closed form checks every element, frame sum and leaf hash.
struct RangeCase {
  static SimCore::Settings configuration(std::size_t threads) {
    SimCore::Settings settings;
    settings.threads = threads;
    settings.maxFrames = 24;
    settings.hz = 1000;
    settings.autoTuneChunks = false;
    settings.chunkSize = 127;
    settings.detReduceLeaf = 17;
    settings.rateGovernorEnable = false;
    settings.driftLogInterval = 0;
    settings.useFMA = false;
    return settings;
  }
  SimCore sim;
  std::array<std::vector<double>, 2> values{std::vector<double>(513),
                                            std::vector<double>(4097)};
  bool correct = true;
  std::array<unsigned, 2> reductions{};
  std::uint64_t seed;
  double expected(std::size_t i, std::int64_t frame) const {
    const auto scale = std::uint64_t{1} << static_cast<unsigned>(frame + 1);
    return static_cast<double>((seed + i % 17 + 2) * scale - 2);
  }
  explicit RangeCase(std::size_t threads, std::uint64_t initial)
      : sim(configuration(threads)), seed(initial) {
    for (std::size_t p = 0; p < values.size(); ++p) {
      auto &data = values[p];
      for (std::size_t i = 0; i < data.size(); ++i)
        data[i] = static_cast<double>(seed + i % 17);
      const auto phase =
          sim.addPhase("range-" + std::to_string(p), data.size());
      sim.addParallelRangeTask(phase, [&, p](std::size_t b, std::size_t e,
                                             std::int64_t, SimCore::Seconds) {
        for (auto i = b; i < e; ++i)
          values[p][i] += 1;
      });
      sim.addParallelRangeTask(
          phase,
          [&, p](std::size_t b, std::size_t e, std::int64_t, SimCore::Seconds) {
            for (auto i = b; i < e; ++i)
              values[p][i] *= 2;
          },
          SimCore::TaskHint::Latency);
      sim.addDeterministicRangeReduction(
          phase,
          [&, p](std::size_t b, std::size_t e, std::int64_t, SimCore::Seconds) {
            double sum = 0;
            for (auto i = b; i < e; ++i)
              sum += values[p][i];
            return sum;
          },
          [&, p](double sum, const std::uint64_t *hashes, std::size_t count,
                 std::int64_t frame, SimCore::Seconds) {
            const auto size = values[p].size();
            correct &= count == (size + 16) / 17;
            double total = 0;
            for (std::size_t b = 0, leaf = 0; b < size; b += 17, ++leaf) {
              double partial = 0;
              for (auto i = b; i < std::min(b + 17, size); ++i) {
                const auto value = expected(i, frame);
                correct &= values[p][i] == value;
                partial += value;
              }
              total += partial;
              std::uint64_t bits = 0;
              std::memcpy(&bits, &partial, sizeof(bits));
              const auto hash =
                  (1469598103934665603ULL ^ bits) * 1099511628211ULL;
              correct &= leaf < count && hashes[leaf] == hash;
            }
            correct &= sum == total;
            ++reductions[p];
          });
    }
  }
  bool run() {
    sim.run();
    return correct && reductions[0] == 24 && reductions[1] == 24;
  }
};
} // namespace

TEST(SimCoreTimingReliability,
     RangeAndReductionStorageIsQuiescentAcrossFrames) {
  for (const std::size_t threads : {0u, 1u, 2u, 4u, 8u}) {
    SCOPED_TRACE(threads);
    RangeCase fixture(threads, 3);
    EXPECT_TRUE(fixture.run());
  }
}

TEST(SimCoreTimingReliability,
     IndependentOwnersAndRepeatedStartupStayIsolated) {
  // Construction calibrates a process-global clock and binds the caller's
  // arena. Construct on each execution thread, serially, before either runs.
  std::mutex initialization;
  std::barrier stages(2);
  std::array<bool, 2> okay{true, true};
  const auto run = [&](std::size_t index, std::size_t threads,
                       std::uint64_t seed) {
    for (unsigned iteration = 0; iteration < 3; ++iteration) {
      std::unique_ptr<RangeCase> fixture;
      {
        std::lock_guard lock(initialization);
        fixture = std::make_unique<RangeCase>(threads, seed + iteration);
      }
      stages.arrive_and_wait();
      okay[index] &= fixture->run();
      stages.arrive_and_wait();
      fixture.reset();
      stages.arrive_and_wait();
    }
  };
  std::thread first(run, 0, 4, 7), second(run, 1, 8, 19);
  first.join();
  second.join();
  EXPECT_TRUE(okay[0]);
  EXPECT_TRUE(okay[1]);
}

TEST(SimCoreTimingReliability, WatchdogActionsReachTheFinalQuiescentBoundary) {
  SimCore::Settings settings;
  settings.threads = 2;
  settings.hz = 1000;
  settings.maxFrames = 1;
  settings.budgetMonitor = false;
  settings.rateGovernorEnable = false;
  settings.bintraceEnable = true;
  settings.bintraceEventsPerThread = 1024;
  settings.driftLogInterval = 0;
  SimCore sim(settings);
  const auto phase = sim.addPhase("watchdog-handoff");
  sim.addSerialSubsystem(phase, [&](std::int64_t, SimCore::Seconds) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (sim.watchdogTrips() == 0 &&
           std::chrono::steady_clock::now() < deadline)
      std::this_thread::yield();
    EXPECT_GT(sim.watchdogTrips(), 0);
    // A trip is immediately visible, but coordinator-owned settings cannot
    // change underneath the active callback. The final boundary must apply it.
    EXPECT_TRUE(sim.limpModeActive());
    EXPECT_TRUE(sim.visualizersEnabled());
    EXPECT_FALSE(sim.broadphaseCoarse());
    EXPECT_EQ(sim.rungActivations(4), 0u);
  });
  sim.run();
  EXPECT_GT(sim.watchdogTrips(), 0);
  EXPECT_FALSE(sim.visualizersEnabled());
  EXPECT_TRUE(sim.broadphaseCoarse());
  EXPECT_EQ(sim.subSteps(), 1);
  for (int rung = 1; rung <= 4; ++rung)
    EXPECT_EQ(sim.rungActivations(rung), 1u);
  std::size_t events = 0;
  for (const auto &event : sim.bintrace().snapshot().events)
    if (event.code == bintrace::EV_WatchdogTrip)
      ++events;
  EXPECT_EQ(events, static_cast<std::size_t>(sim.watchdogTrips()));
}

namespace {
std::atomic<unsigned> clock_reads{0};
std::atomic<unsigned> clock_fault{0};
std::uint64_t faulty_counter() {
  const auto read = clock_reads.fetch_add(1);
  if (read < 2)
    return static_cast<std::uint64_t>(read + 1) * 1000;
  switch (clock_fault.load()) {
  case 0:
    return 1'000'000'000ULL;
  case 1:
    return 1;
  default:
    return std::numeric_limits<std::uint64_t>::max();
  }
}
} // namespace

TEST(SimCoreTimingReliability, InvalidClockSamplesCannotPoisonFallback) {
  struct RestoreClock {
    ~RestoreClock() { HighResClock::init(); }
  } restore;
  for (unsigned fault = 0; fault < 3; ++fault) {
    SCOPED_TRACE(fault);
    clock_reads = 0;
    clock_fault = fault;
    HighResClock::init(faulty_counter);
    ASSERT_TRUE(HighResClock::using_tsc());
    const auto sample = HighResClock::now();
    const auto reference = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
    EXPECT_FALSE(HighResClock::using_tsc());
    EXPECT_LE(sample, reference + 1'000'000);
    auto previous = sample;
    for (unsigned i = 0; i < 32; ++i) {
      const auto next = HighResClock::now();
      EXPECT_GT(next, previous);
      previous = next;
    }
    const auto after = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
    EXPECT_LE(previous, after + 1'000'000);
  }
}

TEST(SimCoreTimingReliability, WatchdogStopsAcrossIdleAndArmedLifecycles) {
  for (unsigned iteration = 0; iteration < 32; ++iteration) {
    std::atomic<unsigned> callbacks{0};
    {
      rt::Watchdog watchdog(std::chrono::hours(24), std::chrono::hours(24),
                            [&] { callbacks.fetch_add(1); });
      if ((iteration & 1u) == 0) {
        watchdog.arm(std::chrono::milliseconds(1));
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (callbacks.load() == 0 &&
               std::chrono::steady_clock::now() < deadline)
          std::this_thread::yield();
        EXPECT_GT(callbacks.load(), 0u);
      } else {
        std::this_thread::yield();
      }
    }
    // Returning from destruction establishes callback lifetime completion.
    const auto completed = callbacks.load();
    std::this_thread::yield();
    EXPECT_EQ(callbacks.load(), completed);
  }
}
