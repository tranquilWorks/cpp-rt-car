#pragma once

// Installed example source, not an additional supported binary/source SDK API.
#include <rtfw/benchmark.hpp>
#include <memory>
#include <span>

namespace rtfw::benchmark::cpu {
enum class Kind { compile, graph, range, nested_for, nested_reduce, pressure,
                  memory, multiple, lifecycle };
enum class Policy { static_workers, throughput, host };
enum class Memory { normal, simulated, acquire_failure, apply_failure,
                    rollback_retry, native };
struct Case {
    const char* id;
    const char* family;
    Kind kind;
    std::size_t phases{1}, entities{64}, grain{16}, workers{1};
    Policy policy{Policy::static_workers};
    bool chain{false}, invalid_graph{false};
    std::size_t depth{0}, queue{512}, trace{0}, scratch{64}, cycles{1};
    Memory memory{Memory::normal};
};
[[nodiscard]] std::span<const Case> cases() noexcept;

// Single host owner. Metadata registration does no Runtime work. prepare creates
// only the selected fixture; finish MUST succeed before publishing a result.
class Provider {
public:
    Provider();
    ~Provider();
    Provider(const Provider&) = delete;
    Provider& operator=(const Provider&) = delete;
    [[nodiscard]] ProviderV1 table() noexcept;
    [[nodiscard]] Status prepare(std::string_view);
    [[nodiscard]] Status finish() noexcept;
    [[nodiscard]] bool prepared() const noexcept;
    [[nodiscard]] std::uint64_t invocations() const noexcept;
private:
    struct State;
    std::unique_ptr<State> state_;
    static Status describe(void*, std::size_t, Descriptor&);
    static Status invoke(void*, std::string_view, std::uint64_t, Observation&);
};
} // namespace rtfw::benchmark::cpu
