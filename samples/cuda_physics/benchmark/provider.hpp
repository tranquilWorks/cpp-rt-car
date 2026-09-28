#pragma once
#include "../pipeline/simulated_driver.hpp"
#include <rtfw/benchmark.hpp>
#include <span>

namespace rtfw::cuda_physics::benchmark {
namespace b=rtfw::benchmark;
namespace p=pipeline;
struct Case { const char* id; std::uint32_t entities,workers; bool graph,active; };
std::span<const Case> cases() noexcept;
// Host callback/step timestamps only. No native handles or device clock claims.
struct Correlation {
    std::uint64_t id{},invocation{},lane{},timeline_value{},commands{},host_submit_ns{},host_complete_ns{};
};
class Provider {
public:
    static constexpr std::size_t invocation_count=7, record_capacity=14;
    Provider();
    ~Provider();
    Provider(const Provider&)=delete;
    Provider& operator=(const Provider&)=delete;
    b::ProviderV1 table() noexcept;
    b::Status prepare(std::string_view, std::size_t capacity=record_capacity);
    b::Status finish() noexcept;
    std::span<const Correlation> correlations() const noexcept;
    // Configuring/serialized host-only fault injection for the source kit tests.
    void corrupt_next_output() noexcept;
private:
    struct State;
    std::unique_ptr<State> state_;
    static b::Status describe(void*,std::size_t,b::Descriptor&);
    static b::Status invoke(void*,std::string_view,std::uint64_t,b::Observation&);
};
}
