#include "device_provider.hpp"
#include "device_cases/fake_cuda.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <thread>

namespace rtfw::benchmark::device {
namespace {
constexpr std::array catalog{
    Case{"cuda-roundtrip-64", "roundtrip", 64, 1, false, true},
    Case{"cuda-roundtrip-4096", "roundtrip", 4096, 1, false, true},
    Case{"cuda-kernel-64", "kernel", 64, 1, false, true},
    Case{"cuda-kernel-4096", "kernel", 4096, 1, false, true},
    Case{"cuda-device-copy-64", "device-copy", 64, 1, false, true},
    Case{"cuda-device-copy-4096", "device-copy", 4096, 1, false, true},
    Case{"cuda-memset-64", "memset", 64, 1, false, true},
    Case{"cuda-memset-4096", "memset", 4096, 1, false, true},
    Case{"cuda-depth-1", "depth", 64, 1, false, true},
    Case{"cuda-depth-4", "depth", 64, 4, false, true},
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
};
struct Measures {
    std::uint64_t submissions{}, completions{}, polls{}, rejected{}, copied_bytes{},
        kernels{}, checked_elements{}, peak_outstanding{}, timeouts{}, failed{}, resets{}, cleanup_retries{};
    std::array<std::uint64_t, 12> values() const noexcept {
        return {submissions, completions, polls, rejected, copied_bytes, kernels,
                checked_elements, peak_outstanding, timeouts, failed, resets, cleanup_retries};
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
} // namespace

std::span<const Case> cases() noexcept { return catalog; }
struct Provider::State {
    const CudaSession* session{};
    const Case* selected{};
    std::unique_ptr<CudaFixture> fixture;
    std::uint64_t ordinal{};
    bool unavailable{};
};
Provider::Provider(const CudaSession* session) : state_(std::make_unique<State>()) { state_->session = session; }
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
    d.case_id = c.id; d.subsystem = "cuda";
    d.implementation = c.real ? "cuda-supplied-session-v1" : "cuda-fake-protocol-v1";
    d.configuration = "bytes-" + std::to_string(c.bytes) + "-depth-" + std::to_string(c.depth);
    d.workload_kind = c.allocation_free ? "complete-transaction-with-output-validation" : "lifecycle-fault-recovery-with-cleanup";
    d.workload_sha256 = sha256(std::string(c.id) + ";" + d.configuration + ";integer-increment-v1");
    d.parameters = {{"bytes",c.bytes,c.bytes,c.bytes},{"depth",c.depth,c.depth,c.depth}};
    for (const char* name : {"submissions","completions","polls","rejected","copied_bytes",
                            "kernels","checked_elements","peak_outstanding","timeouts","failed","resets","cleanup_retries"})
        d.counters.push_back({name, std::string_view(name)=="copied_bytes" ? "bytes" : "count", 0, max_integer});
    return Status::ok;
}
Status Provider::prepare(std::string_view id) {
    if (state_->selected) return Status::busy;
    auto it = std::find_if(catalog.begin(), catalog.end(), [id](const Case& c) { return id == c.id; });
    if (it == catalog.end()) return Status::not_found;
    state_->selected = &*it; state_->ordinal = 0;
    const auto* session = it->real ? state_->session : nullptr;
    if (it->real && (!session || !session->context || !session->stream ||
        (std::string_view(it->operation)=="kernel" && !session->increment_kernel))) {
        state_->unavailable = true;
        return Status::not_run;
    }
    try {
        if (!it->allocation_free) return Status::ok;
        state_->fixture = std::make_unique<CudaFixture>(*it);
        return state_->fixture->setup(session);
    } catch (...) { return Status::provider_error; }
}
Status Provider::finish() noexcept {
    if (state_->fixture && state_->fixture->finish()!=Status::ok) return Status::provider_error;
    state_->fixture.reset(); state_->selected=nullptr; state_->unavailable=false; state_->ordinal=0;
    return Status::ok;
}
Status Provider::invoke(void* user, std::string_view id, std::uint64_t ordinal, Observation& out) {
    auto& self = *static_cast<Provider*>(user);
    auto& s = *self.state_;
    if (!s.selected || id!=s.selected->id || ordinal!=s.ordinal) return Status::provider_error;
    if (s.unavailable) return Status::not_run;
    try {
        Measures measures;
        if (s.selected->allocation_free) out.checksum = s.fixture->invoke(ordinal, measures);
        else {
            auto fixture = std::make_unique<CudaFixture>(*s.selected);
            require(fixture->setup(nullptr) == Status::ok);
            out.checksum = fixture->invoke(ordinal, measures);
            require(fixture->finish() == Status::ok);
        }
        const auto values = measures.values(); out.counters.assign(values.begin(), values.end());
        out.correct = true; ++s.ordinal;
        return Status::ok;
    } catch (...) { return Status::invariant_failed; }
}
} // namespace rtfw::benchmark::device
