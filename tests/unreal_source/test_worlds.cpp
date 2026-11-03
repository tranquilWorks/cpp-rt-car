#include <rtfw_unreal/worlds.hpp>
#include <rtfw_unreal/sample.hpp>
#include <library.hpp>
#include "../cuda_physics/allocation_guard.hpp"
#include "../../samples/host_adapter/jobs.hpp"
#include "../../samples/host_adapter/memory.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>

static unsigned checks = 0;
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr, "worlds check %d: %s\n", __LINE__, #x); std::abort(); } } while (false)
namespace {
struct Clock final : rt::RuntimeClock {
    std::uint64_t now_ns() noexcept override { return 1000000; }
};
struct Bindings {
    inline static unsigned created = 0, destroyed = 0;
    inline static int hold_retirement = -1, fail_attach = -1;
    inline static bool parallel_workers = false;
    unsigned identity = created++;
    Clock source;
    host_example::Jobs jobs;
    host_example::Memory memory;
    bool valid_ = false;
    explicit Bindings(const rt::RuntimeConfig&) noexcept { valid_ = jobs.start(parallel_workers) == rt::Status::ok; }
    ~Bindings() { CHECK(memory.live_count() == 0); CHECK(jobs.pending.load() == 0); ++destroyed; }
    bool valid() const noexcept { return valid_; }
    rt::RuntimeClock& clock() noexcept { return source; }
    rt::Status attach(rt::Runtime& runtime, const rt::RuntimeConfig&) noexcept {
        if (static_cast<int>(identity) == fail_attach) return rt::Status::internal_error;
        auto s = runtime.set_host_executor(jobs.adapter());
        if (s == rt::Status::ok) s = runtime.set_memory_provider(memory.table());
        if (s == rt::Status::ok) s = runtime.set_cpu_memory_policy(rtfw_unreal::memory_policy());
        return s;
    }
    void close_admission() noexcept {} // Serialized cooperative fixture has no external submitters.
    bool quiescent() noexcept {
        return static_cast<int>(identity) != hold_retirement && jobs.pending.load() == 0 && memory.live_count() == 0;
    }
};
using Host = rtfw_unreal::WorldsHost<Bindings, 2, 2, 2>;
rt::RuntimeConfig config() {
    auto c = rtfw_unreal::configuration(); c.worker_count = host_example::Jobs::workers; return c;
}
void start(Host& host, void* owner, rtfw_unreal::Sample& model, rtfw_unreal::WorldHandle& world) {
    CHECK(host.create(owner, config(), world) == rt::Status::ok);
    CHECK(host.configure(world, rtfw_unreal::Sample::configure, &model) == rt::Status::ok);
    CHECK(host.finalize(world) == rt::Status::ok);
    CHECK(host.start(world) == rt::Status::ok);
}
rt::Status configure_fail(void*, rt::Runtime&) noexcept { return rt::Status::callback_failed; }
Host* reentrant = nullptr;
rtfw_unreal::WorldHandle nested;
rt::Status configure_reentry(void*, rt::Runtime&) noexcept {
    int owner;
    CHECK(reentrant->create(&owner, config(), nested) == rt::Status::invalid_state);
    CHECK(reentrant->close_all() == rt::Status::invalid_state);
    return rt::Status::ok;
}
void lifecycle() {
    Host host;
    // Each fixture binding embeds three host arenas. Keep the second host off
    // the stack so this negative control also fits the native Windows stack.
    auto foreign = std::make_unique<Host>();
    rtfw_unreal::Sample a, b;
    int first, second, third;
    rtfw_unreal::WorldHandle one, two, output{99,88,77};
    start(host, &first, a, one); start(host, &second, b, two);
    CHECK(host.create(&third, config(), output) == rt::Status::resource_exhausted);
    CHECK(output.registry == 99 && output.generation == 88);
    CHECK(host.create(&first, config(), output) == rt::Status::invalid_state);
    CHECK(foreign->close_world(one) == rt::Status::invalid_handle);
    {
        rtfw_physics_allocation::begin();
        for (std::uint64_t frame = 0; frame < 128; ++frame) {
            CHECK(host.step(one, {frame, std::chrono::milliseconds(1)}) == rt::Status::ok);
            if ((frame & 1u) == 0) CHECK(host.step(two, {frame / 2, std::chrono::milliseconds(2)}) == rt::Status::ok);
        }
        CHECK(rtfw_physics_allocation::end() == 0);
    }
    CHECK(a.frames == 128 && b.frames == 64 && a.matches() && b.matches());
    a.fail = true;
    CHECK(host.step(one, {128, std::chrono::milliseconds(1)}) == rt::Status::callback_failed);
    CHECK(host.close_world(one) == rt::Status::ok);
    CHECK(host.step(one, {}) == rt::Status::invalid_handle);
    CHECK(host.close_all() == rt::Status::ok && host.live_worlds() == 0);
    CHECK(host.create(&first, config(), output) == rt::Status::ok);
    CHECK(!rtfw_unreal::same(one, output));
    CHECK(host.configure(output, configure_fail, nullptr) == rt::Status::callback_failed);
    reentrant = &host;
    CHECK(host.configure(output, configure_reentry, nullptr) == rt::Status::ok);
    CHECK(host.close_all() == rt::Status::ok);
}
void retry_and_failed_setup() {
    Host host;
    int first, second;
    rtfw_unreal::Sample a, b;
    rtfw_unreal::WorldHandle one, two;
    Bindings::hold_retirement = static_cast<int>(Bindings::created);
    start(host, &first, a, one); start(host, &second, b, two);
    const auto destroyed = Bindings::destroyed;
    CHECK(host.close_all() == rt::Status::invalid_state);
    CHECK(host.live_worlds() == 1 && Bindings::destroyed == destroyed + 1);
    CHECK(host.step(one, {}) == rt::Status::invalid_state);
    CHECK(host.close_world(two) == rt::Status::invalid_handle);
    Bindings::hold_retirement = -1;
    CHECK(host.close_world(one) == rt::Status::ok);
    Bindings::fail_attach = static_cast<int>(Bindings::created);
    CHECK(host.create(&first, config(), one) == rt::Status::internal_error);
    CHECK(one.generation != 0 && host.live_worlds() == 1);
    CHECK(host.step(one, {}) == rt::Status::invalid_state);
    CHECK(host.close_all() == rt::Status::ok);
    Bindings::fail_attach = -1;
}
void parallel_worlds() {
    Bindings::parallel_workers = true;
    Host host;
    int first, second;
    rtfw_unreal::Sample a, b;
    rtfw_unreal::WorldHandle one, two;
    start(host, &first, a, one); start(host, &second, b, two);
    rtfw_physics_allocation::begin();
    for (std::uint64_t frame = 0; frame < 64; ++frame) {
        CHECK(host.step(one, {frame, std::chrono::milliseconds(1)}) == rt::Status::ok);
        CHECK(host.step(two, {frame, std::chrono::milliseconds(1)}) == rt::Status::ok);
    }
    CHECK(rtfw_physics_allocation::end() == 0);
    CHECK(a.frames == 64 && b.frames == 64 && a.matches() && b.matches());
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    auto status = host.close_all();
    while (status != rt::Status::ok && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield(); status = host.close_all();
    }
    CHECK(status == rt::Status::ok && host.live_worlds() == 0);
    Bindings::parallel_workers = false;
}
void real_module(const char* module_path) {
    Host host;
    rtfw_host::Library library(host);
    int owner;
    rtfw_unreal::WorldHandle old;
    for (unsigned cycle = 0; cycle < 8; ++cycle) {
        CHECK(library.open(std::filesystem::absolute(module_path)));
        rtfw_unreal::Sample model;
        rtfw_unreal::WorldHandle world;
        CHECK(host.create(&owner, config(), world) == rt::Status::ok);
        rt::ExtensionHandle extension;
        CHECK(host.attach(world, library.module(), extension) == rt::Status::ok);
        CHECK(host.configure(world, rtfw_unreal::Sample::configure, &model) == rt::Status::ok);
        CHECK(host.finalize(world) == rt::Status::ok && host.start(world) == rt::Status::ok);
        CHECK(!library.close());
        if (cycle) CHECK(host.close_world(old) == rt::Status::invalid_handle);
        for (std::uint64_t frame = 0; frame < 16; ++frame)
            CHECK(host.step(world, {frame, std::chrono::milliseconds(1)}) == rt::Status::ok);
        CHECK(model.frames == 16 && model.matches());
        CHECK(host.close_world(world) == rt::Status::invalid_state);
        using Release = void (RTFW_EXTENSION_CALL*)();
        Release release = nullptr;
        CHECK(library.symbol("rtfw_host_demo_release", release));
        release();
        // Hold only host scheduler retirement, after Registry has closed the
        // Runtime and detached its extension. Library must STILL refuse unload.
        Bindings::hold_retirement = static_cast<int>(Bindings::created - 1);
        CHECK(host.close_world(world) == rt::Status::invalid_state);
        CHECK(!library.close());
        Bindings::hold_retirement = -1;
        CHECK(host.close_world(world) == rt::Status::ok);
        CHECK(library.close()); old = world;
    }
}
}
int main(int argc, char** argv) {
    CHECK(argc == 2);
    lifecycle(); retry_and_failed_setup(); parallel_worlds(); real_module(argv[1]);
    CHECK(Bindings::created == Bindings::destroyed);
    std::printf("unreal portable orchestration: %u checks passed; actual Unreal NOT RUN\n", checks);
}
