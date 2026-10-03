#include <rt/runtime.hpp>
#include <rt/loopback_backend.hpp>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>

namespace closeout {
enum class Schedule { ordinary, before_acceptance, stop_before_acceptance, after_acceptance };
enum class Corruption { none, submissions, frames, actions, completions, cancellations, rejection, terminal, output, attempts, returned_status };
inline Schedule schedule{};
inline Corruption corruption{};
inline rt::HalV2CommandTimelineExtension original{}, table{};
inline std::atomic<bool> entered{}, released{}, completed{};
inline std::atomic<unsigned> entries{}, stops{};
inline bool is_stopped{}, saw_terminal{};
inline std::uint64_t at_terminal{}, at_stop{};
inline rt::HalV2Status submit_status{};

void check(bool value) {
    if(!value) throw std::runtime_error("closeout observation failed");
}
void wait(const std::atomic<bool>& flag) {
    const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while(!flag.load()) {
        check(std::chrono::steady_clock::now()<end);
        std::this_thread::yield();
    }
}
rt::HalV2BackendRegistration wrap(rt::HalV2BackendRegistration registration) {
    original=*registration.command_timeline;table=original;
    table.submit=[](void* instance,const rt::DeviceCommandBatch* batch) {
        ++entries;
        if(schedule==Schedule::before_acceptance || schedule==Schedule::stop_before_acceptance) {
            entered=true;
            while(!released.load()) std::this_thread::yield();
        }
        const auto status=original.submit(instance,batch);
        submit_status=status;
        if(schedule==Schedule::after_acceptance) {
            entered=true;
            while(!released.load()) std::this_thread::yield();
        }
        completed=true;
        return status;
    };
    table.request_stop=[](void* instance) {
        ++stops;
        const auto status=original.request_stop(instance);
        released=true;
        return status;
    };
    registration.command_timeline=&table;
    return registration;
}
template<class Fixture> void terminal(Fixture& fixture) {
    if(schedule!=Schedule::ordinary) wait(entered);
    saw_terminal=true;at_terminal=fixture.backend.stats().submissions;
    check(fixture.providers==1 && fixture.produced_count==1 && fixture.copied_count==0);
    if(schedule==Schedule::before_acceptance || schedule==Schedule::stop_before_acceptance)
        check(at_terminal==0 && entries==1 && !completed);
    if(schedule==Schedule::after_acceptance) check(at_terminal==1 && entries==1 && !completed);
    if(corruption==Corruption::output) fixture.copied_count=1;
}
void before_stop() {
    if(schedule==Schedule::before_acceptance || schedule==Schedule::after_acceptance) {
        released=true;wait(completed);check(submit_status==rt::HalV2Status::ok);
    }
}
template<class Fixture> void stopped(Fixture& fixture) {
    check(fixture.owner.rt.state()==rt::RuntimeState::stopped && stops>=1);
    is_stopped=true;at_stop=fixture.backend.stats().submissions;
    if(schedule==Schedule::stop_before_acceptance)
        check(completed && submit_status==rt::HalV2Status::invalid_state && at_stop==0);
    if(schedule==Schedule::before_acceptance || schedule==Schedule::after_acceptance)
        check(at_stop==1);
    if constexpr(requires { fixture.failure_submission; }) {
        if(corruption==Corruption::attempts) fixture.failure_submission.attempts=0;
        if(corruption==Corruption::returned_status)
            fixture.failure_submission.result=at_stop==0?rt::HalV2Status::ok:rt::HalV2Status::invalid_state;
    }
}
void terminals(std::size_t& count) {
    if(corruption==Corruption::terminal) count=0;
}
rt::SampledIoLoopbackStats stats(const rt::SampledIoLoopbackBackend& backend) {
    auto value=backend.stats();
    if(!is_stopped) return value;
    switch(corruption) {
    case Corruption::submissions: value.submissions=2;break;
    case Corruption::frames: ++value.frames_copied;break;
    case Corruption::actions: ++value.logical_actions;break;
    case Corruption::completions: value.completions=2;break;
    case Corruption::cancellations: value.cancellations=2;break;
    case Corruption::rejection: ++value.rejected;break;
    default:break;
    }
    return value;
}
void reset(Schedule next,Corruption error=Corruption::none) {
    schedule=next;corruption=error;entered=false;released=false;completed=false;
    entries=0;stops=0;is_stopped=false;saw_terminal=false;at_terminal=0;at_stop=0;
}
}

#include "device-observed.hpp"

namespace b=rtfw::benchmark::runtime;
const b::Case& selected(std::string_view id) {
    using b::Family;
    static constexpr b::Case catalog[]{
#include "catalog.inc"
    };
    for(const auto& value:catalog) if(value.id==id) return value;
    throw std::runtime_error("missing catalog case");
}
void failure_case(const char* label,closeout::Schedule schedule,
                  closeout::Corruption corruption=closeout::Corruption::none,
                  bool timeout=true) {
    closeout::reset(schedule,corruption);
    bool rejected=false;
    try {
        b::detail::DeviceFailure fixture(selected(timeout?"device-timeout-nonpublication":"device-failure-nonpublication"));
        const auto result=fixture.run(0);
        closeout::check(result.correct && result.operations==1 && result.rejected==1 && result.records==1 && result.checksum==0);
        closeout::check(fixture.finish()==rt::Status::ok);
    } catch(const std::runtime_error& error) {
        if(std::string_view(error.what()).find("runtime fixture invariant")==std::string_view::npos) throw;
        std::printf("Observed invariant rejection: %s\n",error.what());
        rejected=true;
    }
    closeout::check(rejected==(corruption!=closeout::Corruption::none));
    closeout::check(closeout::saw_terminal && closeout::stops>=1);
    if(corruption==closeout::Corruption::none) closeout::check(closeout::is_stopped);
    std::printf("PASS %s terminal_submissions=%llu settled_submissions=%llu rejected=%u\n",label,
        static_cast<unsigned long long>(closeout::at_terminal),static_cast<unsigned long long>(closeout::at_stop),
        static_cast<unsigned>(rejected));
}
int main() {
    try {
        using closeout::Schedule;using closeout::Corruption;
        failure_case("accept-after-terminal",Schedule::before_acceptance);
        failure_case("stop-before-acceptance",Schedule::stop_before_acceptance);
        failure_case("accept-before-terminal",Schedule::after_acceptance);
        failure_case("ordinary-timeout",Schedule::ordinary);
        failure_case("completion-error",Schedule::ordinary,Corruption::none,false);
        for(const auto& [name,error]:std::array{
                std::pair{"extra-submission",Corruption::submissions},
                std::pair{"wrong-device-copy",Corruption::frames},
                std::pair{"wrong-logical-actions",Corruption::actions},
                std::pair{"extra-completion",Corruption::completions},
                std::pair{"extra-cancellation",Corruption::cancellations},
                std::pair{"unexpected-rejection",Corruption::rejection},
                std::pair{"missing-submit-attempt",Corruption::attempts},
                std::pair{"wrong-submit-result",Corruption::returned_status},
                std::pair{"missing-terminal",Corruption::terminal},
                std::pair{"published-output",Corruption::output}}) {
            failure_case(name,Schedule::after_acceptance,error);
            failure_case(name,Schedule::stop_before_acceptance,error);
        }
        closeout::reset(Schedule::ordinary);
        auto fixture=std::make_unique<b::detail::Loopback>(selected("device-loopback-2-flight1-8"));
        const auto result=fixture->run(0);
        closeout::check(result.correct && fixture->providers==2 && fixture->copied_count==4);
        closeout::check(fixture->backend.stats().submissions==2 && fixture->backend.stats().completions==2);
        closeout::check(fixture->finish()==rt::Status::ok);
        std::puts("PASS successful submission and copied output");
        return 0;
    } catch(const std::exception& error) {
        std::fprintf(stderr,"FAIL %s\n",error.what());return 1;
    }
}
