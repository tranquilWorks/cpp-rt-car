#include "lifecycle.hpp"
#include <rt/live_control.hpp>
#include <algorithm>
#include <iostream>
#include <span>

struct Payload { std::uint32_t value = 0; };
template <> struct rt::LiveControlTypeTraits<Payload> {
    static constexpr std::uint32_t application_type_identity = 27105;
    static constexpr std::uint32_t application_schema_version = 1;
    static constexpr auto update_kind = rt::LiveControlUpdateKind::scenario_parameters;
    static constexpr std::size_t encoded_extent = 4;
    static bool validate(const Payload& value) noexcept { return value.value <= 100000; }
    static bool encode(const Payload& value, std::span<std::byte, 4> out) noexcept {
        for (unsigned i=0; i<4; ++i) out[i]=static_cast<std::byte>(value.value >> (8*i));
        return validate(value);
    }
    static bool decode(std::span<const std::byte, 4> in, Payload& out) noexcept {
        out.value=0;
        for (unsigned i=0; i<4; ++i) out.value |= std::to_integer<std::uint32_t>(in[i]) << (8*i);
        return validate(out);
    }
};
void payload_cases() {
    std::array<std::byte, rt::live_control_typed_payload_extent<Payload>> bytes{};
    REQUIRE(rt::encode_live_control_typed_payload(Payload{0x10203}, bytes)==rt::LiveControlTypedStatus::ok);
    const auto offset=rt::live_control_typed_envelope_bytes;
    REQUIRE(bytes[offset]==std::byte{3} && bytes[offset+1]==std::byte{2} &&
            bytes[offset+2]==std::byte{1} && bytes[offset+3]==std::byte{0});
    rt::LiveControlUpdateRecord record;
    record.payload_bytes=static_cast<std::uint32_t>(bytes.size());
    record.payload_digest=rt::live_control_payload_digest(bytes);
    record.policy_flags=rt::live_control_payload_canonical_little_endian;
    record.update_kind=rt::LiveControlUpdateKind::scenario_parameters;
    Payload out{7};
    REQUIRE(rt::decode_live_control_typed_payload(record, bytes, out)==rt::LiveControlTypedStatus::ok);
    REQUIRE(out.value==0x10203);
    for (unsigned which=0; which<4; ++which) {
        auto changed=record;
        auto input=bytes;
        std::span<const std::byte> view=input;
        if (which==0) changed.payload_digest ^= 1;
        if (which==1) view=view.first(view.size()-1);
        if (which==2) { input[0] ^= std::byte{1}; changed.payload_digest=rt::live_control_payload_digest(input); }
        if (which==3) changed.update_kind=rt::LiveControlUpdateKind::clear_fault;
        out.value=0xfeed;
        REQUIRE(rt::decode_live_control_typed_payload(changed, view, out)!=rt::LiveControlTypedStatus::ok);
        REQUIRE(out.value==0xfeed);
    }
}
void active_replay_cases() {
    Clock clock;
    std::array<std::byte,8> state{};
    rt::Runtime runtime{clock};
    struct Stop { rt::Runtime& r; ~Stop(){(void)r.stop();} } stop{runtime};
    rt::RuntimeConfig config;
    config.callback_capacity=2; config.worker_count=1; config.executor_queue_capacity=8;
    config.task_scratch_slots=8; config.trace_capacity=0;
    REQUIRE(runtime.configure(config)==rt::Status::ok);
    REQUIRE(runtime.set_rate_execution_policy({16,1,1,1,64})==rt::Status::ok);
    REQUIRE(runtime.set_mixed_rate_closure_policy({1,256,256,131072,16,
        rt::MixedRateOverflowPolicy::overwrite_committed,true,true,{}})==rt::Status::ok);
    rt::PhaseHandle work,anchor_work;
    REQUIRE(runtime.register_callback({"work",[](void* p,const rt::CallbackContext&){
        auto& value=*static_cast<std::array<std::byte,8>*>(p);
        value[0]=static_cast<std::byte>(std::to_integer<unsigned>(value[0])+1);
        return rt::CallbackResult::ok;
    },&state},work)==rt::Status::ok);
    REQUIRE(runtime.register_callback({"anchor",[](void*,const rt::CallbackContext&){
        return rt::CallbackResult::ok;
    },nullptr},anchor_work)==rt::Status::ok);
    rt::RateDomainHandle rate,anchor;
    REQUIRE(runtime.register_rate_domain({"rate",100,1,100,1},rate)==rt::Status::ok);
    REQUIRE(runtime.register_rate_domain({"anchor",400,1,400,1},anchor)==rt::Status::ok);
    REQUIRE(runtime.bind_phase_to_rate_domain(work,rate)==rt::Status::ok);
    REQUIRE(runtime.bind_phase_to_rate_domain(anchor_work,anchor)==rt::Status::ok);
    REQUIRE(runtime.register_state({"state",1,state})==rt::Status::ok);
    REQUIRE(runtime.finalize()==rt::Status::ok && runtime.start()==rt::Status::ok);
    std::size_t extent=0;
    REQUIRE(runtime.checkpoint_size(extent)==rt::Status::ok);
    std::vector<std::byte> checkpoint(extent);
    rt::ArtifactWriteResult written;
    REQUIRE(runtime.write_checkpoint(0,checkpoint,written)==rt::Status::ok);
    std::array<rt::ReplayInputRecord,2> records{};
    for (std::size_t i=0; i<records.size(); ++i) {
        records[i]={{i+1,std::chrono::nanoseconds{100},std::nullopt,1000+i*100},1,{}};
        REQUIRE(runtime.step(records[i].frame)==rt::Status::ok);
    }
    std::vector<std::byte> artifact(131072);
    REQUIRE(runtime.write_active_replay_artifact(checkpoint,records,artifact,written)==rt::Status::ok);
    artifact.resize(written.bytes_written);
    const auto before=state;
    rt::ActiveReplayMetadata metadata;
    REQUIRE(rt::inspect_active_replay_artifact(artifact,metadata)==rt::Status::ok);
    REQUIRE(metadata.total_bytes==artifact.size() && metadata.input_record_count==2);
    REQUIRE(metadata.first_frame_index==1 && metadata.last_frame_index==2);
    auto broken=artifact; broken[0]^=std::byte{1};
    metadata.runtime_id=0xfeed;
    REQUIRE(rt::inspect_active_replay_artifact(broken,metadata)!=rt::Status::ok && metadata.runtime_id==0);
    artifact.pop_back(); metadata.runtime_id=0xfeed;
    REQUIRE(rt::inspect_active_replay_artifact(artifact,metadata)!=rt::Status::ok && metadata.runtime_id==0);
    REQUIRE(state==before);
    REQUIRE(runtime.stop()==rt::Status::ok);
}
int main() {
    try {
        lifecycle_case({0,1,0,1,2,3,0,1,4,5,6,7,12,13,8,9,0,1,10,11,8,9});
        lifecycle_case({1,1,3,0,0,2,13,12,6,7,4,5,9,8,11,10});
        lifecycle_case({});
        payload_cases();
        active_replay_cases();
        std::cout << "PASS fixed Runtime lifecycle, owner isolation, checkpoint, typed payload and active replay cases\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL " << e.what() << '\n'; return 1;
    }
}
