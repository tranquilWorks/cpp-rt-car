#pragma once

#include <rt/runtime.hpp>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <optional>

namespace rtfw_host {
namespace detail {
// Host-local identity. Never wrap or retry an unbounded compare/exchange loop.
inline std::atomic<std::uint64_t> registry_sequence{0};
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
inline std::uint64_t next_registry() noexcept {
    auto value = registry_sequence.load(std::memory_order_relaxed);
    if (value == std::numeric_limits<std::uint64_t>::max()) return 0;
    return registry_sequence.compare_exchange_strong(value, value + 1,
        std::memory_order_relaxed) ? value + 1 : 0;
}
}

struct WorldHandle {
    std::uint64_t registry = 0, generation = 0;
    std::size_t slot = 0;
};
struct ModuleHandle {
    std::uint64_t registry = 0, generation = 0;
    std::size_t slot = 0;
};

// One serialized host control thread owns this registry and all frame calls.
// It owns Runtime objects; clocks, callback/provider/job data and modules remain
// borrowed. They must outlive checked world close and module release.
template<std::size_t Worlds = 8, std::size_t Modules = 8,
         std::size_t Bindings = 8,
         std::uint64_t LastGeneration = std::numeric_limits<std::uint64_t>::max()>
class Registry {
    static_assert(Worlds > 0 && Worlds <= 64 && Modules > 0 && Modules <= 64);
    static_assert(Bindings > 0 && Bindings <= RTFW_RUNTIME_EXTENSION_CAPACITY);
    static_assert(LastGeneration > 0);
    struct Binding {
        bool live = false;
        std::size_t module = 0;
        rt::ExtensionHandle extension{};
    };
    struct World {
        std::uint64_t generation = 1;
        void* owner = nullptr;
        bool closing = false;
        std::optional<rt::Runtime> runtime;
        std::array<Binding, Bindings> bindings{};
    };
    struct Module {
        std::uint64_t generation = 1;
        void* owner = nullptr;
        rtfw_extension_entry_fn_v1 entry = nullptr;
        std::size_t references = 0;
    };
    std::array<World, Worlds> worlds_{};
    std::array<Module, Modules> modules_{};
    const std::uint64_t identity_ = detail::next_registry();
    bool busy_ = false;
    struct Guard {
        bool& busy;
        explicit Guard(bool& value) noexcept : busy(value) { busy = true; }
        ~Guard() { busy = false; }
    };
    bool valid(WorldHandle h) const noexcept {
        return h.registry == identity_ && h.generation != 0 && h.slot < Worlds &&
            worlds_[h.slot].owner && worlds_[h.slot].generation == h.generation;
    }
    bool valid(ModuleHandle h) const noexcept {
        return h.registry == identity_ && h.generation != 0 && h.slot < Modules &&
            modules_[h.slot].owner && modules_[h.slot].generation == h.generation;
    }
    static void retire(std::uint64_t& generation) noexcept {
        generation = generation == LastGeneration ? 0 : generation + 1;
    }
    void release_binding(Binding& binding) noexcept {
        --modules_[binding.module].references;
        binding = {};
    }
    rt::Status close(World& world) noexcept {
        world.closing = true; // Never reopen admission after a failed cleanup.
        if (world.runtime->state() == rt::RuntimeState::configuring) {
            // No start is possible in this state. Destroy the unstarted Runtime
            // before retiring its copied callable tables and module references.
            world.runtime.reset();
            for (auto& binding : world.bindings)
                if (binding.live) release_binding(binding);
        } else {
            auto status = world.runtime->stop();
            if (status != rt::Status::ok) return status;
            rt::Status first = rt::Status::ok;
            for (auto& binding : world.bindings) {
                if (!binding.live) continue;
                bool ready = false;
                status = world.runtime->detach_extension(binding.extension, ready);
                if (status == rt::Status::ok && ready) release_binding(binding);
                else if (first == rt::Status::ok)
                    first = status == rt::Status::ok ? rt::Status::invalid_state : status;
            }
            if (first != rt::Status::ok) return first;
            world.runtime.reset();
        }
        world.owner = nullptr;
        retire(world.generation);
        return rt::Status::ok;
    }
    template<class Operation>
    rt::Status call(WorldHandle h, Operation operation) noexcept {
        if (!identity_ || busy_) return rt::Status::invalid_state;
        if (!valid(h)) return rt::Status::invalid_handle;
        auto& world = worlds_[h.slot];
        if (world.closing) return rt::Status::invalid_state;
        Guard guard(busy_);
        return operation(*world.runtime);
    }
public:
    using Configure = rt::Status (*)(void*, rt::Runtime&) noexcept;
    Registry() = default;
    Registry(const Registry&) = delete;
    Registry& operator=(const Registry&) = delete;
    ~Registry() {
        // Destruction cannot report cleanup failure. Require an explicit close
        // and release; never dereference a borrowed owner from a destructor.
        for (const auto& world : worlds_) if (world.owner) std::terminate();
        for (const auto& module : modules_) if (module.owner) std::terminate();
    }
    bool ready() const noexcept { return identity_ != 0; }

    rt::Status create_world(void* owner, const rt::RuntimeConfig& config,
                            WorldHandle& output, rt::RuntimeClock* clock = nullptr) noexcept {
        if (!identity_ || busy_) return rt::Status::invalid_state;
        if (!owner) return rt::Status::invalid_argument;
        for (const auto& world : worlds_)
            if (world.owner == owner) return rt::Status::invalid_state;
        Guard guard(busy_);
        for (std::size_t i = 0; i < Worlds; ++i) {
            auto& world = worlds_[i];
            if (world.owner || !world.generation) continue;
            try {
                if (clock) world.runtime.emplace(*clock);
                else world.runtime.emplace();
            } catch (...) { return rt::Status::resource_exhausted; }
            const auto status = world.runtime->configure(config);
            if (status != rt::Status::ok) { world.runtime.reset(); return status; }
            world.owner = owner;
            world.closing = false;
            output = {identity_, world.generation, i};
            return rt::Status::ok;
        }
        return rt::Status::resource_exhausted;
    }
    rt::Status configure_world(WorldHandle h, Configure configure, void* configuration_context) noexcept {
        if (!configure) return rt::Status::invalid_argument;
        return call(h, [=](rt::Runtime& runtime) noexcept {
            if (runtime.state() != rt::RuntimeState::configuring) return rt::Status::invalid_state;
            const auto result = configure(configuration_context, runtime);
            return runtime.state() == rt::RuntimeState::configuring
                ? result : rt::Status::invalid_state;
        });
    }
    rt::Status add_module(void* owner, rtfw_extension_entry_fn_v1 entry,
                          ModuleHandle& output) noexcept {
        if (!identity_ || busy_) return rt::Status::invalid_state;
        if (!owner || !entry) return rt::Status::invalid_argument;
        for (const auto& module : modules_)
            if (module.owner && (module.owner == owner || module.entry == entry))
                return rt::Status::invalid_state;
        for (std::size_t i = 0; i < Modules; ++i) {
            auto& module = modules_[i];
            if (module.owner || !module.generation) continue;
            module.owner = owner;
            module.entry = entry;
            output = {identity_, module.generation, i};
            return rt::Status::ok;
        }
        return rt::Status::resource_exhausted;
    }
    rt::Status attach(WorldHandle h, ModuleHandle module, rt::ExtensionHandle& output) noexcept {
        if (!identity_ || busy_) return rt::Status::invalid_state;
        if (!valid(h) || !valid(module)) return rt::Status::invalid_handle;
        auto& world = worlds_[h.slot];
        if (world.closing || world.runtime->state() != rt::RuntimeState::configuring)
            return rt::Status::invalid_state;
        for (const auto& binding : world.bindings)
            if (binding.live && binding.module == module.slot) return rt::Status::invalid_state;
        Guard guard(busy_);
        for (auto& binding : world.bindings) {
            if (binding.live) continue;
            rt::ExtensionHandle extension{};
            const auto status = world.runtime->register_extension(modules_[module.slot].entry, extension);
            if (status != rt::Status::ok) return status;
            binding = {true, module.slot, extension};
            ++modules_[module.slot].references;
            output = extension;
            return rt::Status::ok;
        }
        return rt::Status::resource_exhausted;
    }
    rt::Status finalize(WorldHandle h) noexcept {
        return call(h, [](rt::Runtime& runtime) noexcept { return runtime.finalize(); });
    }
    rt::Status start(WorldHandle h) noexcept {
        return call(h, [](rt::Runtime& runtime) noexcept { return runtime.start(); });
    }
    rt::Status step(WorldHandle h, const rt::HostFrameContext& frame,
                    rt::StepResult* result = nullptr) noexcept {
        return call(h, [&](rt::Runtime& runtime) noexcept { return runtime.step(frame, result); });
    }
    rt::Status close_world(WorldHandle h) noexcept {
        if (!identity_ || busy_) return rt::Status::invalid_state;
        if (!valid(h)) return rt::Status::invalid_handle;
        Guard guard(busy_);
        return close(worlds_[h.slot]);
    }
    rt::Status close_all() noexcept {
        if (!identity_ || busy_) return rt::Status::invalid_state;
        Guard guard(busy_);
        rt::Status first = rt::Status::ok;
        for (auto& world : worlds_) {
            if (!world.owner) continue;
            const auto status = close(world);
            if (first == rt::Status::ok && status != rt::Status::ok) first = status;
        }
        return first;
    }
    rt::Status module_references(ModuleHandle h, std::size_t& output) const noexcept {
        if (!identity_ || busy_) return rt::Status::invalid_state;
        if (!valid(h)) return rt::Status::invalid_handle;
        output = modules_[h.slot].references;
        return rt::Status::ok;
    }
    rt::Status release_module(ModuleHandle h) noexcept {
        if (!identity_ || busy_) return rt::Status::invalid_state;
        if (!valid(h)) return rt::Status::invalid_handle;
        auto& module = modules_[h.slot];
        if (module.references) return rt::Status::invalid_state;
        module.owner = nullptr;
        module.entry = nullptr;
        retire(module.generation);
        return rt::Status::ok;
    }
};
}
