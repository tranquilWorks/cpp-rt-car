// The same source is compiled against both documented 1.x CMake targets.
#include <rt/runtime.hpp>
#include "memory.hpp"
#include <array>
#include <charconv>
#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <vector>
namespace {
void require(bool value) { if (!value) throw std::runtime_error("migration/soak invariant failed"); }
struct Clock final : rt::RuntimeClock {
  std::uint64_t now_ns() noexcept override { return 1000; }
};
struct Owner {
  Clock clock;
  std::array<std::byte,8> state{};
  std::size_t calls=0;
  golden::Memory& memory;
  rt::Runtime runtime{clock};
  explicit Owner(golden::Memory& storage):memory(storage) {
    rt::RuntimeConfig cfg;
    cfg.callback_capacity=1; cfg.worker_count=1;
    cfg.executor_queue_capacity=2; cfg.task_scratch_slots=2;
    cfg.scratch_bytes=256; cfg.task_scratch_bytes=256; cfg.trace_capacity=32;
    require(runtime.configure(cfg)==rt::Status::ok);
    require(runtime.set_memory_provider(memory.table())==rt::Status::ok);
    require(runtime.set_cpu_memory_policy(golden::Memory::policy())==rt::Status::ok);
    rt::PhaseHandle phase;
    require(runtime.register_callback({"increment",[](void* p,const rt::CallbackContext&) {
      auto& self=*static_cast<Owner*>(p);
      self.state[0]=static_cast<std::byte>((std::to_integer<unsigned>(self.state[0])+1u)&255u);
      ++self.calls; return rt::CallbackResult::ok;
    },this},phase)==rt::Status::ok);
    require(runtime.register_state({"state",1,state})==rt::Status::ok);
    require(runtime.finalize()==rt::Status::ok);
    require(runtime.start()==rt::Status::ok && memory.live_count()==3);
  }
  ~Owner(){(void)runtime.stop();}
  void step(std::uint64_t frame) {
    require(runtime.step({frame,std::chrono::nanoseconds{100}})==rt::Status::ok);
  }
};
void cycle() {
  auto am=std::make_unique<golden::Memory>();
  auto bm=std::make_unique<golden::Memory>();
  {
    Owner a(*am),b(*bm);
    std::size_t extent=0;
    require(a.runtime.checkpoint_size(extent)==rt::Status::ok && extent<=65536);
    std::vector<std::byte> saved(extent);
    rt::ArtifactWriteResult result;
    require(a.runtime.write_checkpoint(0,saved,result)==rt::Status::ok && result.bytes_written==extent);
    for (std::uint64_t i=1;i<=64;++i) {
      a.step(i);require(b.calls==0 && b.state[0]==std::byte{0});
    }
    const auto expected=a.state;
    require(a.calls==64 && expected[0]==std::byte{64});
    require(a.runtime.restore_checkpoint(saved)==rt::Status::ok && a.state[0]==std::byte{0});
    for (std::uint64_t i=1;i<=64;++i) a.step(i);
    require(a.state==expected && a.calls==128 && b.calls==0);
    // Documented ordinary compatible cross-owner checkpoint restore.
    require(b.runtime.restore_checkpoint(saved)==rt::Status::ok);
    for (std::uint64_t i=1;i<=64;++i) b.step(i);
    require(b.state==expected && b.calls==64 && a.calls==128);
    require(a.runtime.stop()==rt::Status::ok && am->live_count()==0);
    require(a.runtime.step({65,std::chrono::nanoseconds{100}})==rt::Status::invalid_state);
    require(a.runtime.start()==rt::Status::invalid_state);
    b.step(65);require(b.calls==65 && b.state[0]==std::byte{65} && a.state==expected);
    require(b.runtime.stop()==rt::Status::ok && bm->live_count()==0);
    require(a.runtime.stop()==rt::Status::ok && b.runtime.stop()==rt::Status::ok);
  }
  require(am->acquisitions==3 && bm->acquisitions==3);
  require(am->releases==3 && bm->releases==3);
  require(am->violations==0 && bm->violations==0 && am->live_count()==0 && bm->live_count()==0);
}
}
int main(int argc,char** argv) {
  unsigned cycles=2;
  if (argc==3 && std::string_view(argv[1])=="--cycles") {
    const std::string_view arg(argv[2]);
    const auto parsed=std::from_chars(arg.data(),arg.data()+arg.size(),cycles);
    if(parsed.ec!=std::errc{} || parsed.ptr!=arg.data()+arg.size() || cycles<1 || cycles>10000) return 2;
  } else if(argc!=1) return 2;
  try {
    for(unsigned i=1;i<=cycles;++i) {
      cycle();
      std::cout << "{\"cycle\":" << i << ",\"frames\":193,\"logical_ns\":19300,\"owners\":2,\"acquired\":6,\"released\":6,\"live\":0,\"checkpoint_reexecutions\":1,\"expected_refusals\":2}" << std::endl;
    }
    return 0;
  } catch(const std::exception& e) {std::cerr << e.what() << '\n';return 1;}
}
