#include "control_support.hpp"
#include <rt/loopback_backend.hpp>
#include <cstring>

namespace rtfw::benchmark::runtime::detail {
namespace {
constexpr std::uint64_t period=3'600'000'000'000;
struct Loopback final:Fixture {
    Case c;
    bool composition{},fault{},shedding{},replay_mode{},replaying{},valid{true};
    std::size_t frame_bytes;
    std::vector<std::byte> storage,produced,consumed,initial_input,initial_output;
    std::array<std::byte,8> state{},control_payload{};
    std::array<std::byte,8> foreign_state{};
    rt::CrossRateChannelHandle input_channel{},output_channel{};
    rt::DeviceCommandBatch declaration{};
    rt::DeviceBackendHandle backend_handle{};
    rt::LiveControlProducerHandle control_handle{};
    std::vector<std::byte> initial,artifact,corrupt;
    std::vector<rt::ReplayInputRecord> inputs;
    std::array<rt::RateActionRecord,1024> rate_actions{};
    std::array<rt::MixedRateActionRecord,1024> actions{};
    std::array<rt::RuntimeTraceEvent,1024> traces{};
    std::array<rt::LiveControlActionRecord,1024> control_actions{};
    std::uint64_t produced_count{},providers{},copied_count{},held_count{},signal{1},applied{},optional_calls{};
    std::uint64_t trigger_calls{};
    std::array<std::uint64_t,2> optional_by_rank{};
    std::array<rt::RateDomainHandle,2> optional_domains{};
    std::uint64_t clock_base{1000};
    rt::SampledIoLoopbackBackend backend;
    RuntimeOwner owner;
    RuntimeOwner foreign;
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
        batch=s.declaration;
        // Timeout fixtures deliberately retain a loopback completion until the
        // existing service deadline expires. This is declared lifecycle work,
        // with no elapsed-time assertion or benchmark-duration threshold.
        batch.timeout_ns=std::string_view(s.c.mode)=="timeout"?1'000'000:period/2;
        batch.signals[0].value=++s.signal;
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
    static rt::CallbackResult optional(void* opaque,const rt::CallbackContext& ctx) {
        auto& s=*static_cast<Loopback*>(opaque);++s.optional_calls;
        if(s.fault && ctx.rate_release) for(std::size_t i=0;i<2;++i)
            if(ctx.rate_release->domain==s.optional_domains[i]) ++s.optional_by_rank[i];
        return rt::CallbackResult::ok;
    }
    static rt::CallbackResult trigger(void* opaque,const rt::CallbackContext&) {
        ++static_cast<Loopback*>(opaque)->trigger_calls;return rt::CallbackResult::ok;
    }
    static rt::CallbackResult apply(void* opaque,const rt::ReplayInputView& input) {
        auto& s=*static_cast<Loopback*>(opaque);
        s.valid &= input.payload.empty() && input.input_type==23 && input.frame.frame_index==s.applied+1;
        ++s.applied;return s.valid?rt::CallbackResult::ok:rt::CallbackResult::error;
    }
    explicit Loopback(const Case& value):c(value),composition(c.family==Family::composition),
        fault(composition && c.variant!=0),shedding(composition && c.variant==2),replay_mode(c.family==Family::replay || composition),
        frame_bytes(sizeof(rt::SampledIoFrameHeader)+c.bytes),storage(frame_bytes*c.capacity*2),
        produced(frame_bytes),consumed(frame_bytes),initial_input(frame_bytes),initial_output(frame_bytes),
        inputs(replay_mode?c.count:0),backend({8,1,65536,1,7}) {
        okay(backend.add_route({17,101,202,7,303,404}));
        auto cfg=config();if(replay_mode) cfg.memory_budget_bytes=480U*1024U*1024U;cfg.device_backend_capacity=1;cfg.device_buffer_capacity=1;
        cfg.device_outstanding_capacity=4;cfg.device_completion_batch=4;cfg.trace_capacity=composition?1024:0;
        if(fault){cfg.watchdog_timeout_ns=60'000'000'000;cfg.watchdog_max_degradation_level=3;}
        okay(owner.rt.configure(cfg));okay(owner.rt.set_rate_execution_policy({64,23,1,shedding?3U:1U,1024}));
        okay(owner.rt.set_mixed_rate_closure_policy({23,1024,1024,1024*1024,64,
            rt::MixedRateOverflowPolicy::overwrite_committed,true,true,{}}));
        if(composition) {
            configure_controls(owner.rt,1,1,8,8,true,true);
            // Composition replay includes rejected admissions as well as the
            // original accepted payloads. Select the integrated public v2
            // retention API explicitly; ordinary replay fixtures remain v1.
            rt::LiveControlReplayRetentionPolicy retention;
            retention.policy_identity=230302;
            retention.admission_capacity=4*c.count+8;
            retention.payload_capacity_bytes=16*c.count+64;
            okay(owner.rt.set_live_control_replay_retention_policy(retention));
        }
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
        if(shedding) {
            rt::PhaseHandle phase;rt::RateDomainHandle domain;
            okay(owner.rt.register_callback({"shedding-trigger",trigger,this},phase));
            okay(owner.rt.register_rate_domain({"shedding-trigger",period,1,3,1,
                rt::RateCriticality::normal,false,rt::RateLateAction::skip,0},domain));
            okay(owner.rt.bind_phase_to_rate_domain(phase,domain));
        }
        if(fault) for(std::size_t i=0;i<2;++i) {
            rt::PhaseHandle phase;rt::RateDomainHandle domain;
            okay(owner.rt.register_callback({"optional-"+std::to_string(i),optional,this},phase));
            okay(owner.rt.register_rate_domain({"optional-rate-"+std::to_string(i),period,1,10,1,
                rt::RateCriticality::background,true,rt::RateLateAction::skip,0},domain));
            okay(owner.rt.bind_phase_to_rate_domain(phase,domain));optional_domains[i]=domain;
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
        if(replay_mode) {
            initial=checkpoint(owner.rt);
            // Independently finalized foreign topology: well-formed artifacts
            // must be rejected before registered state or callbacks change.
            auto foreign_cfg=config();foreign_cfg.memory_budget_bytes=32U*1024U*1024U;
            okay(foreign.rt.configure(foreign_cfg));
            okay(foreign.rt.set_rate_execution_policy({64,23,1,1,1024}));
            okay(foreign.rt.set_mixed_rate_closure_policy({23,1024,1024,1024*1024,64,
                rt::MixedRateOverflowPolicy::overwrite_committed,true,true,{}}));
            if(composition) {
                configure_controls(foreign.rt,1,1,8,8,true,true);
                rt::LiveControlReplayRetentionPolicy retention;
                retention.policy_identity=230302;retention.admission_capacity=4*c.count+8;
                retention.payload_capacity_bytes=16*c.count+64;
                okay(foreign.rt.set_live_control_replay_retention_policy(retention));
            }
            okay(foreign.rt.register_state({"foreign-loopback-observation",1,foreign_state}));
            rt::PhaseHandle phase;rt::RateDomainHandle domain;
            okay(foreign.rt.register_callback({"foreign-topology",optional,this},phase));
            okay(foreign.rt.register_rate_domain({"foreign-rate",period,1,period,1},domain));
            okay(foreign.rt.bind_phase_to_rate_domain(phase,domain));
            finalized(foreign);
        }
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
                require(result==rt::LiveControlAdmissionResult::accepted);++m.admissions;
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
            if(shedding) require(result.rate.shed_transitions==1 && result.rate.recovery_transitions==1 &&
                result.rate.currently_shed_domains==0 && result.rate.optional_executed_domain_releases==2);
            else require(result.rate.shed_transitions==0 && result.rate.recovery_transitions==0);
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
        const auto optional_before=optional_calls,trigger_before=trigger_calls;
        const auto ranks_before=optional_by_rank;
        rt::RateTelemetryMetadata rate_start;okay(owner.rt.rate_telemetry_metadata(rate_start));
        rt::RateCounterSnapshot counters_before;okay(owner.rt.rate_counters_snapshot(counters_before));
        auto m=execute(0);const auto expected=load64(state);
        require(optional_calls-optional_before==(fault?2*c.count:0) && trigger_calls-trigger_before==(shedding?c.count:0));
        if(fault) for(std::size_t i=0;i<2;++i) require(optional_by_rank[i]-ranks_before[i]==c.count);
        rt::RateTelemetryCursor rate_cursor;rate_cursor.runtime_id=rate_start.runtime_id;rate_cursor.next_sequence=rate_start.next_sequence;
        rt::RateTelemetryReadResult rate_read;okay(owner.rt.read_rate_actions(rate_cursor,rate_actions,rate_read));
        require(rate_read.lost_records==0);
        std::uint64_t transition_count=0;
        for(std::size_t i=0;i<rate_read.records_read;++i) {
            const auto& action=rate_actions[i];
            if(action.transition==rt::RateTransitionId::none) continue;
            require(shedding && action.transition_domain_registration_index==5 &&
                action.transition==(transition_count%2==0?rt::RateTransitionId::shed:rt::RateTransitionId::recover));
            ++transition_count;
        }
        require(transition_count==(shedding?2*c.count:0));
        rt::RateCounterSnapshot counters_after;okay(owner.rt.rate_counters_snapshot(counters_after));
        for(auto id:{rt::RateCounterId::shed_transitions,rt::RateCounterId::recovery_transitions})
            require(counters_after.values[static_cast<std::size_t>(id)]-counters_before.values[static_cast<std::size_t>(id)]==(shedding?c.count:0));
        m.transitions=transition_count;m.rate_actions=rate_read.records_read;
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
        for(auto bytes:{std::span<const std::byte>(corrupt),std::span<const std::byte>(artifact).first(artifact.size()-1)}) {
            const auto bad=composition?owner.rt.replay_live_control(bytes,apply,this):owner.rt.replay_active(bytes,apply,this);
            require(bad!=rt::Status::ok && load64(state)==expected && applied==0);++m.rejected;
        }
        store64(foreign_state,991);const auto previous_optional=optional_calls;
        const auto bad_identity=composition?foreign.rt.replay_live_control(artifact,apply,this):foreign.rt.replay_active(artifact,apply,this);
        require(bad_identity==rt::Status::incompatible_artifact && load64(foreign_state)==991 &&
                load64(state)==expected && applied==0 && optional_calls==previous_optional);++m.rejected;
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
        require(produced_count==4*c.count && providers==2*c.count && copied_count==4*c.count);
        require(optional_calls-optional_before==(fault?4*c.count:0) && trigger_calls-trigger_before==(shedding?2*c.count:0));
        if(fault) for(std::size_t i=0;i<2;++i) require(optional_by_rank[i]-ranks_before[i]==2*c.count);
        m.callbacks=produced_count+providers+copied_count+(optional_calls-optional_before)+(trigger_calls-trigger_before);
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
        m.operations+=c.count;m.records=copied_count;m.bytes=m.records*c.bytes+artifact.size();m.correct &= valid;
        return m;
    }
    rt::Status finish() noexcept override{
        const auto first=owner.stop(),second=foreign.stop();return first==rt::Status::ok?second:first;
    }
};
// The public loopback executes every command. This fixture-owned extension
// adapter only batches observation of four ready completions, so configured
// occupancy is exercised independently of host scheduling or elapsed time.
struct CompletionGate {
    rt::HalV2CommandTimelineExtension original{},table{};
    std::atomic<std::uint64_t> submitted{},completed{},pending{},peak{};
    std::atomic<bool> draining{};
    void configure(const rt::HalV2CommandTimelineExtension& source) {
        original=source;table=source;
        table.instance=this;
        table.get_capabilities=[](void* p,rt::HalV2CommandTimelineCapabilities* out) {
            auto& s=*static_cast<CompletionGate*>(p);return s.original.get_capabilities(s.original.instance,out);
        };
        table.submit=[](void* p,const rt::DeviceCommandBatch* batch) {
            auto& s=*static_cast<CompletionGate*>(p);
            const auto status=s.original.submit(s.original.instance,batch);
            if(status==rt::HalV2Status::ok){++s.submitted;++s.pending;s.peak=std::max(s.peak.load(),s.pending.load());}
            return status;
        };
        table.poll=[](void* p,rt::HalV2BatchCompletion* out,std::uint64_t capacity,std::uint64_t* count) {
            auto& s=*static_cast<CompletionGate*>(p);
            if(!s.draining && s.pending<4){*count=0;return rt::HalV2Status::ok;}
            const auto status=s.original.poll(s.original.instance,out,capacity,count);
            if(status==rt::HalV2Status::ok){s.completed+=*count;s.pending-=*count;}
            return status;
        };
        table.cancel=[](void* p,std::uint64_t batch) {
            auto& s=*static_cast<CompletionGate*>(p);s.draining=true;
            return s.original.cancel(s.original.instance,batch);
        };
        table.request_stop=[](void* p) {
            auto& s=*static_cast<CompletionGate*>(p);s.draining=true;
            return s.original.request_stop(s.original.instance);
        };
    }
};
struct InFlight final:Fixture {
    static constexpr std::size_t frame_size=sizeof(rt::SampledIoFrameHeader)+8;
    struct Phase {InFlight* owner{};std::size_t index{};};
    std::array<std::byte,8*frame_size> storage{};
    std::array<rt::DeviceCommandBatch,4> declarations{};
    std::array<Phase,4> phases{};
    std::uint64_t calls{};
    rt::SampledIoLoopbackBackend backend{{4,1,65536,1,7}};
    CompletionGate gate;
    RuntimeOwner owner;
    static rt::CallbackResult dispatch(void* p,const rt::DeviceCallbackContext& ctx,rt::DeviceCommandBatch& batch) {
        auto& phase=*static_cast<Phase*>(p);auto& s=*phase.owner;
        if(!ctx.rate_release) return rt::CallbackResult::error;
        batch=s.declarations[phase.index];batch.timeout_ns=period/2;
        batch.signals[0].value=ctx.rate_release->domain_release_sequence+2;
        ++s.calls;return rt::CallbackResult::ok;
    }
    InFlight() {
        okay(backend.add_route({17,101,202,7,303,404}));
        auto cfg=config();cfg.device_backend_capacity=1;cfg.device_buffer_capacity=1;
        cfg.device_outstanding_capacity=4;cfg.device_completion_batch=4;
        okay(owner.rt.configure(cfg));okay(owner.rt.set_rate_execution_policy({4,23,1,1,0}));
        auto registration=backend.hal_v2_registration();gate.configure(*registration.command_timeline);
        registration.command_timeline=&gate.table;
        rt::DeviceBackendHandle backend_handle;okay(owner.rt.register_device_backend(registration,backend_handle));
        rt::DeviceMemoryDomainHandle memory_domain;rt::HalV2MemoryDomain memory;
        require(owner.rt.device_memory_domain_at(backend_handle,0,memory_domain,memory));
        rt::DeviceBufferHandle buffer;
        okay(owner.rt.register_device_buffer({"flight-slots",backend_handle,memory_domain,storage,{},storage.size(),
            rt::HalV2MemoryOwnership::borrowed_host,RTFW_DEVICE_BUFFER_HOST_READ|RTFW_DEVICE_BUFFER_HOST_WRITE|
            RTFW_DEVICE_BUFFER_DEVICE_READ|RTFW_DEVICE_BUFFER_DEVICE_WRITE,rt::HalV2MemoryCoherency::host_coherent,
            rt::hal_v2_memory_sync_none},buffer));
        rt::RateDomainHandle domain;
        okay(owner.rt.register_rate_domain({"flight-rate",period,1,period,1},domain));
        for(std::size_t i=0;i<4;++i) {
            auto bytes=std::span<std::byte>(storage).subspan(i*frame_size,frame_size);
            put(bytes.subspan(sizeof(rt::SampledIoFrameHeader)),i+1);
            rt::SampledIoFrameHeader header;header.channel_identity=101;header.sequence=i+1;
            header.release_generation=i+1;header.sample_count=4;
            header.encoding=static_cast<std::uint32_t>(rt::SampledIoEncoding::signed_int16_le);
            header.timestamp_domain_identity=1;header.first_sample_timestamp=i+1;header.sample_interval_ns=1;
            header.trigger_identity=404;header.trigger_sequence=i+1;header.calibration_identity=303;
            header.status=static_cast<std::uint32_t>(rt::SampledIoFrameStatus::produced);
            header.payload_checksum=rt::sampled_io_payload_checksum(bytes.subspan(sizeof(header)));
            std::memcpy(bytes.data(),&header,sizeof(header));
            auto& declaration=declarations[i];declaration.command_count=1;declaration.signal_count=1;
            auto& command=declaration.commands[0];command.kind=static_cast<std::uint32_t>(rt::HalV2CommandKind::dispatch);
            command.opcode=17;command.buffer_count=2;
            command.buffers[0]={buffer.value,RTFW_DEVICE_ACCESS_READ,0,i*frame_size,frame_size};
            command.buffers[1]={buffer.value,RTFW_DEVICE_ACCESS_WRITE,0,(i+4)*frame_size,frame_size};
            const auto name="flight-"+std::to_string(i);
            rt::DeviceTimelineHandle timeline;okay(owner.rt.register_device_timeline({name,backend_handle,1},timeline));
            declaration.signals[0].timeline_handle=timeline.value;
            phases[i]={this,i};rt::PhaseHandle phase;
            okay(owner.rt.register_device_batch_phase({name,backend_handle,dispatch,&phases[i],declaration},phase));
            const std::array roles{rt::DeviceRatePayloadRole::input,rt::DeviceRatePayloadRole::output};
            okay(owner.rt.bind_device_phase_to_rate_domain({phase,domain,period/2,1,roles}));
        }
        finalized(owner);
    }
    Measures run(std::uint64_t ordinal) override {
        const auto before=backend.stats();const auto previous_calls=calls;
        const auto submitted=gate.submitted.load(),completed=gate.completed.load();
        require(gate.pending==0);gate.peak=0;
        const auto release=1000+ordinal*period;owner.clock.now=release;
        if(owner.rt.step(frame(ordinal,period,release))!=rt::Status::ok) throw std::runtime_error(std::string(owner.rt.last_error()));
        const auto after=backend.stats();
        require(gate.peak==4 && gate.pending==0 && gate.submitted-submitted==4 && gate.completed-completed==4);
        require(calls-previous_calls==4 && after.submissions-before.submissions==4 &&
                after.completions-before.completions==4 && after.frames_copied-before.frames_copied==4 &&
                after.rejected==before.rejected && after.cancellations==before.cancellations);
        rt::RuntimeMetricSnapshot snapshot;okay(owner.rt.metrics_snapshot(rt::RuntimeMetricWindow::cumulative,nullptr,snapshot));
        require(snapshot.samples[static_cast<std::size_t>(rt::RuntimeMetricId::device_outstanding)].value==0);
        Measures m;m.operations=1;m.callbacks=calls-previous_calls;m.records=gate.completed-completed;
        m.bytes=32;m.peak_outstanding=gate.peak;
        for(std::size_t i=0;i<4;++i) {
            const auto bytes=std::span<const std::byte>(storage).subspan((i+4)*frame_size,frame_size);
            rt::SampledIoFrameHeader header;std::memcpy(&header,bytes.data(),sizeof(header));
            require(header.channel_identity==202 && header.timestamp_domain_identity==7 &&
                header.payload_checksum==rt::sampled_io_payload_checksum(bytes.subspan(sizeof(header))) &&
                matches(bytes.subspan(sizeof(header)),i+1));
            m.checksum+=digest(bytes.subspan(sizeof(header)));
        }
        return m;
    }
    rt::Status finish() noexcept override{return owner.stop();}
};
struct DeviceFailure final:Fixture {
    Case c;
    explicit DeviceFailure(const Case& value):c(value){}
    Measures run(std::uint64_t) override {
        // The fixed telemetry buffers are too large for a bounded caller stack.
        // This lifecycle scope includes allocation and retains the entire owner
        // until the checked stop below has drained accepted work.
        auto owned=std::make_unique<Loopback>(c);auto& fixture=*owned;Measures m;
        const bool timeout=std::string_view(c.mode)=="timeout";
        const auto expected=timeout?rt::Status::device_timeout:rt::Status::device_error;
        const auto fault=timeout?rt::SampledIoLoopbackFault::completion_timeout:rt::SampledIoLoopbackFault::completion_error;
        rt::SampledIoChannelStatus before,after;
        require(fixture.owner.rt.sampled_io_channel_status(fixture.output_channel,before));
        okay(fixture.backend.inject_next(fault));
        rt::StepResult step;
        require(fixture.owner.rt.step(frame(1,2*period,1000),&step)==expected);
        require(fixture.owner.rt.sampled_io_channel_status(fixture.output_channel,after));
        require(after.accepted_frames==before.accepted_frames && after.last_sequence==before.last_sequence);
        require(fixture.copied_count==0 && load64(fixture.state)==0 && fixture.providers==1 && fixture.produced_count==1);
        rt::MixedRateActionCursor cursor;rt::MixedRateActionReadResult read;
        okay(fixture.owner.rt.read_mixed_rate_actions(cursor,fixture.actions,read));
        require(read.lost_records==0);
        std::size_t terminals=0;
        for(std::size_t i=0;i<read.records_read;++i) {
            const auto& action=fixture.actions[i];
            if(action.action==rt::MixedRateActionId::device_terminal) {
                require(action.terminal_status==static_cast<std::int32_t>(expected));++terminals;
            }
            require(action.action!=rt::MixedRateActionId::sampled_publish || action.phase_index!=1 ||
                    action.terminal_status!=static_cast<std::int32_t>(rt::Status::ok));
        }
        require(terminals==1 && fixture.backend.stats().submissions==1);
        // Timeout ownership may be quarantined until this checked stop. Keep
        // Runtime, backend, clock and borrowed buffers alive through closure.
        okay(fixture.finish());
        rt::RuntimeMetricSnapshot snapshot;
        okay(fixture.owner.rt.metrics_snapshot(rt::RuntimeMetricWindow::cumulative,nullptr,snapshot));
        require(snapshot.samples[static_cast<std::size_t>(rt::RuntimeMetricId::device_outstanding)].value==0);
        m.operations=1;m.callbacks=step.callbacks_executed;m.rejected=1;
        m.records=terminals;m.mixed_actions=read.records_read;m.checksum=load64(fixture.state);
        return m;
    }
    rt::Status finish() noexcept override{return rt::Status::ok;}
};
}
std::unique_ptr<Fixture> make_device(const Case& c){
    if(std::string_view(c.mode)=="inflight") return std::make_unique<InFlight>();
    if(std::string_view(c.mode)=="failure" || std::string_view(c.mode)=="timeout") return std::make_unique<DeviceFailure>(c);
    return std::make_unique<Loopback>(c);
}
std::unique_ptr<Fixture> make_composition(const Case& c){return std::make_unique<Loopback>(c);}
} // namespace rtfw::benchmark::runtime::detail
