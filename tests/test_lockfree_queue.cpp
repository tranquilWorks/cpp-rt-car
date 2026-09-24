#include "simcore/lockfree_queue.hpp"
#include <gtest/gtest.h>
#include <array>
#include <barrier>
#include <latch>
#include <thread>
#include <vector>

using namespace simcore;

TEST(LockFreeQueue, MultiProducerConsumer) {
  LockFreeQueue<int> q;
  const int N = 1000;
  std::thread p1([&] {
    for (int i = 0; i < N; ++i)
      q.push(i);
  });
  std::thread p2([&] {
    for (int i = 0; i < N; ++i)
      q.push(i);
  });
  p1.join();
  p2.join();
  std::vector<int> out1, out2;
  std::thread c1([&] {
    int v;
    while (q.pop(v))
      out1.push_back(v);
  });
  std::thread c2([&] {
    int v;
    while (q.pop(v))
      out2.push_back(v);
  });
  c1.join();
  c2.join();
  scan();
  EXPECT_EQ(out1.size() + out2.size(), static_cast<size_t>(2 * N));
}

TEST(LockFreeQueue, FifoEmptyAndReuse) {
  LockFreeQueue<int> queue;
  int value = -1;
  EXPECT_FALSE(queue.pop(value));
  for (int round = 0; round < 16; ++round) {
    for (int i = 0; i < 64; ++i)
      queue.push(round * 64 + i);
    for (int i = 0; i < 64; ++i) {
      ASSERT_TRUE(queue.pop(value));
      EXPECT_EQ(value, round * 64 + i);
    }
    EXPECT_FALSE(queue.pop(value));
    scan();
  }
}

TEST(LockFreeQueue, SimultaneousProducersConsumersDeliverEachValueOnce) {
  constexpr int count = 256;
  for (int round = 0; round < 16; ++round) {
    LockFreeQueue<int> queue;
    for (int i = 0; i < 2 * count; ++i)
      queue.push(i);
    std::barrier start(4);
    std::array<std::array<int, count>, 2> observed{};
    std::array<std::thread, 4> workers;
    for (int producer = 0; producer < 2; ++producer) {
      workers[static_cast<std::size_t>(producer)] = std::thread([&, producer] {
        start.arrive_and_wait();
        for (int i = 0; i < count; ++i)
          queue.push((producer + 2) * count + i);
      });
    }
    for (std::size_t consumer = 0; consumer < 2; ++consumer) {
      workers[consumer + 2] = std::thread([&, consumer] {
        start.arrive_and_wait();
        // Preloaded work guarantees exactly count successful calls per owner;
        // no scheduler-dependent polling or retry budget is needed.
        for (int i = 0; i < count; ++i) {
          int value = -1;
          EXPECT_TRUE(queue.pop(value));
          observed[consumer][static_cast<std::size_t>(i)] = value;
        }
        scan();
      });
    }
    for (auto &worker : workers)
      worker.join();
    std::array<unsigned, 4 * count> seen{};
    for (const auto &values : observed) {
      for (int value : values) {
        ASSERT_GE(value, 0);
        ASSERT_LT(value, 4 * count);
        ++seen[static_cast<std::size_t>(value)];
      }
    }
    for (int i = 0; i < 2 * count; ++i) {
      int value = -1;
      ASSERT_TRUE(queue.pop(value));
      ASSERT_GE(value, 0);
      ASSERT_LT(value, 4 * count);
      ++seen[static_cast<std::size_t>(value)];
    }
    int value = -1;
    EXPECT_FALSE(queue.pop(value));
    for (auto deliveries : seen)
      EXPECT_EQ(deliveries, 1u);
    scan();
  }
}

namespace {
struct CopyGate {
  std::latch entered{1};
  std::latch resume{1};
  std::atomic<unsigned> destroyed{0};
};

struct CopyProbe {
  int value = 0;
  CopyGate *gate = nullptr;
  bool owned_copy = false;
  bool pause = false;
  CopyProbe() = default;
  CopyProbe(int v, CopyGate &g) : value(v), gate(&g) {}
  CopyProbe(const CopyProbe &other)
      : value(other.value), gate(other.gate), owned_copy(true) {}
  CopyProbe &operator=(const CopyProbe &other) {
    if (pause && other.value == 1) {
      other.gate->entered.count_down();
      other.gate->resume.wait();
    }
    // Read after the competing consumer has advanced and scanned. The queue
    // must keep the source node alive throughout the user-defined copy.
    value = other.value;
    gate = other.gate;
    return *this;
  }
  ~CopyProbe() {
    if (owned_copy && value == 1 && gate)
      gate->destroyed.fetch_add(1);
  }
};
} // namespace

TEST(LockFreeQueue, SuccessorLivesUntilInFlightValueCopyFinishes) {
  CopyGate gate;
  LockFreeQueue<CopyProbe> queue;
  for (int i = 1; i <= 32; ++i)
    queue.push(CopyProbe{i, gate});
  bool popped = true;
  std::thread paused([&] {
    CopyProbe output;
    output.pause = true;
    popped = queue.pop(output);
    scan();
  });
  gate.entered.wait();
  for (int i = 1; i <= 32; ++i) {
    CopyProbe output;
    EXPECT_TRUE(queue.pop(output));
    EXPECT_EQ(output.value, i);
  }
  scan();
  EXPECT_EQ(gate.destroyed.load(), 0u);
  gate.resume.count_down();
  paused.join();
  EXPECT_FALSE(popped);
  scan();
  EXPECT_EQ(gate.destroyed.load(), 1u);
}
