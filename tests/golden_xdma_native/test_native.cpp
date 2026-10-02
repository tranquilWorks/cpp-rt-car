#include "../../samples/golden_xdma_native/session.hpp"
#include "../../samples/golden_xdma/simulated_driver.hpp"
#include "../golden_cuda/allocation.hpp"
#include "../../samples/golden_xdma/oracle.hpp"
#include <iostream>
namespace gn=golden::xdma::native;
namespace gx=golden::xdma;
#define CHECK(x) do { if (!(x)) { std::cerr<<"FAIL "<<__LINE__<<": "<<#x<<'\n';return false;} } while(false)
gn::Configuration config() {
  gn::Configuration c;c.tuple="software-fixture";c.bitstream=std::string(64,'a');
  for(std::size_t i=0;i<7;++i)c.paths[i]="/nonexistent/golden-native-"+std::to_string(i);
  c.input={0,32768};c.output={8192,40960};return c;
}
struct Fake {
  gx::SimulatedDriver card{16};
  rt::XdmaDriverApi api() {
    auto a=card.api();a.user_data=this;
    a.initialize=[](void *p) noexcept {auto &s=static_cast<Fake*>(p)->card;return s.api().initialize(&s);};
    a.shutdown=[](void *p) noexcept {auto &s=static_cast<Fake*>(p)->card;return s.api().shutdown(&s);};
    a.reset=[](void *p) noexcept {auto &s=static_cast<Fake*>(p)->card;return s.api().reset(&s);};
    a.request_stop=[](void *p) noexcept {auto &s=static_cast<Fake*>(p)->card;return s.api().request_stop(&s);};
    a.monotonic_time_ns=[](void *p) noexcept {return static_cast<Fake*>(p)->card.now.load();};
    a.control_read32=[](void *p,std::uint32_t o) noexcept {auto &s=static_cast<Fake*>(p)->card;return s.api().control_read32(&s,o);};
    a.control_write32=[](void *p,std::uint32_t o,std::uint32_t v) noexcept {auto &s=static_cast<Fake*>(p)->card;return s.api().control_write32(&s,o,v);};
    a.wait_user_event=[](void *p,std::uint32_t i,std::uint64_t t) noexcept {auto &s=static_cast<Fake*>(p)->card;return s.api().wait_user_event(&s,i,t);};
    a.transfer=[](void *p,rt::XdmaDirection d,std::uint32_t ch,std::uint64_t off,void *h,std::uint64_t n) noexcept {
      auto &s=static_cast<Fake*>(p)->card;
      if(ch>1 || (off!=ch*32768ull && off!=ch*32768ull+8192))
        return rt::XdmaTransferResult{rt::XdmaDriverResult::invalid_value,0};
      return s.api().transfer(&s,d,ch,off-ch*32768ull,h,n);
    };return a;
  }
};
bool positive() {
  auto c=config();CHECK(c.valid());
  Fake f;golden::Clock clock;
  auto memory=std::make_unique<golden::Memory>();
  auto io=std::make_unique<gn::Io>(f.api(),c,1000);
  auto s=std::make_unique<gn::Session>(golden::Options{},*memory,*io,clock,1000);
  CHECK(s->prepare()==rt::Status::ok);
  CHECK(io->native_capabilities().deterministic_mock==0);
  CHECK(s->plan.phase_count==10 && s->plan.sampled_io_channel_count==4);
  CHECK(s->controls());
  rtfw_physics_allocation::begin();
  auto result=s->step(0);
  const auto allocations=rtfw_physics_allocation::end();
  if(result!=rt::Status::ok || allocations)std::cerr<<"step status="<<int(result)<<" allocations="<<allocations<<" error="<<s->runtime->last_error()<<"\n";
  CHECK(result==rt::Status::ok && allocations==0);
  gx::Oracle oracle(golden::Options{});CHECK(oracle.step(0,s->world));
  CHECK(f.card.downloads>=2 && f.card.controls==f.card.events);
  CHECK(s->close()==rt::Status::ok && !f.card.live && memory->live_count()==0);
  CHECK(s->close()==rt::Status::ok);
  return true;
}
bool refusal(gx::SimulatedDriver::Fault fault) {
  Fake f;golden::Clock clock;
  auto memory=std::make_unique<golden::Memory>();
  gn::Io io(f.api(),config(),1000);gn::Session s(golden::Options{},*memory,io,clock,1000);
  CHECK(s.prepare()==rt::Status::ok);
  f.card.fault_lane=0;f.card.fault=fault;
  CHECK(s.step(0)!=rt::Status::ok);
  f.card.fault=gx::SimulatedDriver::Fault::none;
  CHECK(s.close()==rt::Status::ok && !f.card.live && memory->live_count()==0);
  return true;
}
bool ownership() {
  for (unsigned mode=0;mode<3;++mode) {
    Fake f;golden::Clock clock;auto memory=std::make_unique<golden::Memory>();
    auto c=config();if(mode==2)c.output[0]=0;
    gn::Io io(f.api(),c,1000);gn::Session s(golden::Options{},*memory,io,clock,1000);
    if(mode==0)f.card.fail_after_acquire=true;
    const auto prepared=s.prepare();
    if(mode==1) {
      CHECK(prepared==rt::Status::ok);
      f.card.fail_shutdown=true;
      CHECK(s.close()!=rt::Status::ok && f.card.live && memory->live_count()>0);
    } else CHECK(prepared!=rt::Status::ok);
    CHECK(s.close()==rt::Status::ok && !f.card.live && memory->live_count()==0);
    if(mode==2)CHECK(f.card.initializes==0 && f.card.uploads==0);
  }
  return true;
}
bool formats() {
  auto c=config();auto bad=c;bad.output[0]=0;CHECK(!bad.valid());bad=c;bad.input[1]=UINT64_MAX;CHECK(!bad.valid());
  Fake f;gn::Io io(f.api(),c,2000);
  golden::Frame sample{},application{};
  gx::sampled_header(sample,1,2,1,2000,rt::SampledIoFrameStatus::produced);
  CHECK(io.decode(sample,application,1,16));
  gx::sampled_header(sample,1,2,1,1999,rt::SampledIoFrameStatus::produced);
  CHECK(!io.decode(sample,application,1,16));
  gx::sampled_header(sample,1,2,1,2001,rt::SampledIoFrameStatus::produced);
  CHECK(!io.decode(sample,application,1,16));
  gx::sampled_header(sample,1,2,1,2000,rt::SampledIoFrameStatus::produced);
  sample[golden::header_bytes]^=std::byte{1};CHECK(!io.decode(sample,application,1,16));
  CHECK(f.card.initializes==0 && f.card.uploads==0);return true;
}

bool preparation() {
  Fake f;golden::Clock clock;auto memory=std::make_unique<golden::Memory>();
  gn::Io io(f.api(),config(),1000);gn::Session s(golden::Options{},*memory,io,clock,1000);
  CHECK(s.prepare()==rt::Status::ok);
  CHECK(io.native_capabilities().deterministic_mock==0 && s.plan.phase_count==10 &&
        s.plan.sampled_io_channel_count==4 && s.plan.device_buffer_count==6);
  CHECK(f.card.safe_acks>=2 && f.card.downloads>=2);
  CHECK(s.close()==rt::Status::ok && !f.card.live && memory->live_count()==0);
  return true;
}
bool protocol(gx::SimulatedDriver::Fault fault) {
  // The native Runtime's 500-us deadline is intentionally not an instrumented
  // host performance oracle. Exercise its actual backend command protocol with
  // the injected driver clock, independently of wall-clock scheduling speed.
  Fake driver;
  golden::Frame input{},output{},application{};
  std::array<std::byte,4> ack{};
  gx::sampled_header(input,0,1,0,0,rt::SampledIoFrameStatus::initial);
  gx::sampled_header(output,1,2,1,1000,rt::SampledIoFrameStatus::produced);
  rt::XdmaBackendConfig config;
  config.queue_capacity=1;config.buffer_capacity=3;config.worker_count=1;
  config.max_transfer_bytes=config.max_buffer_bytes=golden::maximum_frame_bytes;
  config.control_aperture_bytes=8;config.user_event_count=2;
  rt::XdmaDeviceBackend backend(driver.api(),config);
  auto reg=backend.hal_v2_registration("golden.native.protocol");
  const auto api=reg.api;const auto commands=*reg.command_timeline;
  rt::HalV2InitializeConfig init;init.requested_in_flight=1;init.requested_registered_buffers=3;
  CHECK(api.initialize(api.instance,&init)==rt::HalV2Status::ok);
  std::array<std::span<std::byte>,3> storage{golden::frame_span(input,0),golden::frame_span(output,1),ack};
  std::array<std::uint64_t,3> tokens{};
  for(std::size_t i=0;i<3;++i) {
    rt::HalV2BufferRegistration b;b.data=storage[i].data();b.bytes=storage[i].size();
    b.name[0]=static_cast<char>('a'+i);
    b.flags=RTFW_DEVICE_BUFFER_HOST_READ|RTFW_DEVICE_BUFFER_HOST_WRITE|
        RTFW_DEVICE_BUFFER_DEVICE_READ|RTFW_DEVICE_BUFFER_DEVICE_WRITE;
    CHECK(api.register_buffer(api.instance,&b,&tokens[i])==rt::HalV2Status::ok);
  }
  rt::DeviceCommandBatch batch;batch.batch_id=1;batch.timeout_ns=gx::completion_ns;
  batch.command_count=5;batch.signal_count=1;batch.signals[0].timeline_handle=1;batch.signals[0].value=1;
  const auto transfer=[&](std::size_t at,std::size_t ref,bool down) {
    auto &c=batch.commands[at];c.kind=static_cast<std::uint32_t>(rt::HalV2CommandKind::dispatch);
    c.opcode=down?rt::xdma_device_opcode_card_to_host:rt::xdma_device_opcode_host_to_card;
    c.buffer_count=1;c.buffers[0].buffer_token=tokens[ref];c.buffers[0].bytes=storage[ref].size();
    c.buffers[0].access=down?RTFW_DEVICE_ACCESS_WRITE:RTFW_DEVICE_ACCESS_READ;
    rt::XdmaTransfer t;t.device_offset=ref?8192:0;c.payload_size=sizeof(t);std::memcpy(c.payload.data(),&t,sizeof(t));
  };
  transfer(0,0,false);transfer(1,1,false);transfer(4,1,true);
  CHECK(rt::set_xdma_control_write(batch.commands[2],0,0));
  rt::HalV2BufferReference event;event.buffer_token=tokens[2];event.bytes=4;event.access=RTFW_DEVICE_ACCESS_WRITE;
  CHECK(rt::set_xdma_user_event_wait(batch.commands[3],0,event));
  driver.card.fault_lane=0;driver.card.fault=fault;
  rt::HalV2BatchCompletion completion;std::uint64_t n=0;
  const auto limit=std::chrono::steady_clock::now()+std::chrono::seconds(2);
  rtfw_physics_allocation::begin();
  const auto submitted=commands.submit(commands.instance,&batch);
  rt::HalV2Status polled=rt::HalV2Status::ok;
  while(!n && polled==rt::HalV2Status::ok && std::chrono::steady_clock::now()<limit) {
    polled=commands.poll(commands.instance,&completion,1,&n);std::this_thread::yield();
  }
  const auto allocations=rtfw_physics_allocation::end();
  CHECK(submitted==rt::HalV2Status::ok && polled==rt::HalV2Status::ok && n==1 && allocations==0);
  rt::SampledIoFrameHeader h;std::memcpy(&h,output.data(),sizeof(h));
  gn::Io codec(driver.api(),::config(),1000);
  const bool valid=completion.status==static_cast<std::int32_t>(rt::HalV2Status::ok) &&
      completion.batch_id==1 && ack[0]==std::byte{1} && h.sequence==2 && h.trigger_sequence==2 &&
      h.release_generation==1 && h.first_sample_timestamp==1000 && codec.decode(output,application,1,16);
  CHECK(valid==(fault==gx::SimulatedDriver::Fault::none));
  if(valid)CHECK(driver.card.uploads==2 && driver.card.downloads==1 && driver.card.controls==1 && driver.card.events==1);
  for(auto token:tokens)CHECK(api.unregister_buffer(api.instance,token)==rt::HalV2Status::ok);
  CHECK(api.shutdown(api.instance)==rt::HalV2Status::ok && !driver.card.live);
  return true;
}
int main(int argc,char **argv) {
  if(argc==2 && std::string_view(argv[1])=="--frame") {
    // Explicit execution probe: requires the unchanged native 500-us deadline.
    // Retain its assertions even where instrumented-host timing cannot pass.
    if(!positive())return 1;
    for(auto f:{gx::SimulatedDriver::Fault::short_transfer,gx::SimulatedDriver::Fault::header_corrupt,
                gx::SimulatedDriver::Fault::payload_corrupt,gx::SimulatedDriver::Fault::stale_sequence,
                gx::SimulatedDriver::Fault::missing_ack})if(!refusal(f))return 1;
    std::cout<<"PASS optional native frame probe (no timing qualification)\n";return 0;
  }
  if(argc!=1)return 2;
  if(!formats() || !preparation() || !ownership())return 1;
  for(auto f:{gx::SimulatedDriver::Fault::none,gx::SimulatedDriver::Fault::short_transfer,
              gx::SimulatedDriver::Fault::header_corrupt,gx::SimulatedDriver::Fault::payload_corrupt,
              gx::SimulatedDriver::Fault::stale_sequence,gx::SimulatedDriver::Fault::missing_ack})
    if(!protocol(f))return 1;
  std::cout<<"PASS native preparation, golden protocol, zero allocation, format/ownership\n";
}
