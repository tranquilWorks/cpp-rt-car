#include "../../samples/golden_xdma/owner.hpp"
#include "../../samples/golden_xdma/replay.hpp"
#include "../../samples/golden_system/oracle.hpp"
#include "../golden_cuda/allocation.hpp"
#include <iostream>
#include <vector>
#define CHECK(x) do { if (!(x)) { std::cerr << __func__ << ':' << __LINE__ << " failed: " #x << '\n'; return false; } } while(false)
using namespace golden;
namespace gx = golden::xdma;
namespace allocation = rtfw_physics_allocation;
using rt::Status;
bool same(const StateBytes &a, const StateBytes &b) {
  for (std::size_t i=0;i<a.size();++i) {
    if ((i>=64 && i<72) || (i>=480 && i<488)) continue;
    if(a[i]!=b[i]) { std::cerr << "state differs at " << i << '\n'; return false; }
  }
  return bool(get(a,64,8)) == bool(get(b,64,8));
}
bool parity(Options o, bool xdma_only = false) {
  std::vector<StateBytes> reference(o.ticks);
  {
    auto memory=std::make_unique<Memory>();
    auto jobs=std::make_unique<Jobs>();
    if(o.host) CHECK(jobs->start(o.workers)==Status::ok);
    auto s=std::make_unique<golden::Session>(o,o.host?jobs.get():nullptr,*memory);
    auto policy=Memory::policy(); policy.thread_policy_count=1;
    policy.thread_policies[0].role=rt::thread_role_executor_worker;
    policy.thread_policies[0].policy.wait_strategy=rt::WaitStrategy::park;
    CHECK(s->prepare(fixed::action_capacity,&policy)==Status::ok && s->controls());
    Oracle oracle(o);
    for(std::size_t t=0;t<o.ticks;++t) {
      CHECK(s->step(t)==Status::ok && oracle.step(t,s->world));
      reference[t]=s->world.canonical;
    }
    CHECK(s->close()==Status::ok);s.reset();
    if(o.host) CHECK(jobs->close()==Status::ok);
  }
  for(auto dispatch:{gx::Dispatch::cpu,gx::Dispatch::kernel,gx::Dispatch::graph}) {
    if (xdma_only && dispatch != gx::Dispatch::cpu) continue;
    std::cout << "parity dispatch=" << int(dispatch) << " host=" << o.host << " count=" << o.count << " ticks=" << o.ticks << std::endl;
    auto owner=std::make_unique<gx::Owner>(o,dispatch);
    auto status=owner->prepare();
    if(status!=Status::ok) std::cerr << "prepare=" << int(status) << ' ' << owner->session->runtime->last_error() << '\n';
    CHECK(status==Status::ok);
    auto &s=*owner->session;
    gx::Replay replay(o.ticks);
    CHECK(replay.begin(s) && s.controls());
    Oracle oracle(o);
    for(std::size_t t=0;t<o.ticks;++t) {
      replay.record(s,t);
      allocation::begin();status=s.step(t);auto allocations=allocation::end();
      if(status!=Status::ok)std::cerr << "tick=" << t << " status=" << int(status) << ' ' << s.runtime->last_error() << '\n';
      CHECK(status==Status::ok && allocations==0);
      CHECK(oracle.step(t,s.world) && same(reference[t],s.world.canonical));
    }
    CHECK(replay.seal(s));
    allocation::begin();const auto replayed=replay.verify(s);auto allocations=allocation::end();
    if(!replayed)std::cerr << "replay=" << int(replay.result.mismatch_status) << ' ' << s.runtime->last_error() << '\n';
    CHECK(replayed && allocations==0);
    CHECK(owner->close()==Status::ok);
    CHECK(owner->driver->initializes==1 && owner->driver->shutdowns==1);
  }
  return true;
}
int main(int argc, char **argv) {
  const bool xdma_only = argc == 2 && std::string_view(argv[1]) == "--xdma-only";
  if (argc != 1 && !xdma_only) return 2;
  allocation::begin();auto *p=new std::byte[37]; volatile auto *q=p; q[0]=std::byte{1};delete[]p;const auto positive=allocation::end();
  if(!positive)return 2;
  Options o; o.count=9;
  for(auto host:{false,true}) {o.host=host;if(!parity(o, xdma_only))return 1;}
  std::cout << (xdma_only ? "PASS XDMA-only" : "PASS XDMA/combined") << " parity and zero-allocation replay; positive=" << positive << '\n';
}
