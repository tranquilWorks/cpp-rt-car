#include "runtime_cases/support.hpp"
#include <cstring>

namespace rtfw::benchmark::runtime {
namespace {
constexpr Case catalog[]{
#include "runtime_cases/catalog.inc"
};
}
std::span<const Case> cases() noexcept {return catalog;}
const char* family_name(Family f) noexcept {
    switch(f) {
    case Family::rates:return "rates";case Family::channels:return "channels";
    case Family::shedding:return "shedding";case Family::controls:return "controls";
    case Family::checkpoint:return "checkpoint";case Family::replay:return "replay";
    case Family::watchdog:return "watchdog";case Family::telemetry:return "telemetry";
    case Family::capacity:return "capacity";case Family::device:return "device";
    case Family::composition:return "composition";
    } return "invalid";
}
struct Provider::State {
    const Case* selected{};
    std::unique_ptr<detail::Fixture> fixture;
    std::uint64_t calls{};
    bool failed{};
};
Provider::Provider():state_(std::make_unique<State>()) {}
Provider::~Provider() {if(finish()!=Status::ok) std::terminate();}
ProviderV1 Provider::table() noexcept {
    ProviderV1 p;p.id="rtfw.runtime";p.case_count=std::size(catalog);p.user=this;
    p.describe=describe;p.invoke=invoke;return p;
}
Status Provider::describe(void*,std::size_t index,Descriptor& d) {
    if(index>=std::size(catalog)) return Status::not_found;
    const auto& c=catalog[index];d={};d.case_id=c.id;d.subsystem=family_name(c.family);
    d.implementation="public-runtime-v1";d.configuration=c.scope;d.workload_kind=c.mode;
    const auto text=std::string(c.id)+"|"+c.scope+"|"+std::to_string(c.count)+"|"+
        std::to_string(c.width)+"|"+std::to_string(c.bytes)+"|"+
        std::to_string(c.capacity)+"|"+std::to_string(c.variant);
    d.workload_sha256=sha256(text);
    d.parameters={{"count",c.count,c.count,c.count},{"width",c.width,c.width,c.width},
        {"bytes",c.bytes,c.bytes,c.bytes},{"capacity",c.capacity,c.capacity,c.capacity},
        {"variant",c.variant,c.variant,c.variant}};
    d.counters={{"operations",c.unit},{"callbacks","count"},{"rejected","count"},
        {"records","count"},{"bytes","bytes"},{"transitions","count"},{"gaps","count"},
        {"rate_actions","records"},{"mixed_actions","records"},{"control_actions","records"}};
    return Status::ok;
}
Status Provider::prepare(std::string_view id) {
    if(state_->fixture) return Status::busy;
    const auto it=std::find_if(std::begin(catalog),std::end(catalog),[&](const Case& c){return id==c.id;});
    if(it==std::end(catalog)) return Status::not_found;
    std::unique_ptr<detail::Fixture> fixture;
    switch(it->family) {
    case Family::rates:fixture=detail::make_rates(*it);break;
    case Family::channels:fixture=detail::make_channels(*it);break;
    case Family::shedding:fixture=detail::make_shedding(*it);break;
    case Family::controls:fixture=detail::make_controls(*it);break;
    case Family::checkpoint:fixture=detail::make_checkpoint(*it);break;
    case Family::replay:fixture=detail::make_replay(*it);break;
    case Family::watchdog:fixture=detail::make_watchdog(*it);break;
    case Family::telemetry:fixture=detail::make_telemetry(*it);break;
    case Family::capacity:fixture=detail::make_capacity(*it);break;
    case Family::device:fixture=detail::make_device(*it);break;
    case Family::composition:fixture=detail::make_composition(*it);break;
    }
    if(!fixture) return Status::provider_error;
    state_->selected=it;state_->fixture=std::move(fixture);state_->calls=0;state_->failed=false;
    return Status::ok;
}
Status Provider::invoke(void* opaque,std::string_view id,std::uint64_t ordinal,Observation& out) {
    auto& s=*static_cast<Provider*>(opaque)->state_;
    if(!s.fixture || s.failed || id!=s.selected->id || ordinal!=s.calls) return Status::invalid;
    try {
        const auto m=s.fixture->run(ordinal);++s.calls;m.observe(out);
        if(!m.correct){s.failed=true;return Status::invariant_failed;}
        return Status::ok;
    } catch(...) {s.failed=true;return Status::invariant_failed;}
}
Status Provider::finish() noexcept {
    if(state_->fixture && state_->fixture->finish()!=rt::Status::ok) return Status::provider_error;
    state_->fixture.reset();state_->selected=nullptr;return Status::ok;
}
bool Provider::prepared() const noexcept {return static_cast<bool>(state_->fixture);}
std::uint64_t Provider::invocations() const noexcept {return state_->calls;}
} // namespace rtfw::benchmark::runtime
