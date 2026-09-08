#pragma once
#include "support.hpp"

namespace rtfw::benchmark::runtime::detail {
inline rt::LiveControlUpdateRecord update_record(
    const rt::LiveControlProducerHandle& h,std::uint64_t sequence,
    std::span<const std::byte> payload,std::uint64_t target) {
    rt::LiveControlUpdateRecord r;
    r.runtime_id=h.runtime_id;r.configuration_generation=h.configuration_generation;
    r.mailbox_identity=h.mailbox_identity;r.producer_identity=h.producer_identity;
    r.producer_sequence=sequence;r.target_frame_index=target;
    r.payload_bytes=static_cast<std::uint32_t>(payload.size());
    r.payload_digest=rt::live_control_payload_digest(payload);return r;
}
inline void configure_controls(rt::Runtime& runtime,std::size_t mailboxes,std::size_t producers,
                               std::size_t records,std::size_t bytes,bool closure,bool replay=false) {
    rt::LiveControlPolicy p;p.policy_identity=230301;
    p.mailbox_capacity=static_cast<std::uint32_t>(mailboxes);
    p.producer_capacity=static_cast<std::uint32_t>(producers);
    p.record_capacity=static_cast<std::uint32_t>(records);
    p.payload_bytes_per_record=static_cast<std::uint32_t>(bytes);
    p.total_payload_storage_bytes=records*bytes;
    okay(runtime.set_live_control_policy(p));
    for(std::size_t i=0;i<mailboxes;++i) {
        rt::LiveControlMailboxRegistration m;m.mailbox_identity=101+i;
        m.record_capacity=static_cast<std::uint32_t>(records/mailboxes);
        m.payload_bytes_per_record=static_cast<std::uint32_t>(bytes);
        okay(runtime.register_live_control_mailbox(m));
    }
    for(std::size_t i=0;i<producers;++i) {
        rt::LiveControlProducerRegistration r;r.mailbox_identity=101+i%mailboxes;
        r.producer_identity=1001+i;okay(runtime.register_live_control_producer(r));
    }
    if(closure) {
        rt::LiveControlClosurePolicy r;r.policy_identity=230303;r.action_capacity=1024;
        if(replay) {
            r.retained_generation_capacity=64;r.retained_record_capacity=256;
            r.retained_payload_bytes=65536;r.replay_record_capacity=1024;
            r.replay_max_bytes=1024*1024;r.replay_enabled=true;
        }
        okay(runtime.set_live_control_closure_policy(r));
    }
}
} // namespace rtfw::benchmark::runtime::detail
