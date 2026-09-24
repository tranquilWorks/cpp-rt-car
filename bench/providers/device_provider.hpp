#pragma once

// Optional benchmark source example. All driver resources are host-owned.
#include <rtfw/benchmark.hpp>
#include <rt/cuda_backend.hpp>
#include <rt/xdma_backend.hpp>
#include <memory>
#include <span>

namespace rtfw::benchmark::device {
struct Case {
    const char* id;
    const char* operation;
    std::size_t bytes;
    std::size_t depth;
    bool real;
    bool allocation_free;
};
[[nodiscard]] std::span<const Case> cases() noexcept;

// The caller retains the context, stream, function and driver user until
// finish succeeds. Missing resources are NOT RUN; malformed supplied tables
// are errors. The supplied kernel increments each int32 element once and
// accepts (device address, uint32 element count).
struct CudaSession {
    rt::CudaDriverApi driver{};
    rt::CudaContext context{};
    rt::CudaStream stream{};
    rt::CudaFunction increment_kernel{};
    // Instantiated increment graph bound to this exact caller-owned device span.
    rt::CudaGraphExec increment_graph{};
    rt::CudaDeviceAddress graph_buffer{};
    std::uint64_t graph_bytes{};
};
struct XdmaSession {
    rt::XdmaDriverApi driver{};
    rt::XdmaBackendConfig config{};
    // Explicit operator-confirmed AXI-MM scratch window. Zero means absent.
    std::uint64_t device_offset{};
    std::uint64_t confirmed_window_bytes{};
};

class Provider {
public:
    explicit Provider(const CudaSession* = nullptr, const XdmaSession* = nullptr);
    ~Provider();
    Provider(const Provider&) = delete;
    Provider& operator=(const Provider&) = delete;
    [[nodiscard]] ProviderV1 table() noexcept;
    [[nodiscard]] Status prepare(std::string_view);
    [[nodiscard]] Status finish() noexcept;
private:
    struct State;
    std::unique_ptr<State> state_;
    static Status describe(void*, std::size_t, Descriptor&);
    static Status invoke(void*, std::string_view, std::uint64_t, Observation&);
};
} // namespace rtfw::benchmark::device
