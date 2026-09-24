// Public-only reproducer for saturated watchdog action/replay divergence.
// No benchmark provider, live controls, device backend or private API is used.
#include <rt/runtime.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <vector>

namespace {
constexpr std::uint64_t budget=60'000'000'000;
constexpr std::uint64_t period=4*budget;
struct Clock final:rt::RuntimeClock {
    std::atomic<std::uint64_t> value{1000};
    std::uint64_t now_ns() noexcept override{return value.load();}
};
struct Work {
    Clock clock;
    std::array<std::byte,8> state{};
    std::uint64_t callbacks{},inputs{};
    static rt::CallbackResult step(void* opaque,const rt::CallbackContext&) {
        auto& w=*static_cast<Work*>(opaque);
        w.state[0]=std::byte(std::to_integer<unsigned>(w.state[0])+1);
        ++w.callbacks;w.clock.value.fetch_add(budget+1);
        return rt::CallbackResult::ok;
    }
    static rt::CallbackResult apply(void* opaque,const rt::ReplayInputView& input) {
        auto& w=*static_cast<Work*>(opaque);++w.inputs;
        w.clock.value=*input.frame.nominal_release_ns;
        return rt::CallbackResult::ok;
    }
};
bool run(std::uint32_t cap) {
    Work work;rt::Runtime runtime(work.clock);
    const auto check=[](rt::Status status){return status==rt::Status::ok;};
    rt::RuntimeConfig config;
    config.worker_count=1;config.watchdog_timeout_ns=budget;
    config.watchdog_max_degradation_level=cap;
    if(!check(runtime.configure(config)) ||
       !check(runtime.set_rate_execution_policy({8,23,1,1,64})) ||
       !check(runtime.set_mixed_rate_closure_policy({23,64,64,1024*1024,8,
          rt::MixedRateOverflowPolicy::overwrite_committed,true,true,{}})) ||
       !check(runtime.register_state({"counter",1,work.state}))) return false;
    rt::PhaseHandle phase;rt::RateDomainHandle domain;
    if(!check(runtime.register_callback({"work",Work::step,&work},phase)) ||
       !check(runtime.register_rate_domain({"rate",period,1,period,1},domain)) ||
       !check(runtime.bind_phase_to_rate_domain(phase,domain)) ||
       !check(runtime.finalize()) || !check(runtime.start())) return false;
    std::size_t size=0;rt::ArtifactWriteResult write;
    bool valid=check(runtime.checkpoint_size(size));
    std::vector<std::byte> checkpoint(size);
    valid &= check(runtime.write_checkpoint(0,checkpoint,write));
    std::array<rt::ReplayInputRecord,8> inputs{};
    for(std::size_t i=0;i<inputs.size() && valid;++i) {
        const auto release=1000+i*period;work.clock.value=release;
        inputs[i]={{i+1,std::chrono::nanoseconds{period},std::nullopt,release},23,{}};
        rt::StepResult result;valid &= check(runtime.step(inputs[i].frame,&result));
        valid &= result.watchdog_fired && result.degradation_level==std::min<std::uint32_t>(static_cast<std::uint32_t>(i+1),cap);
    }
    std::array<rt::MixedRateActionRecord,64> actions{};
    rt::MixedRateActionCursor cursor;rt::MixedRateActionReadResult read;
    valid &= check(runtime.read_mixed_rate_actions(cursor,actions,read));
    bool transitions=true;std::size_t events=0;
    for(std::size_t i=0;i<read.records_read;++i) {
        const auto& a=actions[i];if(a.action!=rt::MixedRateActionId::watchdog_transition) continue;
        const auto before=std::min<std::uint32_t>(static_cast<std::uint32_t>(events),cap);
        const auto after=std::min<std::uint32_t>(static_cast<std::uint32_t>(events+1),cap);
        if(a.degradation_before!=before || a.degradation_after!=after) {
            std::printf("cap=%u frame=%llu observed=%u->%u expected=%u->%u\n",cap,
                static_cast<unsigned long long>(a.frame_index),a.degradation_before,a.degradation_after,before,after);
            transitions=false;
        }
        ++events;
    }
    valid &= events==inputs.size();
    valid &= runtime.write_active_replay_artifact(checkpoint,inputs,{},write)==rt::Status::capacity_exceeded;
    std::vector<std::byte> artifact(write.required_bytes);
    valid &= check(runtime.write_active_replay_artifact(checkpoint,inputs,artifact,write));
    work.callbacks=work.inputs=0;
    rt::ActiveReplayResult replay;
    const auto status=runtime.replay_active(artifact,Work::apply,&work,&replay);
    std::printf("cap=%u replay_status=%d frames=%zu inputs=%llu callbacks=%llu state=%u mismatch=%llu\n",
        cap,static_cast<int>(status),static_cast<std::size_t>(replay.replay.frames_replayed),
        static_cast<unsigned long long>(work.inputs),static_cast<unsigned long long>(work.callbacks),
        std::to_integer<unsigned>(work.state[0]),static_cast<unsigned long long>(replay.mismatch_sequence));
    valid &= check(status) && replay.replay.frames_replayed==8 && work.callbacks==8 && work.inputs==8 &&
        work.state[0]==std::byte{8} && transitions;
    valid &= check(runtime.stop());return valid;
}
}
int main(){const bool cap1=run(1),cap3=run(3);return cap1 && cap3?0:1;}
