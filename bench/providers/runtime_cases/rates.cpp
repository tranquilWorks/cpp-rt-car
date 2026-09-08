#include "support.hpp"
#include <numeric>

namespace rtfw::benchmark::runtime::detail {
namespace {
struct Rates final:Fixture {
    Case c;
    struct Probe {
        std::uint64_t calls{},sum{};
        std::size_t substeps{1};
        bool valid{true};
    };
    std::array<Probe,64> probes{};
    std::array<rt::PhaseHandle,64> phases{};
    std::array<rt::RateDomainHandle,64> domains{};
    RuntimeOwner owner; // Dies before every callback/clock-borrowed input above.
    static rt::CallbackResult callback(void* opaque,const rt::CallbackContext& ctx) {
        auto& p=*static_cast<Probe*>(opaque);
        if(!ctx.rate_release) return rt::CallbackResult::error;
        const auto& r=*ctx.rate_release;
        p.valid &= r.domain_release_sequence==p.calls/p.substeps &&
            r.substep_ordinal==p.calls%p.substeps &&
            r.logical_release_ns==(p.calls/p.substeps)*100U;
        p.sum+=r.domain_release_sequence*7U+r.substep_ordinal+1U;++p.calls;
        return p.valid?rt::CallbackResult::ok:rt::CallbackResult::error;
    }
    static rt::CallbackResult unused(void*,const rt::CallbackContext&) {return rt::CallbackResult::error;}
    explicit Rates(const Case& value):c(value) {
        okay(owner.rt.configure(config(64)));
        const bool inspect=std::string_view(c.mode)=="reference";
        const auto width=inspect?std::size_t{2}:c.width;
        if(!inspect) okay(owner.rt.set_rate_execution_policy({c.count*c.width*c.variant,1,1,1,0}));
        for(std::size_t i=0;i<width;++i) {
            const auto name="rate-"+std::to_string(i);
            probes[i].substeps=inspect?1:c.variant;
            okay(owner.rt.register_callback({name,inspect?unused:callback,&probes[i]},phases[i]));
            const std::uint64_t period=inspect?(i?c.count-1:1):100;
            okay(owner.rt.register_rate_domain({name,period,
                static_cast<std::uint32_t>(inspect?1:c.variant),period,1},domains[i]));
            okay(owner.rt.bind_phase_to_rate_domain(phases[i],domains[i]));
        }
        finalized(owner);
        if(inspect) require(owner.rt.reference_release_count()==c.count);
    }
    Measures run(std::uint64_t ordinal) override {
        Measures m;
        if(std::string_view(c.mode)=="reference") {
            for(std::size_t i=0;i<c.count;++i) {
                rt::ReferenceRelease r;require(owner.rt.reference_release_at(i,r));
                const auto d=i==1?1U:0U;
                const auto seq=i<2?0:i-1;
                m.correct &= r.domain==domains[d] && r.phase==phases[d] &&
                    r.domain_release_sequence==seq && r.release_time_ns==seq;
                ++m.operations;++m.records;m.checksum+=r.release_time_ns+1;
            }
        } else {
            const auto before=std::accumulate(probes.begin(),probes.end(),std::uint64_t{},
                [](std::uint64_t n,const Probe& p){return n+p.calls;});
            const auto duration=c.count*100U,release=1000U+ordinal*duration;
            owner.clock.now=release;
            rt::StepResult result;okay(owner.rt.step(frame(ordinal,duration,release),&result));
            const auto after=std::accumulate(probes.begin(),probes.end(),std::uint64_t{},
                [](std::uint64_t n,const Probe& p){return n+p.calls;});
            m.operations=after-before;m.callbacks=result.callbacks_executed;
            m.records=result.rate.executed_reference_records;m.rejected=result.rate.rejected_reference_records;
            m.correct=m.operations==c.count*c.width*c.variant && m.records==m.operations &&
                m.callbacks==m.operations && !m.rejected;
            for(std::size_t i=0;i<c.width;++i) m.correct &= probes[i].valid;
            const auto releases=c.count*(ordinal+1),substeps=c.variant;
            const auto expected_sum=7*substeps*releases*(releases-1)/2+
                releases*substeps+releases*substeps*(substeps-1)/2;
            for(std::size_t i=0;i<c.width;++i) m.correct &= probes[i].sum==expected_sum;
            // A directly observed callback sum; ordinal-dependent, never a fixed replacement.
            for(std::size_t i=0;i<c.width;++i) m.checksum+=probes[i].sum;
        }
        return m;
    }
    rt::Status finish() noexcept override {return owner.stop();}
};

struct Channels final:Fixture {
    Case c;
    struct Endpoint {Channels* fixture;std::size_t index;};
    std::vector<Endpoint> endpoints;
    std::vector<rt::PhaseHandle> producers;
    std::vector<rt::CrossRateChannelHandle> channels;
    std::vector<std::byte> produced,copied,initial;
    rt::PhaseHandle producer{},consumer{};
    rt::RateDomainHandle producer_domain{},consumer_domain{};
    std::uint64_t producer_period{},consumer_period{},published{},reads{},calls{},stale{},held{},sum{};
    bool valid{true};
    RuntimeOwner owner;
    static rt::CallbackResult produce(void* opaque,const rt::CallbackContext& ctx) {
        const auto& endpoint=*static_cast<Endpoint*>(opaque);
        auto& s=*endpoint.fixture;
        if(!ctx.rate_release) return rt::CallbackResult::error;
        const auto seed=ctx.rate_release->domain_release_sequence+1;
        put(s.produced,seed);
        if(ctx.rate_release->publish(s.channels[endpoint.index],s.produced)!=rt::Status::ok) return rt::CallbackResult::error;
        ++s.published;
        ++s.calls;return rt::CallbackResult::ok;
    }
    static rt::CallbackResult consume(void* opaque,const rt::CallbackContext& ctx) {
        auto& s=*static_cast<Channels*>(opaque);
        if(!ctx.rate_release) return rt::CallbackResult::error;
        const auto time=ctx.rate_release->logical_release_ns;
        const bool initial_only=s.c.variant==2;
        const auto seq=time/s.producer_period;
        const auto age=time%s.producer_period;
        for(auto h:s.channels) {
            rt::CrossRateReadResult read;
            const auto status=ctx.rate_release->copy(h,s.copied,read);
            if(status!=rt::CrossRateReadStatus::ok) return rt::CallbackResult::error;
            s.valid &= matches(s.copied,initial_only?0:seq+1);
            if(initial_only) s.valid &= read.provenance==rt::CrossRateSampleProvenance::initial_sample;
            else s.valid &= read.producer_release_sequence==seq && read.producer_substep_ordinal==0 &&
                read.producer_completion_status==rt::Status::ok &&
                read.producer_timestamp_domain_identity==rt::cross_rate_runtime_logical_timestamp_domain_identity;
            if(!initial_only) s.valid &= read.age_ns==age;
            s.held+=read.held;s.stale+=read.freshness==rt::CrossRateFreshness::stale;
            s.sum+=digest(s.copied);++s.reads;
        }
        ++s.calls;return s.valid?rt::CallbackResult::ok:rt::CallbackResult::error;
    }
    explicit Channels(const Case& value):c(value),endpoints(c.width),producers(c.width),channels(c.width),produced(c.bytes),
        copied(c.bytes),initial(c.bytes) {
        producer_period=c.variant==1 || c.variant==3?200:100;
        consumer_period=c.variant==0?200:100;
        if(c.width==256){producer_period*=10;consumer_period*=10;}
        put(initial,0);
        okay(owner.rt.configure(config(c.width+1)));
        okay(owner.rt.set_rate_execution_policy({65536,1,1,1,0}));
        for(std::size_t i=0;i<c.width;++i) {
            endpoints[i]={this,i};
            okay(owner.rt.register_callback({"producer-"+std::to_string(i),produce,&endpoints[i]},producers[i]));
        }
        producer=producers[0];
        okay(owner.rt.register_callback({"consumer",consume,this},consumer));
        okay(owner.rt.register_rate_domain({"producer-rate",producer_period,1,
            c.variant==2?10:producer_period,1,rt::RateCriticality::normal,false,
            c.variant==2?rt::RateLateAction::hold:rt::RateLateAction::fail,0},producer_domain));
        okay(owner.rt.register_rate_domain({"consumer-rate",consumer_period,1,consumer_period,1},consumer_domain));
        for(auto phase:producers) okay(owner.rt.bind_phase_to_rate_domain(phase,producer_domain));
        okay(owner.rt.bind_phase_to_rate_domain(consumer,consumer_domain));
        for(std::size_t i=0;i<c.width;++i) okay(owner.rt.register_cross_rate_channel({
            "channel-"+std::to_string(i),producers[i],consumer,c.bytes,initial,
            rt::CrossRateMode::sample_and_hold,c.variant==3?0U:1000U},channels[i]));
        finalized(owner);
    }
    Measures run(std::uint64_t ordinal) override {
        Measures m;
        if(std::string_view(c.mode)=="selection") {
            for(std::size_t i=0;i<owner.rt.cross_rate_selection_count();++i) {
                rt::CompiledCrossRateSelection r;require(owner.rt.compiled_cross_rate_selection_at(i,r));
                const auto t=r.consumer_release_sequence*consumer_period;
                m.correct &= r.producer_release_sequence==t/producer_period &&
                    r.age_ns==t%producer_period &&
                    std::find(producers.begin(),producers.end(),r.producer)!=producers.end() && r.consumer==consumer;
                ++m.operations;++m.records;m.checksum+=r.producer_release_sequence+1;
            }
        } else {
            const auto prev_reads=reads,prev_calls=calls,prev_published=published,prev_stale=stale,prev_held=held;
            const auto duration=std::max(producer_period,consumer_period)*c.count;
            // Initial hold must be applied separately per release to stay late for every producer.
            const auto steps=c.variant==2?duration/100U:1U;
            for(std::uint64_t i=0;i<steps;++i) {
                const auto step_duration=duration/steps;
                const auto release=1000U+ordinal*duration+i*step_duration;
                owner.clock.now=release+(c.variant==2?50:0);
                rt::StepResult result;
                okay(owner.rt.step(frame(ordinal*steps+i,step_duration,release),&result));
                m.rejected+=result.rate.rejected_reference_records;
            }
            m.operations=reads-prev_reads;m.callbacks=calls-prev_calls;m.records=published-prev_published;
            m.bytes=m.operations*c.bytes;m.gaps=stale-prev_stale;m.transitions=held-prev_held;
            m.correct=valid && m.operations==duration/consumer_period*c.width &&
                m.records==(c.variant==2?0:duration/producer_period*c.width);
            if(c.variant==3) m.correct &= m.gaps==c.count*c.width;
            if(c.variant==2) m.correct &= m.transitions==m.operations;
            m.checksum=sum;
        }
        return m;
    }
    rt::Status finish() noexcept override {return owner.stop();}
};

struct Shedding final:Fixture {
    Case c;
    std::array<std::uint64_t,3> calls{};
    std::vector<std::byte> initial;
    std::array<rt::RateActionRecord,1024> actions{};
    RuntimeOwner owner;
    static rt::CallbackResult callback(void* opaque,const rt::CallbackContext& context) {
        if(!context.rate_release) return rt::CallbackResult::error;
        ++*static_cast<std::uint64_t*>(opaque);return rt::CallbackResult::ok;
    }
    explicit Shedding(const Case& value):c(value) {
        okay(owner.rt.configure(config()));
        okay(owner.rt.set_rate_execution_policy({16,7,static_cast<std::uint32_t>(c.count),
            static_cast<std::uint32_t>(c.count),c.capacity}));
        for(std::size_t i=0;i<3;++i) {
            const auto name="shed-"+std::to_string(i);
            rt::PhaseHandle phase;rt::RateDomainHandle domain;
            okay(owner.rt.register_callback({name,callback,&calls[i]},phase));
            okay(owner.rt.register_rate_domain({name,100,1,50,1,
                i==0?rt::RateCriticality::critical:rt::RateCriticality::background,i!=0,
                rt::RateLateAction::skip,0},domain));
            okay(owner.rt.bind_phase_to_rate_domain(phase,domain));
        }
        finalized(owner);initial=checkpoint(owner.rt);
    }
    Measures run(std::uint64_t ordinal) override {
        okay(owner.rt.restore_checkpoint(initial));calls={};
        Measures m;rt::RateTelemetryCursor cursor;
        rt::RateTelemetryMetadata before;okay(owner.rt.rate_telemetry_metadata(before));
        cursor.runtime_id=before.runtime_id;cursor.next_sequence=before.next_sequence;
        std::array<std::uint32_t,4> transitioned{};std::size_t transitions=0;
        for(std::uint64_t i=0;i<4*c.count;++i) {
            const auto release=1000U+ordinal*(4*c.count+10)*100U+i*100U;
            owner.clock.now=release+(i<2*c.count?75:0);
            rt::StepResult result;okay(owner.rt.step(frame(i,100,release),&result));
            ++m.operations;m.callbacks+=result.callbacks_executed;
            m.rejected+=result.rate.rejected_reference_records;
            m.transitions+=result.rate.shed_transitions+result.rate.recovery_transitions;
        }
        rt::RateTelemetryReadResult read;okay(owner.rt.read_rate_actions(cursor,actions,read));
        for(std::size_t i=0;i<read.records_read;++i) {
            if(actions[i].transition!=rt::RateTransitionId::none) {
                if(transitions>=transitioned.size()) {m.correct=false;break;}
                transitioned[transitions++]=actions[i].transition_domain_registration_index;
            }
        }
        rt::RateCounterSnapshot snapshot;okay(owner.rt.rate_counters_snapshot(snapshot));
        m.records=read.records_read;m.gaps=read.lost_records;m.checksum=calls[0]*13+calls[1]*7+calls[2];
        m.rate_actions=read.records_read;
        require(m.transitions==4);
        require(transitions==4);
        require(transitioned==std::array<std::uint32_t,4>{2,1,1,2});
        require(calls[0]==2*c.count);
        require(snapshot.values[16]==0 && !m.gaps);
        return m;
    }
    rt::Status finish() noexcept override {return owner.stop();}
};
}
std::unique_ptr<Fixture> make_rates(const Case& c){return std::make_unique<Rates>(c);}
std::unique_ptr<Fixture> make_channels(const Case& c){return std::make_unique<Channels>(c);}
std::unique_ptr<Fixture> make_shedding(const Case& c){return std::make_unique<Shedding>(c);}
} // namespace rtfw::benchmark::runtime::detail
