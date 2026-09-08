#include "control_support.hpp"
#include <limits>

namespace rtfw::benchmark::runtime::detail {
namespace {
struct Controls final:Fixture {
    Case c;
    std::vector<std::byte> payload;
    std::vector<rt::LiveControlProducerHandle> handles;
    std::vector<std::uint64_t> sequences,seeds;
    std::array<rt::LiveControlActionRecord,1024> actions{};
    std::vector<std::byte> initial;
    rt::ReferenceRelease release{};
    std::uint64_t calls{},seen{},sum{};
    bool valid{true},fail_callback{};
    RuntimeOwner owner;
    static rt::CallbackResult callback(void* opaque,const rt::CallbackContext& ctx) {
        auto& s=*static_cast<Controls*>(opaque);++s.calls;
        if(!ctx.live_control) return rt::CallbackResult::error;
        s.valid &= ctx.live_control->records.size()==s.c.width;
        for(const auto& view:ctx.live_control->records) {
            const auto index=view.record.mailbox_identity-101;
            if(index>=s.seeds.size()) return rt::CallbackResult::error;
            s.valid &= view.payload.size()==s.c.bytes && matches(view.payload,s.seeds[index]) &&
                view.record.payload_digest==digest(view.payload);
            s.sum+=digest(view.payload);++s.seen;
        }
        return s.valid && !s.fail_callback?rt::CallbackResult::ok:rt::CallbackResult::error;
    }
    explicit Controls(const Case& value):c(value),payload(c.bytes),
        handles(c.variant==4?256:c.width),sequences(handles.size(),1),seeds(c.width) {
        okay(owner.rt.configure(config()));
        const bool rate=std::string_view(c.mode)=="rate";
        rt::PhaseHandle phase;okay(owner.rt.register_callback({"control-view",callback,this},phase));
        if(rate) {
            okay(owner.rt.set_rate_execution_policy({4,1,1,1,0}));
            rt::RateDomainHandle domain;
            okay(owner.rt.register_rate_domain({"control-rate",100,1,100,1},domain));
            okay(owner.rt.bind_phase_to_rate_domain(phase,domain));
        }
        configure_controls(owner.rt,c.width,handles.size(),c.capacity,c.bytes,c.capacity<1024);
        finalized(owner);
        refresh_handles();
        if(rate) require(owner.rt.reference_release_at(0,release));
        if(rate || std::string_view(c.mode)=="rollback") initial=checkpoint(owner.rt);
    }
    void refresh_handles() {
        for(std::size_t i=0;i<handles.size();++i)
            okay(owner.rt.live_control_producer_handle(101+i%c.width,1001+i,handles[i]));
    }
    rt::LiveControlUpdateRecord record(std::size_t i,std::uint64_t target) {
        auto r=update_record(handles[i],sequences[i],payload,target);
        if(std::string_view(c.mode)=="rate") {
            r.target_kind=rt::LiveControlTargetKind::rate_release;
            r.target_frame_index=std::numeric_limits<std::uint64_t>::max();
            r.reference_release_index=0;
            r.rate_domain_registration_index=static_cast<std::uint32_t>(release.domain_registration_index);
            r.phase_index=release.phase.index();r.rate_substep_ordinal=release.substep_ordinal;
            r.rate_release_sequence=target-1;
        }
        return r;
    }
    Measures run(std::uint64_t ordinal) override {
        Measures m;
        const auto monotonic_release=1000U+ordinal*100U;
        const bool rollback=std::string_view(c.mode)=="rollback";
        if(rollback || std::string_view(c.mode)=="rate") {
            okay(owner.rt.restore_checkpoint(initial));refresh_handles();
            std::fill(sequences.begin(),sequences.end(),1);
            ordinal=0;
        }
        const auto target=ordinal+1,prev_calls=calls,prev_seen=seen;
        rt::LiveControlCommitInfo before;require(owner.rt.live_control_commit_info(before));
        for(std::size_t n=0;n<c.count;++n) {
            const auto i=n%handles.size(),mailbox=i%c.width;
            const auto seed=1+ordinal*c.count+n;put(payload,seed);
            auto r=record(i,target);rt::LiveControlAdmissionResult result;
            okay(owner.rt.stage_live_control_update(handles[i],r,payload,result));
            require(result==rt::LiveControlAdmissionResult::accepted);
            ++sequences[i];seeds[mailbox]=seed;++m.operations;m.bytes+=payload.size();
        }
        // Full/invalid/stale rejections leave the accepted copied records intact.
        put(payload,199);
        auto r=record(0,target);rt::LiveControlAdmissionResult result;
        if(c.count==c.capacity) {
            okay(owner.rt.stage_live_control_update(handles[0],r,payload,result));
            require(result==rt::LiveControlAdmissionResult::full);++m.rejected;
        }
        auto invalid=r;invalid.payload_digest^=1;
        okay(owner.rt.stage_live_control_update(handles[0],invalid,payload,result));
        require(result==rt::LiveControlAdmissionResult::invalid);++m.rejected;
        invalid=r;invalid.policy_flags=0;
        okay(owner.rt.stage_live_control_update(handles[0],invalid,payload,result));
        require(result==rt::LiveControlAdmissionResult::invalid);++m.rejected;
        auto stale=handles[0];++stale.configuration_generation;
        okay(owner.rt.stage_live_control_update(stale,r,payload,result));
        require(result==rt::LiveControlAdmissionResult::stale);++m.rejected;
        std::fill(payload.begin(),payload.end(),std::byte{0});
        fail_callback=rollback;
        owner.clock.now=monotonic_release;
        rt::StepResult step;
        const auto status=owner.rt.step(frame(target,100,monotonic_release),&step);
        require(status==(rollback?rt::Status::callback_failed:rt::Status::ok));
        m.callbacks=calls-prev_calls;m.records=seen-prev_seen;
        rt::LiveControlCommitInfo after;require(owner.rt.live_control_commit_info(after));
        m.correct=valid && m.callbacks==1 && m.records==c.width && after.staged_occupancy==0;
        if(rollback) {
            m.correct &= after.generation_identity==before.generation_identity;
            rt::LiveControlRecordStatusInfo info;require(owner.rt.live_control_record_status(101,1,info));
            m.correct &= info.status==rt::LiveControlRecordStatus::rolled_back;
        } else {
            m.transitions=after.replaced-before.replaced;
            m.correct &= after.committed-before.committed==c.width && m.transitions==c.count-c.width;
        }
        for(std::size_t i=0;i<c.width;++i) {
            rt::LiveControlMailboxInfo info;require(owner.rt.live_control_mailbox_info(101+i,info));
            m.correct &= info.occupancy==0 && info.record_capacity==c.capacity/c.width;
        }
        if(c.capacity<1024) {
            rt::LiveControlActionCursor cursor;rt::LiveControlActionReadResult read;
            okay(owner.rt.read_live_control_actions(cursor,actions,read));m.gaps=read.lost_records;
            m.control_actions=read.records_read;
            for(std::size_t i=0;i<read.records_read;++i) m.correct &= actions[i].schema_version==1;
        }
        m.checksum=sum;return m;
    }
    rt::Status finish() noexcept override {return owner.stop();}
};
}
std::unique_ptr<Fixture> make_controls(const Case& c){return std::make_unique<Controls>(c);}
} // namespace rtfw::benchmark::runtime::detail
