#include "control_support.hpp"
#include <rt/observability_export.hpp>
#include <ostream>
#include <streambuf>

namespace rtfw::benchmark::runtime::detail {
namespace {
constexpr std::uint64_t watchdog_budget=60'000'000'000;
constexpr std::uint64_t watchdog_period=2*watchdog_budget;
std::uint64_t metric(const rt::RuntimeMetricSnapshot& snapshot,rt::RuntimeMetricId id) {
    const auto index=static_cast<std::size_t>(id);
    require(snapshot.sample_count==rt::runtime_metric_count && snapshot.samples[index].id==id);
    return snapshot.samples[index].value;
}
struct Watchdog final:Fixture {
    Case c;
    std::uint64_t calls{};
    std::uint32_t expected{};
    bool valid{true};
    RuntimeOwner owner;
    static rt::CallbackResult work(void* opaque,const rt::CallbackContext& ctx) {
        auto& s=*static_cast<Watchdog*>(opaque);
        s.valid &= ctx.degradation_level==s.expected;++s.calls;
        s.owner.clock.now.fetch_add(s.c.variant==2?watchdog_budget+1:5U,std::memory_order_acq_rel);
        return rt::CallbackResult::ok;
    }
    explicit Watchdog(const Case& value):c(value) {
        auto cfg=config();cfg.watchdog_timeout_ns=c.variant==0?0:watchdog_budget;
        cfg.watchdog_max_degradation_level=static_cast<std::uint32_t>(c.capacity);
        cfg.trace_capacity=1024;okay(owner.rt.configure(cfg));
        okay(owner.rt.register_callback({"logical-work",work,this}));finalized(owner);
    }
    Measures run(std::uint64_t ordinal) override {
        Measures m;const auto before=calls;
        for(std::size_t i=0;i<c.count;++i) {
            expected=owner.rt.degradation_level();
            const auto index=ordinal*c.count+i,release=1000+index*watchdog_period;
            owner.clock.now=release;rt::StepResult result;
            okay(owner.rt.step(frame(index,watchdog_period,release),&result));++m.operations;
            const bool expired=c.variant==2;
            require(result.watchdog_fired==expired);
            const auto next=expired?std::min(expected+1,static_cast<std::uint32_t>(c.capacity)):expected;
            require(owner.rt.degradation_level()==next);m.transitions+=next!=expected;
            m.records+=result.watchdog_fired;
        }
        rt::RuntimeMetricSnapshot snapshot;
        okay(owner.rt.metrics_snapshot(rt::RuntimeMetricWindow::cumulative,nullptr,snapshot));
        require(metric(snapshot,rt::RuntimeMetricId::watchdog_events)==(c.variant==2?(ordinal+1)*c.count:0));
        m.callbacks=calls-before;m.correct=valid && m.callbacks==c.count;
        m.checksum=(calls<<8U)|owner.rt.degradation_level();return m;
    }
    rt::Status finish() noexcept override{return owner.stop();}
};
class BoundedSink final:public std::streambuf {
public:
    explicit BoundedSink(std::span<char> bytes){setp(bytes.data(),bytes.data()+bytes.size());}
    std::size_t size() const noexcept{return static_cast<std::size_t>(pptr()-pbase());}
};
struct Telemetry final:Fixture {
    Case c;
    std::array<rt::RuntimeTraceEvent,1024> events{};
    std::array<char,1024*1024> storage{};
    rt::RuntimeMetricCursor metric_cursor;
    rt::RuntimeTraceCursor trace_cursor;
    std::uint64_t calls{},frames{};
    RuntimeOwner owner;
    static rt::CallbackResult work(void* opaque,const rt::CallbackContext&) {
        ++static_cast<Telemetry*>(opaque)->calls;return rt::CallbackResult::ok;
    }
    explicit Telemetry(const Case& value):c(value) {
        auto cfg=config();cfg.trace_capacity=c.capacity;okay(owner.rt.configure(cfg));
        okay(owner.rt.register_callback({"observable-frame",work,this}));finalized(owner);
        rt::RuntimeMetricSnapshot snapshot;
        okay(owner.rt.metrics_snapshot(rt::RuntimeMetricWindow::interval,&metric_cursor,snapshot));
        rt::RuntimeTraceReadResult read;
        okay(owner.rt.read_trace(trace_cursor,{},read));
    }
    Measures run(std::uint64_t) override {
        Measures m;const auto previous_sequence=trace_cursor.next_sequence,before_calls=calls;
        for(std::size_t i=0;i<c.count;++i) {
            const auto release=1000+frames*100;owner.clock.now=release;
            okay(owner.rt.step(frame(frames,100,release)));++frames;++m.operations;
        }
        rt::RuntimeMetricSnapshot cumulative,interval;
        okay(owner.rt.metrics_snapshot(rt::RuntimeMetricWindow::cumulative,nullptr,cumulative));
        require(cumulative.metadata.schema_version==2 && cumulative.metadata.trace_capacity==c.capacity);
        require(metric(cumulative,rt::RuntimeMetricId::frames_completed)==frames);
        const auto old_metric=metric_cursor;const auto old_trace=trace_cursor;
        if(std::string_view(c.mode)=="export") {
            BoundedSink short_sink(std::span<char>(storage).first(8));std::ostream bad(&short_sink);
            require(rt::write_observability_json(owner.rt,bad,rt::RuntimeMetricWindow::interval,&metric_cursor,&trace_cursor)==rt::Status::internal_error);
            require(metric_cursor.counters==old_metric.counters && metric_cursor.window_end_ns==old_metric.window_end_ns &&
                    metric_cursor.runtime_id==old_metric.runtime_id && trace_cursor.runtime_id==old_trace.runtime_id &&
                    trace_cursor.next_sequence==old_trace.next_sequence);++m.rejected;
            BoundedSink sink(storage);std::ostream stream(&sink);
            okay(rt::write_observability_json(owner.rt,stream,rt::RuntimeMetricWindow::interval,&metric_cursor,&trace_cursor));
            const std::string_view json(storage.data(),sink.size());m.bytes=json.size();
            require(json.find("\"schema_version\":2")!=std::string_view::npos &&
                    json.find("payload")==std::string_view::npos && json.find("0x")==std::string_view::npos);
            // Count retained records with an independent cursor; export owns the
            // transactional cursor being measured above.
        } else {
            okay(owner.rt.metrics_snapshot(rt::RuntimeMetricWindow::interval,&metric_cursor,interval));
            require(metric(interval,rt::RuntimeMetricId::frames_completed)==c.count);
            require(interval.window_start_ns==old_metric.window_end_ns);
        }
        auto read_cursor=old_trace;rt::RuntimeTraceReadResult read;
        okay(owner.rt.read_trace(read_cursor,events,read));m.records=read.events_read;m.gaps=read.lost_events;
        require(read.events_read<=c.capacity && read.lost_events==(read.first_sequence>previous_sequence?read.first_sequence-previous_sequence:0));
        for(std::size_t i=0;i<read.events_read;++i) {
            require(events[i].schema_version==2 && events[i].record_size==64);
            require(events[i].sequence==read.first_sequence+i);
        }
        if(std::string_view(c.mode)!="export") trace_cursor=read_cursor;
        auto foreign_metric=metric_cursor;foreign_metric.runtime_id^=0x8000000000000000ULL;
        require(owner.rt.metrics_snapshot(rt::RuntimeMetricWindow::interval,&foreign_metric,interval)==rt::Status::invalid_argument);++m.rejected;
        auto foreign_trace=trace_cursor;foreign_trace.runtime_id^=0x8000000000000000ULL;
        require(owner.rt.read_trace(foreign_trace,events,read)==rt::Status::invalid_argument);++m.rejected;
        require(c.capacity!=0 || (m.records==0 && metric(cumulative,rt::RuntimeMetricId::trace_events_emitted)==0));
        if(c.count*4>c.capacity && c.capacity!=0) require(m.records==c.capacity);
        m.callbacks=calls-before_calls;m.correct=calls==frames;m.checksum=calls;return m;
    }
    rt::Status finish() noexcept override{return owner.stop();}
};
struct Actions final:Fixture {
    Case c;
    std::array<rt::RateActionRecord,1024> rates{};
    std::array<rt::MixedRateActionRecord,1024> mixed{};
    std::array<rt::LiveControlActionRecord,1024> controls{};
    std::array<std::byte,8> payload{};
    rt::LiveControlProducerHandle handle;
    rt::RateTelemetryCursor rate_cursor;
    rt::MixedRateActionCursor mixed_cursor;
    rt::LiveControlActionCursor control_cursor;
    std::uint64_t calls{},frames{};
    RuntimeOwner owner;
    static rt::CallbackResult work(void* opaque,const rt::CallbackContext& ctx) {
        auto& s=*static_cast<Actions*>(opaque);
        if(!ctx.live_control || ctx.live_control->records.size()!=1 ||
           load64(ctx.live_control->records[0].payload)!=ctx.frame.frame_index) return rt::CallbackResult::error;
        ++s.calls;return rt::CallbackResult::ok;
    }
    explicit Actions(const Case& value):c(value) {
        okay(owner.rt.configure(config()));okay(owner.rt.set_rate_execution_policy({4,23,1,1,1024}));
        okay(owner.rt.set_mixed_rate_closure_policy({23,1024,0,0,0,rt::MixedRateOverflowPolicy::overwrite_committed,false,true,{}}));
        configure_controls(owner.rt,1,1,8,8,true);
        rt::PhaseHandle phase;rt::RateDomainHandle domain;
        okay(owner.rt.register_callback({"actions",work,this},phase));
        okay(owner.rt.register_rate_domain({"actions-rate",100,1,100,1},domain));
        okay(owner.rt.bind_phase_to_rate_domain(phase,domain));finalized(owner);
        okay(owner.rt.live_control_producer_handle(101,1001,handle));
        rt::RateTelemetryMetadata r;okay(owner.rt.rate_telemetry_metadata(r));rate_cursor.runtime_id=r.runtime_id;rate_cursor.next_sequence=r.next_sequence;
        rt::MixedRateActionMetadata m;okay(owner.rt.mixed_rate_action_metadata(m));mixed_cursor.runtime_id=m.runtime_id;mixed_cursor.next_sequence=m.next_sequence;
        rt::LiveControlActionMetadata l;okay(owner.rt.live_control_action_metadata(l));control_cursor.runtime_id=l.runtime_id;
        control_cursor.configuration_generation=l.configuration_generation;control_cursor.next_sequence=l.next_sequence;
    }
    Measures run(std::uint64_t) override {
        Measures m;const auto rate_start=rate_cursor.next_sequence,mixed_start=mixed_cursor.next_sequence,control_start=control_cursor.next_sequence,before_calls=calls;
        for(std::size_t i=0;i<c.count;++i) {
            store64(payload,frames+1);auto update=update_record(handle,frames+1,payload,frames+1);
            rt::LiveControlAdmissionResult result;okay(owner.rt.stage_live_control_update(handle,update,payload,result));
            require(result==rt::LiveControlAdmissionResult::accepted);
            const auto release=1000+frames*100;owner.clock.now=release;
            okay(owner.rt.step(frame(frames+1,100,release)));++frames;++m.operations;
        }
        rt::RateTelemetryReadResult r;okay(owner.rt.read_rate_actions(rate_cursor,rates,r));
        rt::MixedRateActionReadResult x;okay(owner.rt.read_mixed_rate_actions(mixed_cursor,mixed,x));
        rt::LiveControlActionReadResult l;okay(owner.rt.read_live_control_actions(control_cursor,controls,l));
        require(r.records_read==1024 && x.records_read==1024 && l.records_read==1024);
        require(r.lost_records==r.first_sequence-rate_start && x.lost_records==x.first_sequence-mixed_start && l.lost_records==l.first_sequence-control_start);
        require(r.metadata.next_sequence-rate_start==c.count && x.metadata.next_sequence-mixed_start==c.count);
        for(std::size_t i=0;i<1024;++i) require(rates[i].schema_version==1 && rates[i].sequence==r.first_sequence+i &&
            mixed[i].schema_version==1 && mixed[i].sequence==x.first_sequence+i && controls[i].schema_version==1 && controls[i].sequence==l.first_sequence+i);
        rt::LiveControlCommitInfo info;require(owner.rt.live_control_commit_info(info));
        require(info.committed==frames && info.staged_occupancy==0 && calls==frames);
        m.callbacks=calls-before_calls;m.rate_actions=r.records_read;m.mixed_actions=x.records_read;m.control_actions=l.records_read;
        m.gaps=r.lost_records+x.lost_records+l.lost_records;m.checksum=calls;return m;
    }
    rt::Status finish() noexcept override{return owner.stop();}
};
}
std::unique_ptr<Fixture> make_watchdog(const Case& c){return std::make_unique<Watchdog>(c);}
std::unique_ptr<Fixture> make_telemetry(const Case& c){
    return std::string_view(c.mode)=="actions"?std::unique_ptr<Fixture>(std::make_unique<Actions>(c)):std::make_unique<Telemetry>(c);
}
} // namespace rtfw::benchmark::runtime::detail
