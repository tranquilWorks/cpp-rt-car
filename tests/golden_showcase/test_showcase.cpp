#include "../../samples/golden_showcase/provider.hpp"
#include "../../samples/golden_showcase/probes.hpp"
#include "../../samples/golden_showcase/session.hpp"
#include "../../samples/golden_showcase/replay.hpp"
#include "../../samples/golden_system/oracle.hpp"
#include "../../samples/golden_system/replay.hpp"
#include "../golden_cuda/allocation.hpp"
#include <filesystem>
#include <iostream>
#include <memory>
#include <new>
using namespace golden;
namespace s = golden::showcase;
namespace b = rtfw::benchmark;
namespace alloc = rtfw_physics_allocation;
#define CHECK(x) do { if (!(x)) { std::cerr << "line " << __LINE__ << ": " #x "\n"; return false; } } while (false)
bool metadata() {
  const auto path=std::filesystem::temp_directory_path()/"golden-showcase-metadata-must-not-exist";
  CHECK(!std::filesystem::exists(path));
  s::Provider provider({},path);
  auto table=provider.table();
  b::Runner runner; b::ProviderHandle handle;
  CHECK(runner.register_provider(table,handle)==b::Status::ok);
  CHECK(runner.list().size()==15);
  for (std::size_t i=0;i<15;++i) {
    b::Descriptor d;
    CHECK(runner.describe("rtfw.golden",s::case_ids[i],d)==b::Status::ok);
    CHECK(d.subsystem==s::subsystems[i] && d.warmup==2 && d.repetitions==5 && d.retain_raw);
  }
  b::Observation observation;
  CHECK(table.invoke(table.user,"golden-loop",1,observation)==b::Status::invalid);
  CHECK(table.invoke(table.user,"foreign",0,observation)==b::Status::not_found);
  CHECK(table.invoke(table.user,"golden-external",0,observation)==b::Status::not_run);
  CHECK(!std::filesystem::exists(path));
  CHECK(runner.unregister_provider(handle)==b::Status::ok);
  CHECK(runner.unregister_provider(handle)==b::Status::stale);
  return true;
}
bool probes() {
  for (std::size_t kind=0;kind<7;++kind) {
    Options options;options.count=17;
    auto probe=std::make_unique<s::Probe>(options,kind);
    rt::Runtime runtime;
    rt::RuntimeConfig config;config.callback_capacity=1;config.worker_count=3;
    config.executor_queue_capacity=1024;config.scratch_bytes=config.task_scratch_bytes=64;config.task_scratch_slots=1024;
    rt::PhaseHandle phase;
    CHECK(runtime.configure(config)==rt::Status::ok);
    CHECK(runtime.register_callback({"probe",s::Probe::invoke,probe.get()},phase)==rt::Status::ok);
    CHECK(runtime.finalize()==rt::Status::ok && runtime.start()==rt::Status::ok);
    for (std::size_t tick=0;tick<24;++tick) {
      alloc::begin();
      const auto status=runtime.step({tick,std::chrono::nanoseconds(fixed::tick_ns),std::nullopt,std::nullopt});
      const auto allocations=alloc::end();
      CHECK(status==rt::Status::ok && allocations==0 && probe->verify());
    }
    CHECK(runtime.stop()==rt::Status::ok);
  }
  return true;
}
bool default_equivalence(bool host) {
  Options options;options.host=host;options.count=17;options.workers=3;options.grain=1;
  auto memory=std::make_unique<Memory>(), derived_memory=std::make_unique<Memory>();
  auto jobs=std::make_unique<Jobs>(), derived_jobs=std::make_unique<Jobs>();
  if(host) CHECK(jobs->start(3)==rt::Status::ok && derived_jobs->start(3)==rt::Status::ok);
  auto original=std::make_unique<Session>(options,host?jobs.get():nullptr,*memory);
  auto derived=std::make_unique<s::Session>(options,host?derived_jobs.get():nullptr,*derived_memory);
  CHECK(original->prepare()==rt::Status::ok && derived->prepare()==rt::Status::ok);
  rt::RuntimeMetricSnapshot a,b;
  CHECK(original->runtime->metrics_snapshot(rt::RuntimeMetricWindow::cumulative,nullptr,a)==rt::Status::ok);
  CHECK(derived->runtime->metrics_snapshot(rt::RuntimeMetricWindow::cumulative,nullptr,b)==rt::Status::ok);
  CHECK(a.metadata.config_id==b.metadata.config_id && a.metadata.runtime_id!=b.metadata.runtime_id);
  CHECK(original->plan.planned_bytes==derived->plan.planned_bytes);
  CHECK(original->plan.trace_storage_bytes==derived->plan.trace_storage_bytes);
  Replay replay(options.ticks);s::Replay derived_replay(options.ticks);
  CHECK(replay.begin(*original) && derived_replay.begin(*derived));
  CHECK(original->controls() && derived->controls());
  Oracle oracle(options);
  for(std::size_t tick=0;tick<options.ticks;++tick) {
    replay.record(*original,tick);derived_replay.record(*derived,tick);
    alloc::begin();const auto x=original->step(tick);const auto y=derived->step(tick);const auto n=alloc::end();
    CHECK(x==rt::Status::ok && y==rt::Status::ok && n==0);
    CHECK(original->world.canonical==derived->world.canonical && oracle.step(tick,original->world));
  }
  CHECK(replay.seal(*original) && derived_replay.seal(*derived));
  const auto untouched=derived->world.canonical;
  CHECK(derived->runtime->replay_live_control(replay.trusted,s::Replay::input,derived.get())!=rt::Status::ok);
  CHECK(derived->world.canonical==untouched);
  alloc::begin();const auto x=replay.verify(*original),y=derived_replay.verify(*derived);const auto n=alloc::end();
  CHECK(x && y && n==0);
  // Failed off-lane rollback retains borrowed Memory until an explicit retry.
  derived_memory->fail_rollback=1;
  CHECK(derived->close()!=rt::Status::ok && derived_memory->live_count()>0);
  CHECK(derived->close()==rt::Status::ok && original->close()==rt::Status::ok);
  CHECK(memory->acquisitions==memory->releases && derived_memory->acquisitions==derived_memory->releases);
  if(host) CHECK(jobs->close()==rt::Status::ok && derived_jobs->close()==rt::Status::ok && jobs->accepted==jobs->completed && derived_jobs->accepted==derived_jobs->completed);
  return true;
}
bool positives() {
  alloc::begin();void *p=::operator new(64);const auto ordinary=alloc::end();::operator delete(p);CHECK(ordinary>0);
  alloc::begin();p=::operator new(128,std::align_val_t{64});const auto aligned=alloc::end();::operator delete(p,std::align_val_t{64});CHECK(aligned>0);
  return true;
}
int main() {
  try { if(!positives() || !metadata() || !probes() || !default_equivalence(false) || !default_equivalence(true)) return 1; }
  catch(const std::exception &e) {std::cerr<<e.what()<<'\n';return 1;}
  std::cout<<"PASS metadata, exact default/state/config parity, borrowed ownership, originating replay and steady ordinary/aligned zero allocation\n";
}
