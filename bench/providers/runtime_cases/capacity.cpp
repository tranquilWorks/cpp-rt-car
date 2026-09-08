#include "control_support.hpp"

namespace rtfw::benchmark::runtime::detail {
namespace {
rt::CallbackResult count_callback(void* opaque,const rt::CallbackContext&) {
    ++*static_cast<std::uint64_t*>(opaque);return rt::CallbackResult::ok;
}
struct Capacity final:Fixture {
    Case c;
    explicit Capacity(const Case& value):c(value){}
    Measures run(std::uint64_t) override {
        Measures m;
        const auto mode=std::string_view(c.mode);
        if(mode=="absolute-controls") {
            require(rt::live_control_mailbox_capacity_limit==64 && rt::live_control_producer_capacity_limit==256 &&
                rt::live_control_record_capacity_limit==65536 && rt::live_control_payload_bytes_limit==65536 &&
                rt::live_control_total_storage_limit==(std::uint64_t{1}<<30U) &&
                rt::active_replay_absolute_max_bytes==(std::size_t{1}<<30U) &&
                rt::live_control_replay_absolute_max_bytes==(std::size_t{1}<<30U) &&
                rt::cross_rate_selection_capacity==262144 && rt::mixed_rate_action_capacity_limit==65536 &&
                rt::rate_telemetry_capacity_limit==65536 && rt::live_control_action_capacity_limit==262144);
            for(unsigned field=0;field<6;++field) {
                RuntimeOwner owner;okay(owner.rt.configure(config()));
                rt::LiveControlPolicy policy;policy.policy_identity=23;
                policy.mailbox_capacity=64;policy.producer_capacity=256;policy.record_capacity=65536;
                policy.payload_bytes_per_record=65536;policy.total_payload_storage_bytes=std::uint64_t{1}<<30U;
                if(field==1)++policy.mailbox_capacity;
                if(field==2)++policy.producer_capacity;
                if(field==3)++policy.record_capacity;
                if(field==4)++policy.payload_bytes_per_record;
                if(field==5)++policy.total_payload_storage_bytes;
                require(owner.rt.set_live_control_policy(policy)==(field?rt::Status::invalid_argument:rt::Status::ok));
                ++m.operations;m.rejected+=field!=0;
            }
            for(unsigned field=0;field<3;++field) {
                RuntimeOwner owner;auto cfg=config();cfg.snapshot_max_bytes=std::size_t{1}<<30U;
                cfg.input_log_max_bytes=std::size_t{1}<<30U;
                if(field==1)++cfg.snapshot_max_bytes;
                if(field==2)++cfg.input_log_max_bytes;
                require(owner.rt.configure(cfg)==(field?rt::Status::invalid_config:rt::Status::ok));
                ++m.operations;m.rejected+=field!=0;
            }
            m.checksum=m.operations*31+m.rejected;return m;
        }
        std::uint64_t calls=0;
        RuntimeOwner owner;okay(owner.rt.configure(config(64)));
        if(mode=="post-stop") {
            configure_controls(owner.rt,1,1,8,8,false);finalized(owner);
            rt::LiveControlProducerHandle h;okay(owner.rt.live_control_producer_handle(101,1001,h));
            okay(owner.stop());std::array<std::byte,8> payload{};put(payload,1);
            auto update=update_record(h,1,payload,1);rt::LiveControlAdmissionResult result;
            okay(owner.rt.stage_live_control_update(h,update,payload,result));
            require(result==rt::LiveControlAdmissionResult::stopped);++m.operations;++m.rejected;
        } else {
            const bool overflow=mode=="reference-overflow";
            const bool dispatch=mode=="dispatch-full";
            const auto width=overflow?2U:(dispatch?1U:static_cast<unsigned>(c.width));
            if(dispatch) okay(owner.rt.set_rate_execution_policy({c.capacity,1,1,1,0}));
            for(std::size_t i=0;i<width;++i) {
                rt::PhaseHandle phase;rt::RateDomainHandle domain;
                const auto name="boundary-"+std::to_string(i);
                okay(owner.rt.register_callback({name,count_callback,&calls},phase));
                const auto p=overflow?(i?65536U:1U):100U;
                okay(owner.rt.register_rate_domain({name,p,static_cast<std::uint32_t>(overflow||dispatch?1:c.variant),p,1},domain));
                okay(owner.rt.bind_phase_to_rate_domain(phase,domain));
            }
            if(overflow) {
                require(owner.rt.finalize()==rt::Status::capacity_exceeded);++m.operations;++m.rejected;
            } else {
                finalized(owner);++m.operations;
                m.records=owner.rt.reference_release_count();
                require(m.records==(dispatch?1:c.width*c.variant));
                for(std::size_t i=0;i<m.records;++i) {rt::ReferenceRelease r;require(owner.rt.reference_release_at(i,r));m.checksum+=r.substep_ordinal+1;}
                if(dispatch) {
                    rt::StepResult result;owner.clock.now=1000;
                    okay(owner.rt.step(frame(0,c.capacity*100,1000),&result));
                    require(result.rate.executed_reference_records==c.capacity && calls==c.capacity);
                    const auto next_release=1000+c.capacity*100;owner.clock.now=next_release;
                    require(owner.rt.step(frame(1,(c.capacity+1)*100,next_release),&result)==rt::Status::capacity_exceeded);
                    require(result.rate.executed_reference_records==c.capacity && result.rate.rejected_reference_records==1 && calls==2*c.capacity);
                    m.callbacks=calls;m.rejected=1;m.records=calls;m.operations=2;
                }
            }
        }
        okay(owner.stop());return m;
    }
    rt::Status finish() noexcept override{return rt::Status::ok;}
};
}
std::unique_ptr<Fixture> make_capacity(const Case& c){return std::make_unique<Capacity>(c);}
} // namespace rtfw::benchmark::runtime::detail
