#include "control_support.hpp"
#include <rt/loopback_backend.hpp>
#include <cstring>

namespace rtfw::benchmark::runtime::detail {
namespace {
constexpr std::uint64_t period=3'600'000'000'000;
struct Loopback final:Fixture {
    Case c;
    bool composition{},fault{},replay_mode{},replaying{},valid{true};
    std::size_t frame_bytes;
    std::vector<std::byte> storage,produced,consumed,initial_input,initial_output;
    std::array<std::byte,8> state{},control_payload{};
    rt::CrossRateChannelHandle input_channel{},output_channel{};
    rt::DeviceCommandBatch declaration{};
    rt::DeviceBackendHandle backend_handle{};
    rt::LiveControlProducerHandle control_handle{};
    std::vector<std::byte> initial,artifact,corrupt;
    std::vector<rt::ReplayInputRecord> inputs;
    std::array<rt::MixedRateActionRecord,1024> actions{};
    std::array<rt::RuntimeTraceEvent,1024> traces{};
    std::array<rt::LiveControlActionRecord,1024> control_actions{};
    std::uint64_t produced_count{},providers{},copied_count{},held_count{},signal{1},applied{},optional_calls{};
    std::uint64_t clock_base{1000};
    rt::SampledIoLoopbackBackend backend;
    RuntimeOwner owner;
    void fill_frame(std::span<std::byte> bytes,std::uint64_t channel,std::uint64_t seq,
                    std::uint64_t time,std::uint64_t domain,bool is_initial) noexcept {
        put(bytes.subspan(sizeof(rt::SampledIoFrameHeader)),seq+1);
        rt::SampledIoFrameHeader header;
        header.channel_identity=channel;header.sequence=seq+1;header.release_generation=is_initial?0:seq+1;
        header.sample_count=static_cast<std::uint32_t>(c.bytes/2);
        header.encoding=static_cast<std::uint32_t>(rt::SampledIoEncoding::signed_int16_le);
        header.timestamp_domain_identity=domain;header.first_sample_timestamp=time;
        header.sample_interval_ns=1;header.trigger_identity=404;header.trigger_sequence=seq+1;
        header.calibration_identity=303;
        header.status=static_cast<std::uint32_t>(is_initial?rt::SampledIoFrameStatus::initial:rt::SampledIoFrameStatus::produced);
        header.payload_checksum=rt::sampled_io_payload_checksum(bytes.subspan(sizeof(header)));
        std::memcpy(bytes.data(),&header,sizeof(header));
    }
    static rt::CallbackResult produce(void* opaque,const rt::CallbackContext& ctx) {
        auto& s=*static_cast<Loopback*>(opaque);
        if(!ctx.rate_release) return rt::CallbackResult::error;
        s.fill_frame(s.produced,101,ctx.rate_release->domain_release_sequence,
                     ctx.rate_release->logical_release_ns,1,false);
        if(s.composition) {
            if(!ctx.live_control || ctx.live_control->records.size()!=1) return rt::CallbackResult::error;
            s.valid &= load64(ctx.live_control->records[0].payload)==ctx.frame.frame_index+17;
            if(s.fault) s.owner.clock.now.fetch_add(60'000'000'001,std::memory_order_acq_rel);
        }
        const auto status=ctx.rate_release->publish(s.input_channel,s.produced);
        if(status==rt::Status::ok) ++s.produced_count;
        return status==rt::Status::ok && s.valid?rt::CallbackResult::ok:rt::CallbackResult::error;
    }
    static rt::CallbackResult dispatch(void* opaque,const rt::DeviceCallbackContext& ctx,rt::DeviceCommandBatch& batch) {
        auto& s=*static_cast<Loopback*>(opaque);
        if(!ctx.rate_release) return rt::CallbackResult::error;
        batch=s.declaration;batch.timeout_ns=period/2;batch.signals[0].value=++s.signal;
        ++s.providers;return rt::CallbackResult::ok;
    }
    static rt::CallbackResult consume(void* opaque,const rt::CallbackContext& ctx) {
        auto& s=*static_cast<Loopback*>(opaque);
        if(!ctx.rate_release) return rt::CallbackResult::error;
        rt::CrossRateReadResult read;
        if(ctx.rate_release->copy(s.output_channel,s.consumed,read)!=rt::CrossRateReadStatus::ok) return rt::CallbackResult::error;
        const auto time=ctx.rate_release->logical_release_ns;
        const auto device_sequence=time/(period*2);
        const auto producer_sequence=device_sequence*2;
        rt::SampledIoFrameHeader header;std::memcpy(&header,s.consumed.data(),sizeof(header));
        s.valid &= read.provenance==rt::CrossRateSampleProvenance::produced &&
            read.producer_release_sequence==device_sequence && read.producer_substep_ordinal==0 &&
            read.producer_timestamp_domain_identity==7 && header.channel_identity==202 &&
            header.timestamp_domain_identity==7 && header.payload_checksum==rt::sampled_io_payload_checksum(std::span<const std::byte>(s.consumed).subspan(sizeof(header))) &&
            matches(std::span<const std::byte>(s.consumed).subspan(sizeof(header)),producer_sequence+1);
        ++s.copied_count;s.held_count+=read.held;
        store64(s.state,load64(s.state)*17+producer_sequence+1+ctx.rate_release->domain_release_sequence);
        return s.valid?rt::CallbackResult::ok:rt::CallbackResult::error;
    }
    static rt::CallbackResult optional(void* opaque,const rt::CallbackContext&) {
        ++static_cast<Loopback*>(opaque)->optional_calls;return rt::CallbackResult::ok;
    }
    static rt::CallbackResult apply(void* opaque,const rt::ReplayInputView& input) {
        auto& s=*static_cast<Loopback*>(opaque);
        s.valid &= input.payload.empty() && input.input_type==23 && input.frame.frame_index==s.applied+1;
        ++s.applied;return s.valid?rt::CallbackResult::ok:rt::CallbackResult::error;
    }
    explicit Loopback(const Case& value):c(value),composition(c.family==Family::composition),
        fault(composition && c.variant==1),replay_mode(c.family==Family::replay || composition),
        frame_bytes(sizeof(rt::SampledIoFrameHeader)+c.bytes),storage(frame_bytes*c.capacity*2),
        produced(frame_bytes),consumed(frame_bytes),initial_input(frame_bytes),initial_output(frame_bytes),
        inputs(replay_mode?c.count:0),backend({8,1,65536,1,7}) {
        okay(backend.add_route({17,101,202,7,303,404}));
        auto cfg=config();cfg.device_backend_capacity=1;cfg.device_buffer_capacity=1;
        cfg.device_outstanding_capacity=4;cfg.device_completion_batch=4;cfg.trace_capacity=composition?1024:0;
        if(fault){cfg.watchdog_timeout_ns=60'000'000'000;cfg.watchdog_max_degradation_level=3;}
        okay(owner.rt.configure(cfg));okay(owner.rt.set_rate_execution_policy({64,23,1,1,1024}));
        okay(owner.rt.set_mixed_rate_closure_policy({23,1024,1024,1024*1024,64,
            rt::MixedRateOverflowPolicy::overwrite_committed,true,true,{}}));
        if(composition) configure_controls(owner.rt,1,1,8,8,true,true);
        okay(owner.rt.register_state({"loopback-observation",1,state}));
        okay(owner.rt.register_device_backend(backend.hal_v2_registration(),backend_handle));
        rt::DeviceMemoryDomainHandle memory_domain;rt::HalV2MemoryDomain memory;
        require(owner.rt.device_memory_domain_at(backend_handle,0,memory_domain,memory));
        rt::DeviceBufferHandle buffer;
        okay(owner.rt.register_device_buffer({"loopback-slots",backend_handle,memory_domain,storage,{},storage.size(),
            rt::HalV2MemoryOwnership::borrowed_host,RTFW_DEVICE_BUFFER_HOST_READ|RTFW_DEVICE_BUFFER_HOST_WRITE|
            RTFW_DEVICE_BUFFER_DEVICE_READ|RTFW_DEVICE_BUFFER_DEVICE_WRITE,rt::HalV2MemoryCoherency::host_coherent,
            rt::hal_v2_memory_sync_none},buffer));
        rt::DeviceTimelineHandle timeline;okay(owner.rt.register_device_timeline({"loopback-timeline",backend_handle,1},timeline));
        declaration.command_count=1;declaration.signal_count=1;
        auto& command=declaration.commands[0];command.kind=static_cast<std::uint32_t>(rt::HalV2CommandKind::dispatch);
        command.opcode=17;command.buffer_count=2;
        command.buffers[0]={buffer.value,RTFW_DEVICE_ACCESS_READ,0,0,frame_bytes*c.capacity};
        command.buffers[1]={buffer.value,RTFW_DEVICE_ACCESS_WRITE,0,frame_bytes*c.capacity,frame_bytes*c.capacity};
        declaration.signals[0].timeline_handle=timeline.value;
        rt::PhaseHandle producer,device,consumer;rt::RateDomainHandle producer_rate,device_rate,consumer_rate;
        okay(owner.rt.register_callback({"canonical-producer",produce,this},producer));
        okay(owner.rt.register_device_batch_phase({"loopback-sensor",backend_handle,dispatch,this,declaration},device));
        okay(owner.rt.register_callback({"canonical-consumer",consume,this},consumer));
        okay(owner.rt.register_rate_domain({"producer-rate",period,1,period,1},producer_rate));
        okay(owner.rt.register_rate_domain({"sensor-rate",period*2,1,period*2,1},device_rate));
        okay(owner.rt.register_rate_domain({"consumer-rate",period,1,period,1},consumer_rate));
        okay(owner.rt.bind_phase_to_rate_domain(producer,producer_rate));
        const std::array roles{rt::DeviceRatePayloadRole::input,rt::DeviceRatePayloadRole::output};
        okay(owner.rt.bind_device_phase_to_rate_domain({device,device_rate,period/2,static_cast<std::uint32_t>(c.capacity),roles}));
        okay(owner.rt.bind_phase_to_rate_domain(consumer,consumer_rate));
        if(fault) for(std::size_t i=0;i<2;++i) {
            rt::PhaseHandle phase;rt::RateDomainHandle domain;
            okay(owner.rt.register_callback({"optional-"+std::to_string(i),optional,this},phase));
            okay(owner.rt.register_rate_domain({"optional-rate-"+std::to_string(i),period,1,10,1,
                rt::RateCriticality::background,true,rt::RateLateAction::skip,0},domain));
            okay(owner.rt.bind_phase_to_rate_domain(phase,domain));
        }
        fill_frame(initial_input,101,0,0,1,true);fill_frame(initial_output,202,0,0,7,true);
        okay(owner.rt.register_cross_rate_channel({"cpu-to-device",producer,device,frame_bytes,initial_input,
            rt::CrossRateMode::sample_and_hold,period*2,{},{0,frame_bytes}},input_channel));
        okay(owner.rt.register_cross_rate_channel({"device-to-cpu",device,consumer,frame_bytes,initial_output,
            rt::CrossRateMode::sample_and_hold,period*2,{1,frame_bytes},{}},output_channel));
        rt::SampledIoChannelRegistration sampled;
        sampled.channel=output_channel;sampled.channel_identity=202;
        sampled.element_count=1;sampled.samples_per_frame=static_cast<std::uint32_t>(c.bytes/2);
        sampled.units_identity=202;sampled.calibration_identity=303;sampled.sample_period_ns=1;
        sampled.timestamp_domain_identity=7;sampled.clock_domain_identity=1;sampled.trigger_identity=404;
        sampled.ring_capacity=rt::cross_rate_snapshot_slot_count;sampled.initial_sequence=1;
        sampled.maximum_age_ns=period*2;sampled.initial_frame=initial_output;
        if(c.capacity<=rt::cross_rate_snapshot_slot_count) okay(owner.rt.register_sampled_io_channel(sampled));
        finalized(owner);
        if(composition) okay(owner.rt.live_control_producer_handle(101,1001,control_handle));
        if(replay_mode) initial=checkpoint(owner.rt);
    }
    Measures execute(std::uint64_t ordinal) {
        Measures m;const auto previous=backend.stats();const auto before_produced=produced_count,before_providers=providers,before_copied=copied_count;
        const auto steps=replay_mode?c.count:1;
        const auto duration=replay_mode?period*2:c.count*period*2;
        const auto first=ordinal*steps;
        for(std::uint64_t i=0;i<steps;++i) {
            const auto index=first+i+1,release=clock_base+(first+i)*duration;
            if(composition) {
                store64(control_payload,index+17);
                auto update=update_record(control_handle,index,control_payload,index);rt::LiveControlAdmissionResult result;
                okay(owner.rt.stage_live_control_update(control_handle,update,control_payload,result));
                require(result==rt::LiveControlAdmissionResult::accepted);++m.transitions;
                if(fault) {
                    update.producer_sequence=index+1;update.payload_digest^=1;
                    okay(owner.rt.stage_live_control_update(control_handle,update,control_payload,result));
                    require(result==rt::LiveControlAdmissionResult::invalid);++m.rejected;
                }
            }
            owner.clock.now=release;rt::StepResult result;
            const auto input=frame(index,duration,release);okay(owner.rt.step(input,&result));
            if(replay_mode) inputs[i]={input,23,{}};
            ++m.operations;m.callbacks+=result.callbacks_executed;
            if(fault) require(result.watchdog_fired);
        }
        const auto after=backend.stats();
        const auto releases=c.count;
        require(after.submissions-previous.submissions==releases && after.completions-previous.completions==releases &&
            after.frames_copied-previous.frames_copied==releases && after.rejected==previous.rejected);
        require(providers-before_providers==releases && produced_count-before_produced==2*releases && copied_count-before_copied==2*releases);
        rt::DeviceTimelineInfo timeline;require(owner.rt.device_timeline_at(backend_handle,0,timeline));
        require(timeline.completed_value==signal && timeline.last_accepted_value==signal);
        rt::RuntimeMetricSnapshot snapshot;okay(owner.rt.metrics_snapshot(rt::RuntimeMetricWindow::cumulative,nullptr,snapshot));
        require(snapshot.samples[static_cast<std::size_t>(rt::RuntimeMetricId::device_outstanding)].value==0);
        m.records=copied_count-before_copied;m.bytes=m.records*c.bytes;m.checksum=load64(state);m.correct=valid;
        return m;
    }
    Measures run(std::uint64_t ordinal) override {
        if(!replay_mode) return execute(ordinal);
        okay(owner.rt.restore_checkpoint(initial));
        std::fill(storage.begin(),storage.end(),std::byte{0});
        valid=true;applied=0;produced_count=providers=copied_count=held_count=0;
        clock_base=owner.clock.now.load()+period*2;
        if(composition) okay(owner.rt.live_control_producer_handle(101,1001,control_handle));
        const auto invocation_checkpoint=checkpoint(owner.rt);
        auto m=execute(0);const auto expected=load64(state);
        std::uint64_t oracle=0;
        for(std::uint64_t i=0;i<2*c.count;++i) oracle=oracle*17+(i/2)*2+1+i;
        require(expected==oracle);
        rt::ArtifactWriteResult write;
        require(owner.rt.write_active_replay_artifact(invocation_checkpoint,inputs,{},write)==rt::Status::capacity_exceeded);
        require(write.required_bytes<=4U*1024U*1024U);artifact.resize(write.required_bytes);
        okay(owner.rt.write_active_replay_artifact(invocation_checkpoint,inputs,artifact,write));
        rt::ActiveReplayMetadata metadata;okay(rt::inspect_active_replay_artifact(artifact,metadata));
        if(composition) {
            std::vector<std::byte> nested=artifact;
            require(owner.rt.write_live_control_replay_artifact(invocation_checkpoint,nested,rt::LiveControlNestedArtifactKind::active_replay,{},write)==rt::Status::capacity_exceeded);
            require(write.required_bytes<=4U*1024U*1024U);artifact.resize(write.required_bytes);
            okay(owner.rt.write_live_control_replay_artifact(invocation_checkpoint,nested,rt::LiveControlNestedArtifactKind::active_replay,artifact,write));
        }
        corrupt=artifact;corrupt.back()^=std::byte{1};
        const auto bad=composition?owner.rt.replay_live_control(corrupt,apply,this):owner.rt.replay_active(corrupt,apply,this);
        require(bad!=rt::Status::ok && load64(state)==expected && applied==0);++m.rejected;
        const auto before_copied=copied_count;
        // Borrowed device buffer contents are application-owned, outside the
        // registered checkpoint state. Restore their declared initial bytes
        // after terminal drain before replay; include this work in invoke.
        std::fill(storage.begin(),storage.end(),std::byte{0});
        if(composition) {
            rt::LiveControlReplayResult result;{ const auto status=owner.rt.replay_live_control(artifact,apply,this,&result); if(status!=rt::Status::ok) throw std::runtime_error(std::string(owner.rt.last_error())+" frame="+std::to_string(result.mismatch_frame_index)+" action="+std::to_string(result.mismatch_action_sequence)+" inputs="+std::to_string(applied)+" copies="+std::to_string(copied_count)+" valid="+std::to_string(valid)); }
            require(result.frames_replayed==c.count && result.generations_compared==c.count && result.mismatch_status==rt::Status::ok);
        } else {
            rt::ActiveReplayResult result;{ const auto status=owner.rt.replay_active(artifact,apply,this,&result); if(status!=rt::Status::ok) throw std::runtime_error(std::string(owner.rt.last_error())+" action="+std::to_string(result.mismatch_sequence)); }
            require(result.replay.frames_replayed==c.count && result.actions_compared==metadata.action_record_count && result.mismatch_status==rt::Status::ok);
        }
        require(load64(state)==expected && applied==c.count && copied_count-before_copied==2*c.count);
        rt::MixedRateActionCursor cursor;rt::MixedRateActionReadResult read;
        okay(owner.rt.read_mixed_rate_actions(cursor,actions,read));
        m.mixed_actions=read.records_read;
        require(read.lost_records==0);
        for(std::size_t i=0;i<read.records_read;++i) require(actions[i].schema_version==1);
        if(composition) {
            rt::RuntimeTraceCursor trace_cursor;rt::RuntimeTraceReadResult trace_read;
            okay(owner.rt.read_trace(trace_cursor,traces,trace_read));require(trace_read.events_read>0);
            rt::LiveControlActionCursor control_cursor;rt::LiveControlActionReadResult control_read;
            okay(owner.rt.read_live_control_actions(control_cursor,control_actions,control_read));require(control_read.records_read>0);
            m.control_actions=control_read.records_read;
        }
        m.operations+=c.count;m.records+=copied_count-before_copied;m.bytes+=artifact.size();m.correct &= valid;
        return m;
    }
    rt::Status finish() noexcept override{return owner.stop();}
};
}
std::unique_ptr<Fixture> make_device(const Case& c){return std::make_unique<Loopback>(c);}
std::unique_ptr<Fixture> make_composition(const Case& c){return std::make_unique<Loopback>(c);}
} // namespace rtfw::benchmark::runtime::detail
