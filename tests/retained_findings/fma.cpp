#include <rt/numerics.hpp>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>
namespace rtfw_physics_allocation { void begin() noexcept; std::size_t end() noexcept; }
static void require(bool good) { if (!good) std::abort(); }
int main() {
  static_assert(rt::detail::kBuildAllowsFma);
  // Preserve raw-reference source compatibility, only under exclusive access.
  bool& legacy = rt::use_fma_flag(); legacy = false;
  require(!rt::use_fma());
  std::atomic<bool> ready{false};
  std::vector<std::thread> owners;
  for (unsigned n=0;n<4;++n) owners.emplace_back([&] {
    while (!ready.load(std::memory_order_acquire)) std::this_thread::yield();
    for (unsigned i=0;i<100000;++i) {
      rt::set_use_fma(false); require(!rt::use_fma());
      require(rt::fma(2.0,3.0,4.0)==10.0);
    }
  });
  rtfw_physics_allocation::begin();
  ready.store(true,std::memory_order_release);
  for (auto& owner:owners) owner.join();
  require(rtfw_physics_allocation::end()==0);
  // This is deliberately still process-global; it is not owner isolation.
  std::thread other([] { rt::set_use_fma(true); }); other.join();
  require(rt::use_fma());
  const double a=1.0+0x1p-27, b=1.0-0x1p-27;
  require(rt::fma(a,b,-1.0)==std::fma(a,b,-1.0));
  rt::set_use_fma(false); require(!rt::use_fma());
  // Mixed policy reads/writes are race-free, but not deterministic policy selection.
  ready=false;
  std::thread writer([&] { while(!ready.load()) {} for(unsigned i=0;i<100000;++i) rt::set_use_fma((i&1)!=0); });
  std::thread reader([&] { ready=true; for(unsigned i=0;i<100000;++i) require(rt::fma(2,3,4)==10); });
  writer.join(); reader.join(); rt::set_use_fma(false);
  std::puts("PASS forced-FMA same-policy concurrency, global-policy boundary, mixed atomic access");
}
