#pragma once
// Derived from the M26-02 CLI; shared CPU model, controls and replay remain
// authoritative.
#include "oracle.hpp"
#include "../golden_system/peer.hpp"
#include "../golden_system/session.hpp"
#include "owner.hpp"
#include "replay.hpp"
#include "telemetry.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <numeric>
namespace fs = std::filesystem;
using namespace golden;
namespace golden::xdma {
enum class Fault { none, underflow, overrun, stop_failure, device_loss, reset_failure };
inline constexpr std::array<std::string_view, 6> fault_names{"none", "underflow", "overrun", "stop_failure", "device_loss", "reset_failure"};
struct Arguments {
  std::string dispatch = "graph";
  Fault fault = Fault::none;
  Options scenario;
  cil::Options peer;
  fs::path output;
};
bool arguments(int argc, char **argv, Arguments &a) {
  for (int i = 1; i < argc; ++i) {
    std::string_view key = argv[i];
    if (i + 1 >= argc)
      return false;
    std::string_view value = argv[++i];
    std::uint64_t number = 0;
    if (key == "--dispatch") {
      if (value != "cpu" && value != "kernel" && value != "graph")
        return false;
      a.dispatch = value;
    } else if (key == "--fault") {
      auto it = std::find(fault_names.begin(), fault_names.end(), value);
      if (it == fault_names.end()) return false;
      a.fault = static_cast<Fault>(it - fault_names.begin());
    } else if (key == "--mode") {
      if (value != "native" && value != "host")
        return false;
      a.scenario.host = value == "host";
    } else if (key == "--output")
      a.output = argv[i];
    else if (key == "--campaign") {
      auto it = std::find(campaign_names.begin(), campaign_names.end(), value);
      if (it == campaign_names.end())
        return false;
      a.scenario.campaign = static_cast<Campaign>(it - campaign_names.begin());
    } else if (key == "--peer") {
      a.scenario.external = true;
      a.peer.key = value;
    } else {
      if (!cil::number(value, number))
        return false;
      if (key == "--count")
        a.scenario.count = number;
      else if (key == "--ticks")
        a.scenario.ticks = number;
      else if (key == "--workers")
        a.scenario.workers = number;
      else if (key == "--grain")
        a.scenario.grain = number;
      else if (key == "--generation")
        a.peer.generation = number;
      else if (key == "--timeout-ms")
        a.peer.timeout_ms = number;
      else
        return false;
    }
  }
  return a.scenario.valid() &&
         (a.fault == Fault::none ||
          ((a.dispatch != "cpu" || (a.fault != Fault::device_loss && a.fault != Fault::reset_failure)) && !a.scenario.external &&
           a.scenario.campaign == Campaign::nominal &&
           a.scenario.ticks >= 19)) &&
         (!a.scenario.external ||
          (cil::valid_key(a.peer.key) && a.peer.generation &&
           a.peer.timeout_ms >= 100 && a.peer.timeout_ms <= 5000)) &&
         !(a.scenario.external && a.scenario.campaign == Campaign::overload);
}
bool write(const fs::path &path, std::span<const std::byte> bytes) {
  std::ofstream f(path, std::ios::binary);
  f.write(reinterpret_cast<const char *>(bytes.data()),
          static_cast<std::streamsize>(bytes.size()));
  f.close();
  return !f.fail();
}
int failure(int line) { std::cerr << "golden_xdma validation failed at line " << line << '\n'; return 1; }
int execute(const Arguments &a) {
  const auto o = a.scenario;
#if defined(_WIN32)
  const auto process_id = GetCurrentProcessId();
#else
  const auto process_id = getpid();
#endif
  const auto session_identity = std::to_string(cil::now_ns()) + "-" + std::to_string(process_id);
  const bool combined = a.dispatch != "cpu";
  const auto dispatch = !combined ? Dispatch::cpu : a.dispatch == "kernel" ? Dispatch::kernel : Dispatch::graph;
  auto owner = std::make_unique<Owner>(o, dispatch);
  auto *s = owner->session.get();
  auto status = owner->prepare();
  if (status != rt::Status::ok) {
    std::cerr << "prepare " << s->runtime->last_error() << '\n'; return failure(__LINE__);
  }
  struct Totals {
    std::array<std::uint64_t, 2> providers{}, publications{};
    std::uint64_t uploads=0, downloads=0, controls=0, events=0, safe_acks=0,
      initializes=0, shutdowns=0, jobs_accepted=0, jobs_completed=0, acquisitions=0, releases=0,
      cuda_providers=0, cuda_publications=0, cuda_uploads=0, cuda_downloads=0, cuda_kernels=0,
      cuda_graphs=0, cuda_events=0, cuda_faults=0;
    bool close(Owner &v) {
      // Read counters only off lane, after step or failed checked stop.
      auto p=v.io->providers, q=v.io->publications;
      const auto cp=v.physics?v.physics->providers:0, cq=v.physics?v.physics->publications:0;
      const auto closed=v.close(); if(closed!=rt::Status::ok) { std::cerr<<"checked close "<<int(closed)<<" "<<(v.session?v.session->runtime->last_error():std::string_view{})<<'\n'; return false; }
      for(std::size_t i=0;i<2;++i){providers[i]+=p[i];publications[i]+=q[i];}
      cuda_providers+=cp;cuda_publications+=cq;
      auto &d=*v.driver;
#define SUM(field) field += d.field.load()
      SUM(uploads); SUM(downloads); SUM(controls); SUM(events); SUM(safe_acks);
      SUM(initializes); SUM(shutdowns);
#undef SUM
      if(v.cuda_driver) {
        auto &c=*v.cuda_driver;
        cuda_uploads+=c.uploads;cuda_downloads+=c.downloads;
        cuda_kernels+=c.kernels;cuda_graphs+=c.graphs;cuda_events+=c.records;cuda_faults+=c.faults;
        if(!c.protocol_ok || !c.clean())return false;
      }
      jobs_accepted+=v.jobs->accepted; jobs_completed+=v.jobs->completed;
      acquisitions+=v.memory->acquisitions; releases+=v.memory->releases;
      return !d.live && initializes==shutdowns && jobs_accepted==jobs_completed && acquisitions==releases;
    }
  } totals;
  std::int32_t fault_status=0, reset_status=0, retry_status=0, stop_status=0;
  std::uint64_t fault_safety=0, fault_overruns=0, retained_regions=0;
  std::unique_ptr<Peer> peer;
  if(o.external){peer=std::make_unique<Peer>(a.peer);if(!peer->start())return 2;}
  Replay replay(o.ticks,o.external);
  if(!replay.begin(*s)||!s->controls() || (a.fault==Fault::underflow && !s->stage<4>(6,3))) return failure(__LINE__);
  Oracle oracle(o,a.fault==Fault::underflow);
  Telemetry telemetry;
  std::uint64_t prior_events=0, prior_failures=0, prior_device_failures=0;
  std::array<std::uint64_t,3> prior_actions{};
  std::size_t recoveries=0;
  std::vector<std::byte> recovery;
  const bool recovering_fault=a.fault!=Fault::none && a.fault!=Fault::underflow;
  for(std::size_t t=0;t<o.ticks;++t){
    if(t==6 && (o.campaign==Campaign::overload || recovering_fault)){
      recovery=s->checkpoint(5);if(recovery.empty())return failure(__LINE__);
      rt::Status expected=rt::Status::callback_failed;
      if(a.fault==Fault::overrun && !s->stage<4>(6,4)) return failure(__LINE__);
      if(a.fault==Fault::stop_failure){if(!s->stage<4>(6,9))return failure(__LINE__);expected=rt::Status::device_timeout;}
      if(a.fault==Fault::device_loss){owner->cuda_driver->lose_query=true;expected=rt::Status::device_lost;}
      if(a.fault==Fault::reset_failure){owner->cuda_driver->fail_query=true;expected=rt::Status::device_reset_required;}
      telemetry.expected_device_failure=expected;
      const auto before=s->world;
      status=s->step(t,o.campaign==Campaign::overload);
      fault_status=static_cast<std::int32_t>(status);
      if(status!=expected){std::cerr<<"fault status "<<int(status)<<' '<<s->runtime->last_error()<<'\n';return failure(__LINE__);}
      if(a.fault==Fault::overrun){
        rt::SampledIoChannelStatus info;
        if(!s->runtime->sampled_io_channel_status(s->world.channels[1],info) || info.overruns!=1 ||
           owner->io->first_duplicate!=rt::Status::ok || owner->io->second_duplicate!=rt::Status::invalid_state ||
           s->world.sensor.position!=before.sensor.position || s->world.sensor.velocity!=before.sensor.velocity ||
           s->world.publications[1]!=before.publications[1]) return failure(__LINE__);
        fault_overruns=info.overruns;
      }
      if(a.fault==Fault::device_loss || a.fault==Fault::reset_failure){
        if(s->world.plant.position!=before.plant.position || s->world.plant.velocity!=before.plant.velocity ||
           s->world.calls[1]!=6 || s->world.calls[2]!=6)return failure(__LINE__);
        if(a.fault==Fault::reset_failure)owner->cuda_driver->fail_stream_sync=true;
        reset_status=static_cast<std::int32_t>(owner->physics->reset());
        if(reset_status==0)return failure(__LINE__);
        if(a.fault==Fault::reset_failure){retry_status=static_cast<std::int32_t>(owner->physics->reset());if(retry_status)return failure(__LINE__);}
        owner->cuda_driver->fail_unregister=true;
      }
      if(telemetry.drain(*s->runtime)!=rt::Status::ok){std::cerr<<"fault telemetry stream "<<telemetry.last_stream<<'\n';return failure(__LINE__);}
      if(a.fault==Fault::stop_failure || a.fault==Fault::device_loss || a.fault==Fault::reset_failure){
        stop_status=static_cast<std::int32_t>(owner->close());
        retained_regions=owner->memory->live_count();
        if(!stop_status || !owner->session || !owner->io || retained_regions!=3)return failure(__LINE__);
        if(a.fault==Fault::stop_failure){
          rt::SampledIoChannelStatus info;
          if(!s->runtime->sampled_io_channel_status(s->world.channels[2],info) || info.safety_state!=rt::SampledIoSafetyState::unknown)return failure(__LINE__);
          fault_safety=static_cast<std::uint64_t>(info.safety_state);
          owner->driver->fault=SimulatedDriver::Fault::none;
        }
      }
      if(!totals.close(*owner))return failure(__LINE__);
      owner=std::make_unique<Owner>(o,dispatch);s=owner->session.get();
      if(owner->prepare()!=rt::Status::ok || s->runtime->restore_checkpoint(recovery)!=rt::Status::ok || !s->world.decode())return failure(__LINE__);
      prior_events=telemetry.events;prior_actions=telemetry.action_records;
      prior_failures=telemetry.deadline_failures;prior_device_failures=telemetry.device_failures;
      Telemetry resumed;if(!resumed.resume(*s->runtime,telemetry))return failure(__LINE__);telemetry=resumed;
      replay=Replay(o.ticks,false,6);replay.initial=s->checkpoint(5);if(replay.initial.empty())return failure(__LINE__);
      ++recoveries;
    }
    if(peer && t%3==0)peer->exchange(t,s->world);
    replay.record(*s,t);status=s->step(t);
    if(status!=rt::Status::ok || !oracle.step(t,s->world)){
      std::cerr<<"read channel="<<owner->io->last_read_channel<<" status="<<int(owner->io->last_read.status)<<" decoded="<<owner->io->last_decoded<<" completion="<<int(owner->io->last_read.producer_completion_status)<<" generation="<<owner->io->last_read.generation<<" ";std::cerr<<"step/oracle "<<t<<' '<<int(status)<<' '<<s->runtime->last_error()<<'\n';return failure(__LINE__);
    }
    if(telemetry.drain(*s->runtime)!=rt::Status::ok){std::cerr<<"telemetry "<<t<<'\n';return failure(__LINE__);}
  }
  const auto final=s->world.canonical; const auto calls=s->world.calls;
  const auto planned=s->plan.planned_bytes;
  const auto callback_count=std::accumulate(calls.begin(),calls.end(),std::uint64_t{0});
  const auto physical_callbacks=callback_count+calls[3]+calls[5];
  if(!recoveries && !telemetry.metrics(*s->runtime,o.ticks,physical_callbacks))return failure(__LINE__);
  std::array<rt::SampledIoChannelStatus,4> sampled{};
  for(std::size_t i=0;i<4;++i)if(!s->runtime->sampled_io_channel_status(s->world.channels[i],sampled[i]))return failure(__LINE__);
  rt::LiveControlMailboxInfo mailbox;rt::LiveControlCommitInfo commit;
  if(!s->runtime->live_control_mailbox_info(2601,mailbox)||!s->runtime->live_control_commit_info(commit))return failure(__LINE__);
  if(!replay.seal(*s)||!replay.verify(*s)){std::cerr<<"replay "<<s->runtime->last_error()<<'\n';return failure(__LINE__);}
  if(!totals.close(*owner))return failure(__LINE__);
  if(peer)(void)peer->close();
  const auto peer_result=peer?peer->result:cil::Code::ok, peer_cleanup=peer?peer->cleanup:cil::Code::ok;
  const auto sample_bytes=sizeof(Owner)+sizeof(Totals)+sizeof(Session)+sizeof(Memory)+sizeof(Jobs)+sizeof(Oracle)+sizeof(Replay)+sizeof(Telemetry)+sizeof(Io)+sizeof(SimulatedDriver)+
    (combined?sizeof(cuda::SimulatedDriver)+sizeof(cuda::CudaPhysics)+2*sizeof(cuda::Storage):0)+
    3*sizeof(StateBytes)+(peer?sizeof(Peer)+sizeof(cil::Region):0)+replay.initial.capacity()+replay.active.capacity()+replay.trusted.capacity()+replay.inputs.capacity()*sizeof(rt::ReplayInputRecord)+replay.external.capacity()*3073+recovery.capacity();
  if(sample_bytes>fixed::sample_budget)return failure(__LINE__);
  if(!a.output.empty()){
    fs::create_directories(a.output);
    if(!write(a.output/"state.bin",final)||!write(a.output/"checkpoint.bin",replay.initial)||!write(a.output/"active.bin",replay.active)||!write(a.output/"trusted.bin",replay.trusted))return failure(__LINE__);
    std::ofstream f(a.output/"execution.json");
    f<<"{\"schema\":1,\"contract_sha256\":\""<<fixed::contract_sha256<<"\",\"variant\":\""<<(combined?"sim_combined":"sim_xdma")<<"\",\"dispatch\":\""<<a.dispatch<<"\",\"physical\":\"NOT_RUN\",\"fault\":\""<<fault_names[static_cast<std::size_t>(a.fault)]<<"\",\"mode\":\""<<(o.host?"host":"native")<<"\",\"campaign\":\""<<campaign_names[static_cast<std::size_t>(o.campaign)]<<'"';
#define FIELD(name,value) f<<",\"" name "\":"<<(value)
    FIELD("count",o.count);FIELD("ticks",o.ticks);FIELD("workers",o.workers);FIELD("grain",o.grain);FIELD("external",o.external?"true":"false");
    f<<",\"session_identity\":\""<<session_identity<<'"';
    FIELD("runtime_id",telemetry.runtime_id);FIELD("runtime_planned_bytes",planned);FIELD("sample_owned_bytes",sample_bytes);
    f<<",\"phase_calls\":[";for(std::size_t i=0;i<8;++i)f<<(i?",":"")<<calls[i];f<<']';
    FIELD("physical_callbacks",physical_callbacks);FIELD("physical_phases",10);FIELD("reference_releases",32);
    FIELD("oracle","true");FIELD("cleanup","true");FIELD("trace_events",telemetry.events+prior_events);FIELD("trace_lost",telemetry.lost);FIELD("action_gaps",telemetry.action_gaps);
    FIELD("rate_actions",prior_actions[0]+telemetry.action_records[0]);FIELD("mixed_actions",prior_actions[1]+telemetry.action_records[1]);FIELD("control_actions",prior_actions[2]+telemetry.action_records[2]);
    FIELD("deadline_failures",prior_failures+telemetry.deadline_failures);FIELD("device_failures",prior_device_failures+telemetry.device_failures);
    FIELD("control_accepted",mailbox.accepted);FIELD("control_invalid",mailbox.invalid);FIELD("control_replaced",commit.replaced);FIELD("control_committed",commit.committed);
    FIELD("replay_frames",replay.result.frames_replayed);FIELD("replay_actions",replay.result.actions_compared);FIELD("replay_generations",replay.result.generations_compared);FIELD("recoveries",recoveries);
    FIELD("jobs_accepted",totals.jobs_accepted);FIELD("jobs_completed",totals.jobs_completed);FIELD("memory_acquired",totals.acquisitions);FIELD("memory_released",totals.releases);
    FIELD("xdma_uploads",totals.uploads);FIELD("xdma_downloads",totals.downloads);FIELD("xdma_controls",totals.controls);FIELD("xdma_events",totals.events);FIELD("xdma_safe_acks",totals.safe_acks);FIELD("xdma_initializes",totals.initializes);FIELD("xdma_shutdowns",totals.shutdowns);
    FIELD("sensor_providers",totals.providers[0]);FIELD("actuator_providers",totals.providers[1]);FIELD("sensor_publications",totals.publications[0]);FIELD("actuator_publications",totals.publications[1]);
    FIELD("cuda_providers",totals.cuda_providers);FIELD("cuda_publications",totals.cuda_publications);FIELD("cuda_uploads",totals.cuda_uploads);FIELD("cuda_downloads",totals.cuda_downloads);FIELD("cuda_kernels",totals.cuda_kernels);FIELD("cuda_graphs",totals.cuda_graphs);FIELD("cuda_events",totals.cuda_events);FIELD("cuda_faults",totals.cuda_faults);
    FIELD("fault_status",fault_status);FIELD("reset_status",reset_status);FIELD("reset_retry_status",retry_status);FIELD("stop_status",stop_status);FIELD("fault_safety",fault_safety);FIELD("fault_overruns",fault_overruns);FIELD("retained_regions",retained_regions);
    f<<",\"sampled\":[";
    for(std::size_t i=0;i<4;++i){auto &c=sampled[i];f<<(i?",":"")<<"{\"identity\":"<<26001+i<<",\"ring\":4,\"header_bytes\":120,\"safe_timeout_ns\":"<<(i%2?0:safe_timeout_ns);FIELD("accepted",c.accepted_frames);FIELD("sequence",c.last_sequence);FIELD("stale",c.stale_frames);FIELD("overruns",c.overruns);FIELD("underruns",c.underruns);FIELD("substituted",c.substituted_frames);FIELD("safety",static_cast<unsigned>(c.safety_state));FIELD("status",static_cast<int>(c.last_status));f<<'}';}f<<']';
    f<<",\"peer_status\":\""<<cil::name(peer_result)<<"\",\"peer_cleanup\":\""<<cil::name(peer_cleanup)<<'"';FIELD("peer_responses",peer?peer->responses:0);f<<"}\n";
#undef FIELD
    f.close();if(f.fail())return failure(__LINE__);
  }
  std::cout<<"golden_xdma ticks="<<o.ticks<<" logical_callbacks="<<callback_count<<" physical_callbacks="<<physical_callbacks<<" replay="<<replay.result.frames_replayed<<" cleanup="<<cil::name(peer_cleanup)<<'\n';
  return peer_result==cil::Code::ok&&peer_cleanup==cil::Code::ok?0:2;
}
int portable_main(int argc, char **argv) {
  try {
    Arguments a;
    if (!arguments(argc, argv, a)) {
      std::cerr << "usage: golden_xdma [--dispatch cpu|kernel|graph] [--fault "
                   "none|underflow|overrun|stop_failure|device_loss|reset_failure] [--mode native|host] "
                   "[--count 1..256] "
                   "[--ticks 1..1024] [--workers 1..3] [--grain 1|4|16|64] "
                   "[--campaign NAME] [--output DIRECTORY] [--peer NAME "
                   "--generation N --timeout-ms N]\n";
      return 2;
    }
    return execute(a);
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return failure(__LINE__);
  }
}

} // namespace golden::xdma
