#pragma once
#include "../runtime_provider.hpp"
#include <rt/runtime.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdint>
#include <exception>
#include <memory>
#include <span>
#include <source_location>
#include <stdexcept>
#include <string>
#include <vector>

namespace rtfw::benchmark::runtime::detail {
inline void require(bool value,std::source_location where=std::source_location::current()) {
    if (!value) throw std::runtime_error(std::string(where.file_name())+":"+std::to_string(where.line())+": runtime fixture invariant");
}
inline void okay(rt::Status value,std::source_location where=std::source_location::current()) {
    if(value!=rt::Status::ok) throw std::runtime_error(std::string(where.file_name())+":"+
        std::to_string(where.line())+": "+rt::status_message(value));
}
inline std::uint64_t digest(std::span<const std::byte> bytes) noexcept {
    std::uint64_t value=14695981039346656037ULL;
    for (auto byte:bytes) { value^=std::to_integer<std::uint8_t>(byte); value*=1099511628211ULL; }
    return value;
}
inline void put(std::span<std::byte> bytes, std::uint64_t value) noexcept {
    for(std::size_t i=0;i<bytes.size();++i) bytes[i]=std::byte((value+i*17U)&255U);
}
inline bool matches(std::span<const std::byte> bytes, std::uint64_t value) noexcept {
    for(std::size_t i=0;i<bytes.size();++i)
        if(bytes[i]!=std::byte((value+i*17U)&255U)) return false;
    return true;
}
struct Clock final:rt::RuntimeClock {
    std::atomic<std::uint64_t> now{1000};
    std::uint64_t now_ns() noexcept override { return now.load(std::memory_order_acquire); }
    rt::Status sleep_until_ns(std::uint64_t release) noexcept override {
        auto current=now.load(std::memory_order_relaxed);
        while(current<release && !now.compare_exchange_weak(current,release,std::memory_order_release,std::memory_order_relaxed)) {}
        return rt::Status::ok;
    }
    bool supports_absolute_sleep() const noexcept override { return true; }
};
inline rt::RuntimeConfig config(std::size_t phases=8) {
    rt::RuntimeConfig c;
    c.callback_capacity=std::max(std::size_t{8},phases);
    c.worker_count=1; c.executor_queue_capacity=std::bit_ceil(std::max(std::size_t{128},phases*2));
    c.task_scratch_slots=c.executor_queue_capacity;
    c.scratch_bytes=64; c.task_scratch_bytes=64; c.trace_capacity=0;
    c.state_capacity=64; c.snapshot_max_bytes=8*1024*1024;
    c.replay_input_capacity=64; c.input_log_max_bytes=1024*1024;
    c.memory_budget_bytes=512U*1024U*1024U;
    return c;
}
struct RuntimeOwner {
    Clock clock;
    rt::Runtime rt{clock};
    ~RuntimeOwner() {
        // Finalize rejection keeps configuring; rollback is owned by Runtime.
        if(rt.state()!=rt::RuntimeState::configuring && rt.state()!=rt::RuntimeState::stopped &&
           rt.stop()!=rt::Status::ok) std::terminate();
    }
    rt::Status stop() noexcept {
        return rt.state()==rt::RuntimeState::configuring || rt.state()==rt::RuntimeState::stopped
            ? rt::Status::ok : rt.stop();
    }
};
struct Measures {
    std::uint64_t operations{},callbacks{},rejected{},records{},bytes{},transitions{},gaps{},checksum{};
    std::uint64_t rate_actions{},mixed_actions{},control_actions{};
    bool correct{true};
    void observe(Observation& out) const {
        out.counters={operations,callbacks,rejected,records,bytes,transitions,gaps,rate_actions,mixed_actions,control_actions};
        // The schema accepts signed-63-bit JSON integers; retain the low bits
        // of the observed digest, while fixture oracles compare full values.
        out.checksum=checksum & max_integer; out.correct=correct;
    }
};
struct Fixture {
    virtual ~Fixture()=default;
    virtual Measures run(std::uint64_t)=0;
    virtual rt::Status finish() noexcept=0;
};
inline rt::HostFrameContext frame(std::uint64_t ordinal,std::uint64_t duration,
                                  std::uint64_t release) {
    return {ordinal,std::chrono::nanoseconds{duration},std::nullopt,release};
}
inline void finalized(RuntimeOwner& owner) { if(owner.rt.finalize()!=rt::Status::ok) throw std::runtime_error(std::string(owner.rt.last_error()));okay(owner.rt.start()); }
inline std::vector<std::byte> checkpoint(rt::Runtime& runtime,std::uint64_t frame_index=0) {
    std::size_t size=0;okay(runtime.checkpoint_size(size));require(size<=16U*1024U*1024U);
    std::vector<std::byte> bytes(size);rt::ArtifactWriteResult result;
    okay(runtime.write_checkpoint(frame_index,bytes,result));require(result.bytes_written==size);
    return bytes;
}
inline std::uint64_t load64(std::span<const std::byte> bytes) noexcept {
    std::uint64_t value=0;
    for(std::size_t i=0;i<8;++i) value|=std::uint64_t(std::to_integer<unsigned char>(bytes[i]))<<(8*i);
    return value;
}
inline void store64(std::span<std::byte> bytes,std::uint64_t value) noexcept {
    for(std::size_t i=0;i<8;++i) bytes[i]=std::byte((value>>(8*i))&255U);
}
std::unique_ptr<Fixture> make_rates(const Case&);
std::unique_ptr<Fixture> make_channels(const Case&);
std::unique_ptr<Fixture> make_shedding(const Case&);
std::unique_ptr<Fixture> make_controls(const Case&);
std::unique_ptr<Fixture> make_checkpoint(const Case&);
std::unique_ptr<Fixture> make_replay(const Case&);
std::unique_ptr<Fixture> make_watchdog(const Case&);
std::unique_ptr<Fixture> make_telemetry(const Case&);
std::unique_ptr<Fixture> make_capacity(const Case&);
std::unique_ptr<Fixture> make_device(const Case&);
std::unique_ptr<Fixture> make_composition(const Case&);
} // namespace rtfw::benchmark::runtime::detail
