#include "device_provider.hpp"
#include "device_cases/fake_cuda.hpp"
#include "device_cases/fake_xdma.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <thread>
#include <rt/runtime.hpp>

namespace rtfw::benchmark::device {
namespace {
constexpr std::array catalog{
    Case{"hal-cuda-v1-depth-1", "hal-cuda-v1", 64, 1, false, true},
    Case{"hal-cuda-v1-depth-4", "hal-cuda-v1", 64, 4, false, true},
    Case{"hal-cuda-v2-depth-1", "hal-cuda-v2", 64, 1, false, true},
    Case{"hal-cuda-v2-depth-4", "hal-cuda-v2", 64, 4, false, true},
    Case{"hal-xdma-v1-depth-1", "hal-xdma-v1", 64, 1, false, true},
    Case{"hal-xdma-v1-depth-4", "hal-xdma-v1", 64, 4, false, true},
    Case{"hal-xdma-v2-depth-1", "hal-xdma-v2", 64, 1, false, true},
    Case{"hal-xdma-v2-depth-4", "hal-xdma-v2", 64, 4, false, true},
    Case{"pipeline-kernel-64-frames-1", "pipeline-kernel", 64, 1, false, true},
    Case{"pipeline-kernel-64-frames-4", "pipeline-kernel", 64, 4, false, true},
    Case{"pipeline-kernel-4096-frames-1", "pipeline-kernel", 4096, 1, false, true},
    Case{"pipeline-kernel-4096-frames-4", "pipeline-kernel", 4096, 4, false, true},
    Case{"pipeline-graph-64-frames-1", "pipeline-graph", 64, 1, false, true},
    Case{"pipeline-graph-64-frames-4", "pipeline-graph", 64, 4, false, true},
    Case{"pipeline-graph-4096-frames-1", "pipeline-graph", 4096, 1, false, true},
    Case{"pipeline-graph-4096-frames-4", "pipeline-graph", 4096, 4, false, true},
    Case{"real-pipeline-kernel-64", "pipeline-kernel", 64, 1, true, true},
    Case{"real-pipeline-kernel-4096", "pipeline-kernel", 4096, 1, true, true},
    Case{"real-pipeline-graph-64", "pipeline-graph", 64, 1, true, true},
    Case{"real-pipeline-graph-4096", "pipeline-graph", 4096, 1, true, true},
    Case{"host-staging-64", "host-staging", 64, 1, false, true},
    Case{"host-staging-4096", "host-staging", 4096, 1, false, true},
    Case{"pipeline-graph-failure", "pipeline-graph", 64, 1, false, false},
    Case{"cuda-roundtrip-64", "roundtrip", 64, 1, false, true},
    Case{"cuda-roundtrip-4096", "roundtrip", 4096, 1, false, true},
    Case{"cuda-kernel-64", "kernel", 64, 1, false, true},
    Case{"cuda-kernel-4096", "kernel", 4096, 1, false, true},
    Case{"cuda-device-copy-64", "device-copy", 64, 1, false, true},
    Case{"cuda-device-copy-4096", "device-copy", 4096, 1, false, true},
    Case{"cuda-memset-64", "memset", 64, 1, false, true},
    Case{"cuda-memset-4096", "memset", 4096, 1, false, true},
    Case{"cuda-graph-64", "graph", 64, 1, false, true},
    Case{"cuda-graph-4096", "graph", 4096, 1, false, true},
    Case{"real-cuda-graph-64", "graph", 64, 1, true, true},
    Case{"real-cuda-graph-4096", "graph", 4096, 1, true, true},
    Case{"cuda-depth-1", "depth", 64, 1, false, true},
    Case{"cuda-depth-4", "depth", 64, 4, false, true},
    Case{"cuda-invalid-inputs", "invalid-inputs", 64, 1, false, false},
    Case{"xdma-invalid-inputs", "xdma-invalid-inputs", 64, 1, false, false},
    Case{"cuda-empty-poll", "empty-poll", 64, 1, false, true},
    Case{"cuda-not-ready-poll", "not-ready-poll", 64, 1, false, true},
    Case{"cuda-timeout", "timeout", 64, 1, false, true},
    Case{"cuda-submit-failure-recovery", "fault-submit", 64, 1, false, false},
    Case{"cuda-context-loss", "fault-context", 64, 1, false, false},
    Case{"cuda-cleanup-retry", "fault-cleanup", 64, 1, false, false},
    Case{"real-cuda-roundtrip-64", "roundtrip", 64, 1, true, true},
    Case{"real-cuda-roundtrip-4096", "roundtrip", 4096, 1, true, true},
    Case{"real-cuda-kernel-64", "kernel", 64, 1, true, true},
    Case{"real-cuda-kernel-4096", "kernel", 4096, 1, true, true},
    Case{"real-cuda-device-copy-64", "device-copy", 64, 1, true, true},
    Case{"real-cuda-device-copy-4096", "device-copy", 4096, 1, true, true},
    Case{"real-cuda-memset-64", "memset", 64, 1, true, true},
    Case{"real-cuda-memset-4096", "memset", 4096, 1, true, true},
    Case{"xdma-roundtrip-64", "xdma-roundtrip", 64, 1, false, true},
    Case{"xdma-roundtrip-4096", "xdma-roundtrip", 4096, 1, false, true},
    Case{"xdma-depth-1", "xdma-depth", 64, 1, false, true},
    Case{"xdma-depth-4", "xdma-depth", 64, 4, false, true},
    Case{"xdma-control-roundtrip", "xdma-control", 64, 1, false, true},
    Case{"xdma-event", "xdma-event", 64, 1, false, true},
    Case{"xdma-io-failure", "xdma-fault-io", 64, 1, false, false},
    Case{"xdma-loss", "xdma-fault-loss", 64, 1, false, false},
    Case{"xdma-cleanup-retry", "xdma-fault-cleanup", 64, 1, false, false},
    Case{"xdma-event-timeout", "xdma-fault-event", 64, 1, false, false},
    Case{"xdma-short-transfer", "xdma-fault-short", 64, 1, false, false},
    Case{"xdma-reset-recovery", "xdma-fault-reset", 64, 1, false, false},
    Case{"xdma-event-cancel", "xdma-fault-cancel", 64, 1, false, false},
    Case{"real-xdma-roundtrip-64", "xdma-roundtrip", 64, 1, true, true},
    Case{"real-xdma-roundtrip-4096", "xdma-roundtrip", 4096, 1, true, true},
};
struct Measures {
    std::uint64_t submissions{}, completions{}, polls{}, rejected{}, copied_bytes{},
        kernels{}, checked_elements{}, peak_outstanding{}, timeouts{}, failed{}, resets{}, cleanup_retries{},
        control_reads{}, control_writes{}, events{}, canceled{};
    std::array<std::uint64_t, 16> values() const noexcept {
        return {submissions, completions, polls, rejected, copied_bytes, kernels,
                checked_elements, peak_outstanding, timeouts, failed, resets, cleanup_retries,
                control_reads, control_writes, events, canceled};
    }
};
void require(bool condition) {
    if (!condition) throw std::runtime_error("device benchmark invariant");
}

struct CudaFixture {
    const Case& spec;
    // Fixed driver storage and buffers live on the heap with this fixture.
    detail::FakeCudaDriver fake;
    std::array<rt::CudaStream, 1> streams{};
    std::unique_ptr<rt::CudaDeviceBackend> backend;
    rtfw_device_backend_api api{};
    std::array<std::int32_t, 1024> values{};
    std::array<std::int32_t, 1024> destination{};
    std::uint64_t token{}, destination_token{}, kernel{}, sequence{};
    bool initialized{}, initialization_succeeded{};

    explicit CudaFixture(const Case& requested) : spec(requested) {}
    ~CudaFixture() { if (finish() != Status::ok) std::terminate(); }

    Status setup(const CudaSession* session) {
        rt::CudaBackendConfig config;
        config.context = session ? session->context : detail::FakeCudaDriver::context;
        streams[0] = session ? session->stream : 0x51u;
        config.streams = streams;
        config.queue_capacity = spec.depth;
        config.buffer_capacity = 2;
        config.kernel_capacity = 1;
        fake.complete_on_record.store(true);
        backend = std::make_unique<rt::CudaDeviceBackend>(
            session ? session->driver : fake.api(), config);
        api = backend->api();
        if (std::string_view(spec.operation) == "kernel") {
            require(backend->register_kernel(session ? session->increment_kernel :
                detail::FakeCudaDriver::add_one_function, kernel) == RTFW_DEVICE_STATUS_OK);
        }
        rtfw_device_init_config init{};
        init.struct_size = sizeof(init);
        init.abi_version = RTFW_DEVICE_ABI_VERSION;
        init.requested_in_flight = spec.depth;
        init.requested_registered_buffers = 2;
        // Failed initialization may still own partial resources. shutdown is
        // checked even on the failing path before the host can release them.
        initialized = true;
        initialization_succeeded = api.initialize(api.instance, &init) == RTFW_DEVICE_STATUS_OK;
        require(initialization_succeeded);
        rtfw_device_buffer_registration registration{};
        registration.struct_size = sizeof(registration);
        registration.flags = RTFW_DEVICE_BUFFER_HOST_READ | RTFW_DEVICE_BUFFER_HOST_WRITE |
            RTFW_DEVICE_BUFFER_DEVICE_READ | RTFW_DEVICE_BUFFER_DEVICE_WRITE;
        registration.data = values.data();
        registration.bytes = spec.bytes;
        std::memcpy(registration.name, "benchmark.cuda", sizeof("benchmark.cuda"));
        require(api.register_buffer(api.instance, &registration, &token) == RTFW_DEVICE_STATUS_OK);
        registration.data = destination.data();
        std::memcpy(registration.name, "benchmark.dest", sizeof("benchmark.dest"));
        require(api.register_buffer(api.instance, &registration, &destination_token) == RTFW_DEVICE_STATUS_OK);
        return Status::ok;
    }

    Status finish() noexcept {
        if (!initialized) return Status::ok;
        const auto result = api.shutdown(api.instance);
        // The serialized failed-initialize path can report invalid_state only
        // when neither initialized nor incomplete-shutdown ownership remains.
        if (result != RTFW_DEVICE_STATUS_OK &&
            !(result == RTFW_DEVICE_STATUS_INVALID_STATE && !initialization_succeeded))
            return Status::provider_error;
        initialized = false;
        return Status::ok;
    }

    rt::DeviceSubmission request(std::uint32_t opcode, std::uint32_t access = 0) {
        auto result = rt::make_device_submission();
        result.submission_id = ++sequence;
        result.timeout_ns = 5'000'000'000ULL;
        result.opcode = opcode;
        if (access != 0) {
            result.buffer_count = 1;
            result.buffers[0].buffer_token = token;
            result.buffers[0].access = access;
            result.buffers[0].bytes = spec.bytes;
        }
        return result;
    }

    void submit(const rt::DeviceSubmission& requested, Measures& m) {
        require(api.submit(api.instance, &requested) == RTFW_DEVICE_STATUS_OK);
        ++m.submissions;
        auto health = rt::make_device_health();
        require(api.get_health(api.instance, &health) == RTFW_DEVICE_STATUS_OK);
        m.peak_outstanding = std::max(m.peak_outstanding, health.outstanding);
    }

    rtfw_device_completion poll(Measures& m, bool must_complete = true) {
        rtfw_device_completion completion{};
        std::uint64_t count = 0;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        do {
            require(api.poll(api.instance, &completion, 1, &count) == RTFW_DEVICE_STATUS_OK);
            ++m.polls;
            if (count == 1) {
                ++m.completions;
                if (completion.status == RTFW_DEVICE_STATUS_TIMEOUT) ++m.timeouts;
                return completion;
            }
            require(count == 0);
            if (!must_complete) return completion;
            std::this_thread::yield();
        } while (std::chrono::steady_clock::now() < deadline);
        throw std::runtime_error("device completion deadline");
    }

    void transact(const rt::DeviceSubmission& requested, Measures& m) {
        submit(requested, m);
        const auto completion = poll(m);
        require(completion.submission_id == requested.submission_id &&
                completion.status == RTFW_DEVICE_STATUS_OK);
    }

    std::uint64_t invoke(std::uint64_t ordinal, Measures& m) {
        const std::string_view operation = spec.operation;
        if(operation=="invalid-inputs") {
            const auto before=values;
            auto invalid=request(rt::cuda_device_opcode_copy_host_to_device,RTFW_DEVICE_ACCESS_READ);
            invalid.buffers[0].bytes=spec.bytes+1;
            require(api.submit(api.instance,&invalid)==RTFW_DEVICE_STATUS_INVALID_ARGUMENT); ++m.rejected;
            invalid.buffers[0].bytes=spec.bytes; invalid.buffers[0].access=RTFW_DEVICE_ACCESS_WRITE;
            require(api.submit(api.instance,&invalid)==RTFW_DEVICE_STATUS_INVALID_ARGUMENT); ++m.rejected;
            require(api.unregister_buffer(api.instance,destination_token)==RTFW_DEVICE_STATUS_OK);
            invalid.buffers[0].access=RTFW_DEVICE_ACCESS_READ; invalid.buffers[0].buffer_token=destination_token;
            require(api.submit(api.instance,&invalid)==RTFW_DEVICE_STATUS_INVALID_ARGUMENT); ++m.rejected;
            require(values==before && fake.launches.load()==0 && fake.frees.load()==1);
        }
        if (operation.starts_with("fault-")) {
            auto requested = request(rt::cuda_device_opcode_noop);
            fake.complete_on_record.store(false);
            if (operation == "fault-submit") {
                fake.fail_next_event_record.store(true);
                require(api.submit(api.instance, &requested) == RTFW_DEVICE_STATUS_ERROR);
                ++m.failed;
                auto health = rt::make_device_health();
                require(api.get_health(api.instance, &health) == RTFW_DEVICE_STATUS_OK);
                require(health.state == RTFW_DEVICE_HEALTH_RESET_REQUIRED && health.outstanding == 1);
                m.peak_outstanding = health.outstanding;
                require(api.reset(api.instance) == RTFW_DEVICE_STATUS_OK);
                require(api.get_health(api.instance, &health) == RTFW_DEVICE_STATUS_OK);
                require(health.state == RTFW_DEVICE_HEALTH_HEALTHY && health.outstanding == 0 && health.resets == 1);
                m.resets = health.resets;
                fake.complete_on_record.store(true);
                transact(request(rt::cuda_device_opcode_noop), m);
            } else {
                submit(requested,m);
                if (operation == "fault-context") {
                    fake.lose_context_on_query.store(true);
                    const auto completion = poll(m);
                    require(completion.submission_id == requested.submission_id && completion.status == RTFW_DEVICE_STATUS_LOST);
                    ++m.failed;
                    require(api.reset(api.instance) == RTFW_DEVICE_STATUS_LOST);
                    auto health = rt::make_device_health();
                    require(api.get_health(api.instance, &health) == RTFW_DEVICE_STATUS_OK);
                    require(health.state == RTFW_DEVICE_HEALTH_LOST && health.outstanding == 1);
                } else {
                    fake.fail_next_event_sync.store(true);
                    require(finish() == Status::provider_error);
                    ++m.failed; ++m.cleanup_retries;
                    require(fake.frees.load() == 0 && fake.host_unregistrations.load() == 0);
                    require(api.submit(api.instance, &requested) == RTFW_DEVICE_STATUS_INVALID_STATE);
                    ++m.rejected;
                }
            }
            require(finish() == Status::ok);
            require(fake.allocations.load() == fake.frees.load() &&
                    fake.host_registrations.load() == fake.host_unregistrations.load());
            return m.failed + m.resets + m.cleanup_retries;
        }
        if (operation == "empty-poll") {
            (void)poll(m, false);
            require(m.completions == 0);
            return 0;
        }
        if (operation == "depth" || operation == "not-ready-poll" || operation == "timeout") {
            fake.complete_on_record.store(false);
            std::array<std::uint64_t, 4> ids{};
            for (std::size_t i = 0; i < spec.depth; ++i) {
                auto requested = request(rt::cuda_device_opcode_noop);
                if (operation == "timeout") requested.timeout_ns = 10;
                ids[i] = requested.submission_id;
                submit(requested, m);
            }
            require(m.peak_outstanding == spec.depth);
            if (operation == "depth") {
                const auto rejected = request(rt::cuda_device_opcode_noop);
                require(api.submit(api.instance, &rejected) == RTFW_DEVICE_STATUS_QUEUE_FULL);
                ++m.rejected;
            }
            if (operation == "timeout") fake.advance(11);
            (void)poll(m, false);
            require(m.completions == 0);
            fake.make_events_ready();
            std::array<bool, 4> seen{};
            for (std::size_t i = 0; i < spec.depth; ++i) {
                const auto completion = poll(m);
                auto found = std::find(ids.begin(), ids.begin() + static_cast<std::ptrdiff_t>(spec.depth),
                                       completion.submission_id);
                require(found != ids.begin() + static_cast<std::ptrdiff_t>(spec.depth));
                const auto index = static_cast<std::size_t>(found - ids.begin());
                require(!seen[index]); seen[index] = true;
                require(completion.status == (operation == "timeout" ?
                    RTFW_DEVICE_STATUS_TIMEOUT : RTFW_DEVICE_STATUS_OK));
            }
            auto health = rt::make_device_health();
            require(api.get_health(api.instance, &health) == RTFW_DEVICE_STATUS_OK && health.outstanding == 0);
            fake.complete_on_record.store(true);
            // Prove a fresh transaction can reuse the drained capacity.
            transact(request(rt::cuda_device_opcode_noop), m);
            return m.completions;
        }
        const auto count = spec.bytes / sizeof(std::int32_t);
        for (std::size_t i = 0; i < count; ++i)
            values[i] = static_cast<std::int32_t>(ordinal * 7 + i * 3);
        transact(request(rt::cuda_device_opcode_copy_host_to_device, RTFW_DEVICE_ACCESS_READ), m);
        m.copied_bytes += spec.bytes;
        if (operation == "kernel") {
            auto requested = request(rt::cuda_device_opcode_launch_kernel, RTFW_DEVICE_ACCESS_READ_WRITE);
            rt::CudaKernelLaunch launch{};
            launch.kernel_token = kernel;
            launch.block_x = 64;
            launch.grid_x = static_cast<std::uint32_t>((count + 63) / 64);
            require(rt::cuda_kernel_add_buffer_argument(launch, 0));
            require(rt::cuda_kernel_add_scalar_argument(launch, static_cast<std::uint32_t>(count)));
            rt::set_cuda_kernel_launch(requested, launch);
            transact(requested, m);
            ++m.kernels;
        }
        auto output_token = token;
        if (operation == "device-copy") {
            auto requested = request(rt::cuda_device_opcode_copy_device_to_device, RTFW_DEVICE_ACCESS_READ);
            requested.buffer_count = 2;
            requested.buffers[1] = requested.buffers[0];
            requested.buffers[1].buffer_token = destination_token;
            requested.buffers[1].access = RTFW_DEVICE_ACCESS_WRITE;
            transact(requested, m);
            m.copied_bytes += spec.bytes;
            output_token = destination_token;
        }
        if (operation == "memset") {
            auto requested = request(rt::cuda_device_opcode_memset_d8, RTFW_DEVICE_ACCESS_WRITE);
            requested.payload_size = 1;
            requested.payload[0] = 0x2a;
            transact(requested, m);
        }
        std::fill_n(values.begin(), count, -1);
        std::fill_n(destination.begin(), count, -1);
        auto download = request(rt::cuda_device_opcode_copy_device_to_host, RTFW_DEVICE_ACCESS_WRITE);
        download.buffers[0].buffer_token = output_token;
        transact(download, m);
        m.copied_bytes += spec.bytes;
        std::uint64_t checksum = 0;
        for (std::size_t i = 0; i < count; ++i) {
            const auto value = operation == "device-copy" ? destination[i] : values[i];
            const auto expected = operation == "memset" ? 0x2a2a2a2a :
                static_cast<std::int32_t>(ordinal * 7 + i * 3 + (operation == "kernel" ? 1 : 0));
            require(value == expected);
            ++m.checked_elements;
            checksum += static_cast<std::uint64_t>(value);
        }
        return checksum;
    }
};
#include "device_cases/cuda_graph.inc"

struct XdmaFixture {
    const Case& spec;
    detail::FakeXdmaDriver fake;
    struct alignas(4096) Buffer { std::array<std::byte,4096> bytes{}; };
    std::unique_ptr<Buffer> storage = std::make_unique<Buffer>();
    std::unique_ptr<rt::XdmaDeviceBackend> backend;
    rtfw_device_backend_api api{};
    rt::HalV2BackendRegistration registration{};
    std::uint64_t token{}, sequence{}, offset{};
    bool native{}, initialized{}, initialization_succeeded{};
    explicit XdmaFixture(const Case& c) : spec(c) {}
    ~XdmaFixture() { if (finish()!=Status::ok) std::terminate(); }

    Status setup(const XdmaSession* session) {
        const std::string_view op=spec.operation;
        native=op=="xdma-control" || op=="xdma-event" || op=="xdma-fault-event" || op=="xdma-fault-cancel";
        rt::XdmaBackendConfig config;
        config.queue_capacity=spec.depth; config.buffer_capacity=1; config.worker_count=1;
        config.max_transfer_bytes=4096; config.max_buffer_bytes=4096;
        config.control_aperture_bytes=256; config.user_event_count=1;
        if(session) {
            require(session->confirmed_window_bytes>=spec.bytes &&
                    session->device_offset<=UINT64_MAX-spec.bytes);
            config=session->config; offset=session->device_offset;
            require(config.queue_capacity>=spec.depth && config.queue_capacity<=4 &&
                    config.buffer_capacity>=1 && config.buffer_capacity<=4 && config.max_buffer_bytes<=4*4096 &&
                    config.worker_count>=1 && config.worker_count<=2 && config.max_transfer_bytes>=spec.bytes && config.max_transfer_bytes<=4096);
        }
        backend=std::make_unique<rt::XdmaDeviceBackend>(session ? session->driver : fake.api_v2(),config);
        initialized=true;
        if(native) {
            registration=backend->hal_v2_registration("benchmark.xdma");
            rt::HalV2InitializeConfig init{};
            init.requested_in_flight=spec.depth; init.requested_registered_buffers=1;
            initialization_succeeded=registration.api.initialize(registration.api.instance,&init)==rt::HalV2Status::ok;
            require(initialization_succeeded);
            rt::HalV2BufferRegistration buffer{};
            buffer.flags=RTFW_DEVICE_BUFFER_HOST_READ|RTFW_DEVICE_BUFFER_HOST_WRITE|
                RTFW_DEVICE_BUFFER_DEVICE_READ|RTFW_DEVICE_BUFFER_DEVICE_WRITE;
            buffer.data=storage->bytes.data(); buffer.bytes=spec.bytes;
            std::memcpy(buffer.name.data(),"benchmark.xdma",sizeof("benchmark.xdma"));
            require(registration.api.register_buffer(registration.api.instance,&buffer,&token)==rt::HalV2Status::ok);
        } else {
            api=backend->api();
            rtfw_device_init_config init{}; init.struct_size=sizeof(init); init.abi_version=RTFW_DEVICE_ABI_VERSION;
            init.requested_in_flight=spec.depth; init.requested_registered_buffers=1;
            initialization_succeeded=api.initialize(api.instance,&init)==RTFW_DEVICE_STATUS_OK;
            require(initialization_succeeded);
            rtfw_device_buffer_registration buffer{}; buffer.struct_size=sizeof(buffer);
            buffer.flags=RTFW_DEVICE_BUFFER_HOST_READ|RTFW_DEVICE_BUFFER_HOST_WRITE|
                RTFW_DEVICE_BUFFER_DEVICE_READ|RTFW_DEVICE_BUFFER_DEVICE_WRITE;
            buffer.data=storage->bytes.data(); buffer.bytes=spec.bytes;
            std::memcpy(buffer.name,"benchmark.xdma",sizeof("benchmark.xdma"));
            require(api.register_buffer(api.instance,&buffer,&token)==RTFW_DEVICE_STATUS_OK);
        }
        return Status::ok;
    }
    Status finish() noexcept {
        if(!initialized) return Status::ok;
        fake.blocked.store(false,std::memory_order_release); fake.blocked.notify_all();
        const auto status=native ? static_cast<std::int32_t>(registration.api.shutdown(registration.api.instance)) :
            api.shutdown(api.instance);
        if(status!=RTFW_DEVICE_STATUS_OK && !(status==RTFW_DEVICE_STATUS_INVALID_STATE && !initialization_succeeded))
            return Status::provider_error;
        initialized=false;
        return Status::ok;
    }
    rt::DeviceSubmission request(rt::XdmaDirection direction) {
        auto r=rt::make_device_submission(); r.submission_id=++sequence; r.timeout_ns=5'000'000'000ULL;
        rt::XdmaTransfer transfer{}; transfer.device_offset=offset;
        rt::set_xdma_transfer(r,direction,transfer); r.buffer_count=1;
        r.buffers[0].buffer_token=token; r.buffers[0].bytes=spec.bytes;
        r.buffers[0].access=direction==rt::XdmaDirection::host_to_card ? RTFW_DEVICE_ACCESS_READ : RTFW_DEVICE_ACCESS_WRITE;
        return r;
    }
    void submit(const rt::DeviceSubmission& r,Measures& m) {
        require(api.submit(api.instance,&r)==RTFW_DEVICE_STATUS_OK); ++m.submissions;
        auto health=rt::make_device_health();
        require(api.get_health(api.instance,&health)==RTFW_DEVICE_STATUS_OK);
        m.peak_outstanding=std::max(m.peak_outstanding,health.outstanding);
    }
    rtfw_device_completion poll(Measures& m) {
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
        do {
            rtfw_device_completion result{}; std::uint64_t count=0;
            require(api.poll(api.instance,&result,1,&count)==RTFW_DEVICE_STATUS_OK); ++m.polls;
            if(count==1) { ++m.completions; return result; }
            require(count==0); std::this_thread::yield();
        } while(std::chrono::steady_clock::now()<deadline);
        throw std::runtime_error("XDMA completion deadline");
    }
    void transfer(rt::XdmaDirection direction,Measures& m) {
        const auto r=request(direction); submit(r,m); const auto completion=poll(m);
        require(completion.submission_id==r.submission_id && completion.status==RTFW_DEVICE_STATUS_OK && completion.value==spec.bytes);
        m.copied_bytes+=completion.value;
    }
    std::uint64_t invoke(std::uint64_t ordinal,Measures& m) {
        const std::string_view op=spec.operation;
        for(std::size_t i=0;i<spec.bytes;++i) storage->bytes[i]=static_cast<std::byte>((ordinal*7+i*3)%251);
        if(native) {
            const auto reads_before=fake.control_reads.load(),writes_before=fake.control_writes.load(),events_before=fake.event_waits.load();
            const auto expected=static_cast<std::uint32_t>(0x12340000u+ordinal);
            const auto before=storage->bytes;
            const bool unsuccessful=op=="xdma-fault-event" || op=="xdma-fault-cancel";
            rt::DeviceCommandBatch batch{}; batch.batch_id=++sequence; batch.timeout_ns=5'000'000'000ULL;
            batch.signal_count=1; batch.signals[0].timeline_handle=1; batch.signals[0].value=sequence;
            rt::HalV2BufferReference output{}; output.buffer_token=token; output.access=RTFW_DEVICE_ACCESS_WRITE; output.bytes=4;
            if(op=="xdma-control") {
                batch.command_count=2;
                require(rt::set_xdma_control_write(batch.commands[0],12,expected));
                require(rt::set_xdma_control_read(batch.commands[1],12,output));
            } else {
                fake.event_ready.store(!unsuccessful); fake.event_value.store(expected);
                fake.blocked.store(op=="xdma-fault-cancel");
                batch.command_count=1; require(rt::set_xdma_user_event_wait(batch.commands[0],0,output));
            }
            auto& command=*registration.command_timeline;
            require(command.submit(command.instance,&batch)==rt::HalV2Status::ok); ++m.submissions;
            rt::HalV2Health health{};
            require(registration.api.get_health(registration.api.instance,&health)==rt::HalV2Status::ok);
            m.peak_outstanding=health.outstanding;
            if(op=="xdma-fault-cancel") {
                const auto stop_deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
                while(!fake.event_waiting.load(std::memory_order_acquire)) {
                    require(std::chrono::steady_clock::now()<stop_deadline);
                    std::this_thread::yield();
                }
                require(command.request_stop(command.instance)==rt::HalV2Status::ok);
                require(command.request_stop(command.instance)==rt::HalV2Status::ok);
                require(fake.stop_requests.load()>=1);
            }
            rt::HalV2BatchCompletion completion{}; std::uint64_t count=0;
            const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
            do {
                require(command.poll(command.instance,&completion,1,&count)==rt::HalV2Status::ok); ++m.polls;
                if(count==1) break;
                require(count==0); std::this_thread::yield();
            } while(std::chrono::steady_clock::now()<deadline);
            require(count==1 && completion.batch_id==batch.batch_id); ++m.completions;
            if(unsuccessful) {
                require(completion.status==static_cast<std::int32_t>(op=="xdma-fault-cancel" ?
                    rt::HalV2Status::canceled : rt::HalV2Status::timeout));
                require(storage->bytes==before);
                ++m.failed;
                if(op=="xdma-fault-cancel") ++m.canceled; else ++m.timeouts;
            } else {
                require(completion.status==static_cast<std::int32_t>(rt::HalV2Status::ok) &&
                        completion.signal_count==1 && completion.signals[0].value==batch.signals[0].value);
                std::uint32_t observed=0; std::memcpy(&observed,storage->bytes.data(),sizeof(observed));
                require(observed==expected); ++m.checked_elements;
            }
            m.control_reads=fake.control_reads.load()-reads_before;
            m.control_writes=fake.control_writes.load()-writes_before;
            m.events=fake.event_waits.load()-events_before;
            return unsuccessful ? 0 : expected;
        }
        if(op=="xdma-invalid-inputs") {
            const auto before=storage->bytes;
            auto invalid=request(rt::XdmaDirection::host_to_card);
            rt::XdmaTransfer transfer{}; transfer.channel=1;
            rt::set_xdma_transfer(invalid,rt::XdmaDirection::host_to_card,transfer);
            require(api.submit(api.instance,&invalid)==RTFW_DEVICE_STATUS_INVALID_ARGUMENT); ++m.rejected;
            invalid=request(rt::XdmaDirection::host_to_card); transfer.channel=0; transfer.device_offset=UINT64_MAX;
            rt::set_xdma_transfer(invalid,rt::XdmaDirection::host_to_card,transfer);
            require(api.submit(api.instance,&invalid)==RTFW_DEVICE_STATUS_INVALID_ARGUMENT); ++m.rejected;
            invalid=request(rt::XdmaDirection::host_to_card); invalid.buffers[0].bytes=spec.bytes+1;
            require(api.submit(api.instance,&invalid)==RTFW_DEVICE_STATUS_INVALID_ARGUMENT); ++m.rejected;
            invalid=request(rt::XdmaDirection::host_to_card); invalid.buffers[0].access=RTFW_DEVICE_ACCESS_WRITE;
            require(api.submit(api.instance,&invalid)==RTFW_DEVICE_STATUS_INVALID_ARGUMENT); ++m.rejected;
            require(storage->bytes==before && fake.transfers.load()==0);
        }
        if(op=="xdma-depth") {
            fake.blocked.store(true);
            std::array<std::uint64_t,4> ids{};
            for(std::size_t i=0;i<spec.depth;++i) { const auto r=request(rt::XdmaDirection::host_to_card);ids[i]=r.submission_id;submit(r,m); }
            require(m.peak_outstanding==spec.depth);
            const auto extra=request(rt::XdmaDirection::host_to_card);
            require(api.submit(api.instance,&extra)==RTFW_DEVICE_STATUS_QUEUE_FULL); ++m.rejected;
            fake.blocked.store(false,std::memory_order_release);fake.blocked.notify_all();
            std::array<bool,4> seen{};
            for(std::size_t i=0;i<spec.depth;++i) {
                const auto completion=poll(m);
                const auto end=ids.begin()+static_cast<std::ptrdiff_t>(spec.depth);
                const auto found=std::find(ids.begin(),end,completion.submission_id);
                require(found!=end && completion.status==RTFW_DEVICE_STATUS_OK && completion.value==spec.bytes);
                const auto index=static_cast<std::size_t>(found-ids.begin());require(!seen[index]);seen[index]=true;
                m.copied_bytes+=completion.value;
            }
            transfer(rt::XdmaDirection::host_to_card,m);
            return m.completions;
        }
        if(op=="xdma-fault-io" || op=="xdma-fault-loss" || op=="xdma-fault-short" || op=="xdma-fault-reset") {
            const bool short_transfer=op=="xdma-fault-short", reset=op=="xdma-fault-reset";
            if(short_transfer) fake.short_transfer_once.store(true);
            else fake.next_result.store(static_cast<std::int32_t>(reset ? rt::XdmaDriverResult::reset_required :
                op=="xdma-fault-io" ? rt::XdmaDriverResult::io_error : rt::XdmaDriverResult::device_lost));
            const auto r=request(rt::XdmaDirection::host_to_card);submit(r,m); const auto completion=poll(m);
            const auto expected_status=reset ? RTFW_DEVICE_STATUS_RESET_REQUIRED :
                op=="xdma-fault-loss" ? RTFW_DEVICE_STATUS_LOST : RTFW_DEVICE_STATUS_ERROR;
            require(completion.submission_id==r.submission_id && completion.status==expected_status);
            require(fake.transfers.load()==(short_transfer ? 1u : 0u));
            if(short_transfer) {
                require(std::equal(storage->bytes.begin(),storage->bytes.begin()+static_cast<std::ptrdiff_t>(spec.bytes-1),fake.device.begin()));
                require(fake.device[spec.bytes-1]==std::byte{0});
            }
            ++m.failed;
            if(!reset) return 1;
            auto health=rt::make_device_health();
            require(api.get_health(api.instance,&health)==RTFW_DEVICE_STATUS_OK && health.state==RTFW_DEVICE_HEALTH_RESET_REQUIRED);
            require(api.reset(api.instance)==RTFW_DEVICE_STATUS_OK); ++m.resets;
            require(fake.resets.load()==1);
            // A complete round trip below proves that reset restores useful work.
        }
        transfer(rt::XdmaDirection::host_to_card,m);
        std::fill_n(storage->bytes.begin(),spec.bytes,std::byte{0xff});
        transfer(rt::XdmaDirection::card_to_host,m);
        std::uint64_t checksum=0;
        for(std::size_t i=0;i<spec.bytes;++i) {
            const auto value=static_cast<unsigned>(storage->bytes[i]);
            require(value==(ordinal*7+i*3)%251);checksum+=value;++m.checked_elements;
        }
        if(op=="xdma-invalid-inputs") {
            require(api.unregister_buffer(api.instance,token)==RTFW_DEVICE_STATUS_OK);
            auto stale=request(rt::XdmaDirection::host_to_card);
            require(api.submit(api.instance,&stale)==RTFW_DEVICE_STATUS_INVALID_ARGUMENT); ++m.rejected;
            require(fake.transfers.load()==2);
        }
        if(op=="xdma-fault-cleanup") {
            fake.fail_shutdown_once.store(true);
            require(finish()==Status::provider_error);++m.cleanup_retries;++m.failed;
            require(fake.initialized.load()); require(finish()==Status::ok);require(!fake.initialized.load());
        }
        return checksum;
    }
};
#include "device_cases/pipeline.inc"
#include "device_cases/hal.inc"
} // namespace

std::span<const Case> cases() noexcept { return catalog; }
struct StagingFixture {
    std::array<std::byte,4096> source{},destination{};
    std::uint64_t invoke(const Case& c,std::uint64_t ordinal,Measures& m) {
        for(std::size_t i=0;i<c.bytes;++i) source[i]=static_cast<std::byte>((ordinal*7+i*3)%251);
        std::fill(destination.begin(),destination.end(),std::byte{0xff});
        std::memcpy(destination.data(),source.data(),c.bytes);
        std::uint64_t checksum=0;
        for(std::size_t i=0;i<c.bytes;++i) {
            const auto value=static_cast<unsigned>(destination[i]);
            require(value==(ordinal*7+i*3)%251); checksum+=value; ++m.checked_elements;
        }
        require(std::all_of(destination.begin()+static_cast<std::ptrdiff_t>(c.bytes),destination.end(),[](std::byte v){return v==std::byte{0xff};}));
        m.copied_bytes=c.bytes; return checksum;
    }
};
struct Provider::State {
    const CudaSession* session{};
    const XdmaSession* xdma_session{};
    const Case* selected{};
    std::unique_ptr<CudaFixture> fixture;
    std::unique_ptr<GraphFixture> graph;
    std::unique_ptr<PipelineFixture> pipeline;
    std::unique_ptr<StagingFixture> staging;
    std::unique_ptr<HalFixture> hal;
    std::unique_ptr<XdmaFixture> xdma;
    std::uint64_t ordinal{};
    bool unavailable{}, failed{};
};
Provider::Provider(const CudaSession* session, const XdmaSession* xdma) : state_(std::make_unique<State>()) {
    state_->session = session; state_->xdma_session=xdma;
}
Provider::~Provider() { if (finish() != Status::ok) std::terminate(); }
ProviderV1 Provider::table() noexcept {
    ProviderV1 result;
    result.id = "rtfw.device"; result.case_count = catalog.size(); result.user = this;
    result.describe = describe; result.invoke = invoke;
    return result;
}
Status Provider::describe(void*, std::size_t index, Descriptor& d) {
    if (index >= catalog.size()) return Status::not_found;
    const auto& c = catalog[index];
    d = {};
    d.case_id = c.id; d.subsystem = std::string_view(c.operation).starts_with("xdma-") ? "xdma" : "cuda";
    if(std::string_view(c.operation).starts_with("pipeline-")) d.subsystem="pipeline";
    if(std::string_view(c.operation)=="host-staging") d.subsystem="host-staging";
    if(std::string_view(c.operation).starts_with("hal-")) d.subsystem="hal";
    d.implementation = d.subsystem+(c.real ? "-supplied-session-v1" : "-fake-protocol-v1");
    d.configuration = "bytes-" + std::to_string(c.bytes) + "-depth-" + std::to_string(c.depth);
    if(d.subsystem=="pipeline") d.configuration="bytes-"+std::to_string(c.bytes)+"-frames-"+
        std::to_string(c.depth)+"-workers-"+std::to_string(c.depth)+"-queue-4";
    d.workload_kind = c.allocation_free ? "complete-transaction-with-output-validation" : "lifecycle-fault-recovery-with-cleanup";
    if(d.subsystem=="hal") d.workload_kind="public-capability-and-topology-inspection";
    d.workload_sha256 = sha256(std::string(c.id) + ";" + d.configuration + ";" + ((d.subsystem=="xdma" || d.subsystem=="host-staging") ? "byte-pattern-v1" : "integer-increment-v1"));
    d.parameters = {{"bytes",c.bytes,c.bytes,c.bytes},{"depth",c.depth,c.depth,c.depth}};
    if(d.subsystem=="hal") d.parameters={{"depth",c.depth,c.depth,c.depth}};
    if(d.subsystem=="pipeline") {
        d.parameters[1]={"queue_depth",4,4,4};
        d.parameters.push_back({"frames",c.depth,c.depth,c.depth});
        d.parameters.push_back({"cpu_workers",c.depth,c.depth,c.depth});
    }
    for (const char* name : {"submissions","completions","polls","rejected","copied_bytes",
                            "kernels","checked_elements","peak_outstanding","timeouts","failed","resets","cleanup_retries","control_reads","control_writes","events","canceled"})
        d.counters.push_back({name, std::string_view(name)=="copied_bytes" ? "bytes" : "count", 0, max_integer});
    return Status::ok;
}
Status Provider::prepare(std::string_view id) {
    if (state_->selected) return Status::busy;
    auto it = std::find_if(catalog.begin(), catalog.end(), [id](const Case& c) { return id == c.id; });
    if (it == catalog.end()) return Status::not_found;
    state_->selected = &*it; state_->ordinal = 0;
    if(std::string_view(it->operation).starts_with("hal-")) {
        try { state_->hal=std::make_unique<HalFixture>(*it); return Status::ok; }
        catch(...) { state_->failed=true; return Status::provider_error; }
    }
    if(std::string_view(it->operation)=="host-staging") {
        try { state_->staging=std::make_unique<StagingFixture>(); return Status::ok; }
        catch(...) { state_->failed=true; return Status::provider_error; }
    }
    if(std::string_view(it->operation).starts_with("pipeline-")) {
        const auto* cs=it->real ? state_->session : nullptr;
        const auto* xs=it->real ? state_->xdma_session : nullptr;
        const bool graph=std::string_view(it->operation)=="pipeline-graph";
        if(it->real && (!cs || !xs || !xs->confirmed_window_bytes || !cs->context || !cs->stream ||
            (graph ? !cs->increment_graph || !cs->graph_buffer : !cs->increment_kernel))) {
            state_->unavailable=true; return Status::not_run;
        }
        try {
            if(!it->allocation_free) return Status::ok;
            state_->pipeline=std::make_unique<PipelineFixture>(*it);
            return state_->pipeline->setup(cs,xs);
        } catch(...) { state_->failed=true; return Status::provider_error; }
    }
    if(std::string_view(it->operation).starts_with("xdma-")) {
        const auto* session=it->real ? state_->xdma_session : nullptr;
        if(it->real && (!session || !session->confirmed_window_bytes)) { state_->unavailable=true;return Status::not_run; }
        try {
            if(!it->allocation_free) return Status::ok;
            state_->xdma=std::make_unique<XdmaFixture>(*it);
            return state_->xdma->setup(session);
        } catch(...) { state_->failed=true; return Status::provider_error; }
    }
    const auto* session = it->real ? state_->session : nullptr;
    if (it->real && (!session || !session->context || !session->stream ||
        (std::string_view(it->operation)=="kernel" && !session->increment_kernel) ||
        (std::string_view(it->operation)=="graph" && (!session->increment_graph || !session->graph_buffer)))) {
        state_->unavailable = true;
        return Status::not_run;
    }
    try {
        if (!it->allocation_free) return Status::ok;
        if(std::string_view(it->operation)=="graph") {
            state_->graph=std::make_unique<GraphFixture>(*it);
            return state_->graph->setup(session);
        }
        state_->fixture = std::make_unique<CudaFixture>(*it);
        return state_->fixture->setup(session);
    } catch (...) { state_->failed=true; return Status::provider_error; }
}
Status Provider::finish() noexcept {
    state_->staging.reset(); state_->hal.reset();
    if(state_->pipeline && state_->pipeline->finish()!=Status::ok) return Status::provider_error;
    state_->pipeline.reset();
    if(state_->graph && state_->graph->finish()!=Status::ok) return Status::provider_error;
    state_->graph.reset();
    if (state_->fixture && state_->fixture->finish()!=Status::ok) return Status::provider_error;
    if (state_->xdma && state_->xdma->finish()!=Status::ok) return Status::provider_error;
    state_->xdma.reset();
    state_->fixture.reset(); state_->selected=nullptr; state_->unavailable=false; state_->failed=false; state_->ordinal=0;
    return Status::ok;
}
Status Provider::invoke(void* user, std::string_view id, std::uint64_t ordinal, Observation& out) {
    auto& self = *static_cast<Provider*>(user);
    auto& s = *self.state_;
    if (s.failed || !s.selected || id!=s.selected->id || ordinal!=s.ordinal) return Status::provider_error;
    if (s.unavailable) return Status::not_run;
    try {
        Measures measures;
        if(s.hal) out.checksum=s.hal->invoke(measures);
        else if(s.staging) out.checksum=s.staging->invoke(*s.selected,ordinal,measures);
        else if(std::string_view(s.selected->operation).starts_with("pipeline-")) {
            if(s.pipeline) out.checksum=s.pipeline->invoke(ordinal,measures);
            else {
                auto fixture=std::make_unique<PipelineFixture>(*s.selected);
                require(fixture->setup(nullptr,nullptr)==Status::ok);
                out.checksum=fixture->invoke(ordinal,measures);
                require(fixture->finish()==Status::ok);
            }
        }
        else if(std::string_view(s.selected->operation).starts_with("xdma-")) {
            if(s.selected->allocation_free) out.checksum=s.xdma->invoke(ordinal,measures);
            else {
                auto fixture=std::make_unique<XdmaFixture>(*s.selected);
                require(fixture->setup(nullptr)==Status::ok);
                out.checksum=fixture->invoke(ordinal,measures);
                require(fixture->finish()==Status::ok);
            }
        }
        else if(s.graph) out.checksum=s.graph->invoke(ordinal,measures);
        else if (s.selected->allocation_free) out.checksum = s.fixture->invoke(ordinal, measures);
        else {
            auto fixture = std::make_unique<CudaFixture>(*s.selected);
            require(fixture->setup(nullptr) == Status::ok);
            out.checksum = fixture->invoke(ordinal, measures);
            require(fixture->finish() == Status::ok);
        }
        const auto values = measures.values(); out.counters.assign(values.begin(), values.end());
        out.correct = true; ++s.ordinal;
        return Status::ok;
    } catch (...) { s.failed=true; out.correct=false; return Status::invariant_failed; }
}
} // namespace rtfw::benchmark::device
