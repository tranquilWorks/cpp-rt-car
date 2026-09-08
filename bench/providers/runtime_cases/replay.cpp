#include "control_support.hpp"

namespace rtfw::benchmark::runtime::detail {
namespace {
struct Replay final:Fixture {
    Case c;
    std::array<std::byte,16> state{};
    std::vector<std::array<std::byte,8>> values;
    std::vector<rt::ReplayInputRecord> inputs;
    std::vector<std::byte> initial,log,artifact,corrupt;
    std::uint64_t calls{},applied{},expected{},control_reads{};
    bool valid{true};
    RuntimeOwner owner;
    static rt::CallbackResult apply(void* opaque,const rt::ReplayInputView& input) {
        auto& s=*static_cast<Replay*>(opaque);
        const auto i=s.applied++;
        s.valid &= input.input_type==23 && input.payload.size()==8 && input.frame.frame_index==i+1 && load64(input.payload)==i+3;
        store64(std::span<std::byte>(s.state).subspan(8),load64(input.payload));
        return s.valid?rt::CallbackResult::ok:rt::CallbackResult::error;
    }
    static rt::CallbackResult work(void* opaque,const rt::CallbackContext& ctx) {
        auto& s=*static_cast<Replay*>(opaque);++s.calls;
        const auto value=load64(std::span<const std::byte>(s.state).subspan(8));
        if(std::string_view(s.c.mode)=="live") {
            if(!ctx.live_control || ctx.live_control->records.size()!=1) return rt::CallbackResult::error;
            const auto& view=ctx.live_control->records[0];
            s.valid &= view.payload.size()==8 && load64(view.payload)==ctx.frame.frame_index+2;
            ++s.control_reads;
        }
        store64(s.state,load64(s.state)*17+value+ctx.frame.frame_index);
        return s.valid?rt::CallbackResult::ok:rt::CallbackResult::error;
    }
    explicit Replay(const Case& value):c(value),values(c.count),inputs(c.count) {
        auto cfg=config();cfg.determinism_tier=rt::DeterminismTier::schedule_independent;
        cfg.replay_input_capacity=c.capacity;
        okay(owner.rt.configure(cfg));okay(owner.rt.register_state({"replay-state",1,state}));
        okay(owner.rt.register_callback({"replay-application",work,this}));
        const bool live=std::string_view(c.mode)=="live";
        if(live) configure_controls(owner.rt,1,1,8,8,true,true);
        finalized(owner);initial=checkpoint(owner.rt);
        rt::LiveControlProducerHandle handle;
        if(live) okay(owner.rt.live_control_producer_handle(101,1001,handle));
        for(std::size_t i=0;i<c.count;++i) {
            store64(values[i],i+3);inputs[i]={frame(i+1,100,1000+i*100),23,values[i]};
            expected=expected*17+(i+3)+(i+1);
            if(live) {
                auto record=update_record(handle,i+1,values[i],i+1);rt::LiveControlAdmissionResult result;
                okay(owner.rt.stage_live_control_update(handle,record,values[i],result));
                require(result==rt::LiveControlAdmissionResult::accepted);
                store64(std::span<std::byte>(state).subspan(8),i+3);
                owner.clock.now=1000+i*100;okay(owner.rt.step(inputs[i].frame));
            }
        }
        rt::ArtifactWriteResult write;
        require(owner.rt.write_input_log(inputs,{},write)==rt::Status::capacity_exceeded);
        log.resize(write.required_bytes);okay(owner.rt.write_input_log(inputs,log,write));
        if(live) {
            require(owner.rt.write_live_control_replay_artifact(initial,log,rt::LiveControlNestedArtifactKind::input_log,{},write)==rt::Status::capacity_exceeded);
            require(write.required_bytes<=16U*1024U*1024U);artifact.resize(write.required_bytes);
            okay(owner.rt.write_live_control_replay_artifact(initial,log,rt::LiveControlNestedArtifactKind::input_log,artifact,write));
        } else artifact=log;
        corrupt=artifact;corrupt.back()^=std::byte{1};
    }
    Measures run(std::uint64_t) override {
        Measures m;calls=applied=control_reads=0;valid=true;
        put(state,99);const auto sentinel=digest(state);
        const bool live=std::string_view(c.mode)=="live";
        for(auto bytes:{std::span<const std::byte>(corrupt),std::span<const std::byte>(artifact).first(artifact.size()-1)}) {
            const auto status=live?owner.rt.replay_live_control(bytes,apply,this):owner.rt.replay(initial,bytes,apply,this);
            require(status!=rt::Status::ok && digest(state)==sentinel && applied==0 && calls==0);++m.rejected;
        }
        if(live) {
            rt::LiveControlReplayMetadata metadata;okay(rt::inspect_live_control_replay_artifact(artifact,metadata));
            rt::LiveControlReplayResult result;okay(owner.rt.replay_live_control(artifact,apply,this,&result));
            require(result.frames_replayed==c.count && result.generations_compared==c.count && result.mismatch_status==rt::Status::ok);
            require(metadata.retained_record_count==c.count && control_reads==c.count);
            m.transitions=result.generations_compared;
        } else {
            rt::InputLogMetadata metadata;okay(rt::inspect_input_log_artifact(log,metadata));
            rt::ReplayResult result;okay(owner.rt.replay(initial,log,apply,this,&result));
            require(result.frames_replayed==c.count && result.records_processed==c.count && metadata.record_count==c.count);
        }
        m.operations=applied;m.callbacks=calls;m.records=applied;m.bytes=artifact.size();
        m.correct=valid && calls==c.count && applied==c.count && load64(state)==expected;
        m.checksum=load64(state);return m;
    }
    rt::Status finish() noexcept override{return owner.stop();}
};
}
std::unique_ptr<Fixture> make_replay(const Case& c) {
    return std::string_view(c.mode)=="active"?make_device(c):std::make_unique<Replay>(c);
}
} // namespace rtfw::benchmark::runtime::detail
