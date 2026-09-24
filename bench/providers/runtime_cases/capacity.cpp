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
            // Public policy setters reject oversized bounds before finalization
            // allocates storage. These are format/configuration checks only.
            for(unsigned field=0;field<5;++field) {
                RuntimeOwner bounded;okay(bounded.rt.configure(config()));
                rt::MixedRateClosurePolicy policy;
                policy.host_policy_version=23;policy.action_capacity=rt::mixed_rate_action_capacity_limit;
                policy.active_replay_record_capacity=rt::active_replay_record_capacity_limit;
                policy.active_replay_max_bytes=rt::active_replay_absolute_max_bytes;
                policy.maximum_actions_per_step=rt::mixed_rate_action_capacity_limit;
                if(field==1) ++policy.action_capacity;
                if(field==2) ++policy.active_replay_record_capacity;
                if(field==3) ++policy.active_replay_max_bytes;
                if(field==4) ++policy.maximum_actions_per_step;
                require(bounded.rt.set_mixed_rate_closure_policy(policy)==(field?rt::Status::invalid_argument:rt::Status::ok));
                ++m.operations;m.rejected+=field!=0;
            }
            for(unsigned field=0;field<7;++field) {
                RuntimeOwner bounded;okay(bounded.rt.configure(config()));
                rt::LiveControlClosurePolicy policy;policy.policy_identity=23;
                policy.action_capacity=rt::live_control_action_capacity_limit;
                policy.retained_generation_capacity=rt::live_control_retained_generation_capacity_limit;
                policy.retained_record_capacity=rt::live_control_record_capacity_limit;
                policy.retained_payload_bytes=rt::live_control_total_storage_limit;
                policy.replay_record_capacity=rt::live_control_record_capacity_limit;
                policy.replay_max_bytes=rt::live_control_replay_absolute_max_bytes;
                if(field==1) ++policy.action_capacity;
                if(field==2) ++policy.retained_generation_capacity;
                if(field==3) ++policy.retained_record_capacity;
                if(field==4) ++policy.retained_payload_bytes;
                if(field==5) ++policy.replay_record_capacity;
                if(field==6) ++policy.replay_max_bytes;
                require(bounded.rt.set_live_control_closure_policy(policy)==(field?rt::Status::invalid_argument:rt::Status::ok));
                ++m.operations;m.rejected+=field!=0;
            }
            for(unsigned overflow=0;overflow<2;++overflow) {
                RuntimeOwner bounded;okay(bounded.rt.configure(config()));
                require(bounded.rt.set_rate_execution_policy({64,23,1,1,rt::rate_telemetry_capacity_limit+overflow})==
                    (overflow?rt::Status::invalid_argument:rt::Status::ok));
                ++m.operations;m.rejected+=overflow;
            }
            m.checksum=m.operations*31+m.rejected;return m;
        }
        if(mode=="selection-bound") {
            std::uint64_t calls=0;std::array<std::byte,8> initial{};
            RuntimeOwner bounded;okay(bounded.rt.configure(config(257)));
            rt::PhaseHandle consumer;rt::RateDomainHandle fast,slow;
            okay(bounded.rt.register_callback({"consumer",count_callback,&calls},consumer));
            okay(bounded.rt.register_rate_domain({"fast",1,1,1,1},fast));
            okay(bounded.rt.register_rate_domain({"slow",c.count,1,c.count,1},slow));
            okay(bounded.rt.bind_phase_to_rate_domain(consumer,fast));
            for(std::size_t i=0;i<256;++i) {
                const auto name="source-"+std::to_string(i);rt::PhaseHandle producer;rt::CrossRateChannelHandle channel;
                okay(bounded.rt.register_callback({name,count_callback,&calls},producer));
                okay(bounded.rt.bind_phase_to_rate_domain(producer,slow));
                okay(bounded.rt.register_cross_rate_channel({name,producer,consumer,8,initial,
                    rt::CrossRateMode::sample_and_hold,c.count},channel));
            }
            const auto status=bounded.rt.finalize();
            require(status==(c.variant?rt::Status::capacity_exceeded:rt::Status::ok));
            if(!c.variant) {
                m.records=bounded.rt.cross_rate_selection_count();require(m.records==262144);
                // Compilation/inspection only: no callbacks or releases execute.
                require(calls==0);
            } else ++m.rejected;
            m.operations=1;m.checksum=m.records;m.correct=calls==0;
            okay(bounded.stop());return m;
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
            if(mode=="domains") {
                rt::RateDomainHandle extra;
                require(owner.rt.register_rate_domain({"reject-next-domain",100,1,100,1},extra)==rt::Status::capacity_exceeded);
                ++m.rejected;
            }
            if(mode=="substeps") {
                rt::RateDomainHandle extra;
                require(owner.rt.register_rate_domain({"reject-next-substep",100,65,100,1},extra)==rt::Status::invalid_argument);
                ++m.rejected;
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
