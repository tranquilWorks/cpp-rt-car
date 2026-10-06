#include "lifecycle.hpp"
#include "library.hpp"
#include "time.hpp"
#include <cstdio>
#include <cstdlib>

static void check(bool condition, const char* message) {
    if (!condition) { std::fprintf(stderr, "host_lifecycle: %s\n", message); std::abort(); }
}

int main(int argc, char** argv) {
    if (argc != 2) { std::fprintf(stderr, "usage: host_lifecycle /absolute/path/to/demo_module\n"); return 2; }
    const std::filesystem::path path(argv[1]);
    rtfw_host::Registry<2, 1, 1> registry;
    check(registry.ready(), "registry identity unavailable");
    rtfw_host::WorldHandle previous_world;
    rtfw_host::ModuleHandle previous_module;
    for (unsigned cycle = 0; cycle < 8; ++cycle) {
        rtfw_host::Library library(registry);
        if (!library.open(path)) { std::fprintf(stderr, "cannot open selected absolute module path\n"); return 1; }
        rtfw_extension_entry_fn_v1 entry = nullptr;
        using Release = void (RTFW_EXTENSION_CALL *)();
        using Frames = std::uint64_t (RTFW_EXTENSION_CALL *)(unsigned);
        Release release = nullptr;
        Frames frames = nullptr;
        if (!library.symbol(RTFW_EXTENSION_ENTRY_SYMBOL_V1, entry) ||
            !library.symbol("rtfw_host_demo_release", release) ||
            !library.symbol("rtfw_host_demo_frames", frames)) {
            check(library.close(), "failed module-symbol cleanup"); return 1;
        }
        const auto module = library.module();
        if (cycle) {
            std::size_t previous_refs = 99;
            check(registry.module_references(previous_module, previous_refs) == rt::Status::invalid_handle &&
                  previous_refs == 99, "stale module after actual reload");
            check(registry.close_world(previous_world) == rt::Status::invalid_handle, "stale world after reload");
        }
        std::array<int, 2> world_owner{};
        std::array<rtfw_host::WorldHandle, 2> world{};
        rt::RuntimeConfig config;
        config.worker_count = 1;
        for (std::size_t i = 0; i < world.size(); ++i) {
            check(registry.create_world(&world_owner[i], config, world[i]) == rt::Status::ok, "world creation");
            rt::ExtensionHandle extension;
            check(registry.attach(world[i], module, extension) == rt::Status::ok, "extension attachment");
            check(registry.finalize(world[i]) == rt::Status::ok, "world finalization");
            check(registry.start(world[i]) == rt::Status::ok, "world start");
        }
        for (std::uint64_t index = 0; index < 16; ++index) {
            rt::HostFrameContext frame;
            check(rtfw_host::frame_from_ticks(index, 3000, 3000000, std::nullopt, frame)
                  == rt::Status::ok, "frame tick conversion");
            for (auto h : world) check(registry.step(h, frame) == rt::Status::ok, "world frame");
        }
        check(frames(0) == 16 && frames(1) == 16, "isolated module state");
        check(registry.close_all() == rt::Status::invalid_state, "retained first shutdown error");
        std::size_t references = 0;
        check(registry.module_references(module, references) == rt::Status::ok && references == 1,
              "independent second-world cleanup");
        check(registry.release_module(module) == rt::Status::invalid_state, "unload refused while borrowed");
        check(!library.close(), "managed library retains live module");
        check(registry.step(world[0], {}) == rt::Status::invalid_state, "closed frame admission");
        check(registry.close_world(world[1]) == rt::Status::invalid_handle, "retired world handle");
        release();
        check(registry.close_all() == rt::Status::ok, "checked shutdown retry");
        check(registry.module_references(module, references) == rt::Status::ok && references == 0,
              "all Runtime callable owners detached");
        entry = nullptr; release = nullptr; frames = nullptr;
        check(library.close(), "actual operating-system module unload");
        check(registry.module_references(module, references) == rt::Status::invalid_handle, "retired module handle");
        previous_world = world[0]; previous_module = module;
    }
    std::puts("host_lifecycle: cycles=8 worlds=2 frames=16 partial_cleanup=retained retry=ok unload=ok");
}
