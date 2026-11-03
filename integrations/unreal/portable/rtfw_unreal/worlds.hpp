#pragma once

#include <lifecycle.hpp>
#include <array>
#include <cstdint>
#include <exception>
#include <optional>

namespace rtfw_unreal {
using WorldHandle = rtfw_host::WorldHandle;
using ModuleHandle = rtfw_host::ModuleHandle;

inline bool same(WorldHandle a, WorldHandle b) noexcept {
    return a.registry == b.registry && a.generation == b.generation && a.slot == b.slot;
}
inline bool same(ModuleHandle a, ModuleHandle b) noexcept {
    return a.registry == b.registry && a.generation == b.generation && a.slot == b.slot;
}

// Shared production orchestration: Unreal supplies Bindings, portable tests
// supply an ordinary host. No engine substitute is used to compile UE code.
// All methods, including inspection, belong to one serialized control thread.
template<class Bindings, std::size_t Worlds = 8, std::size_t Modules = 8,
         std::size_t Attachments = 8>
class WorldsHost {
    using Registry = rtfw_host::Registry<Worlds, Modules, Attachments>;
    struct World {
        void* owner = nullptr;
        WorldHandle handle{};
        bool closing = false, runtime_closed = false;
        std::optional<Bindings> bindings;
        std::array<ModuleHandle, Attachments> modules{};
        std::size_t module_count = 0;
    };
    Registry registry_;
    std::array<World, Worlds> worlds_{};
    bool busy_ = false;
    struct Guard {
        bool& busy;
        explicit Guard(bool& b) noexcept : busy(b) { busy = true; }
        ~Guard() { busy = false; }
    };
    World* find(WorldHandle h) noexcept {
        for (auto& w : worlds_) if (w.owner && same(w.handle, h)) return &w;
        return nullptr;
    }
    rt::Status close(World& w) noexcept {
        w.closing = true;
        w.bindings->close_admission();
        if (!w.runtime_closed) {
            const auto s = registry_.close_world(w.handle);
            if (s != rt::Status::ok) return s;
            w.runtime_closed = true;
        }
        // Runtime completion precedes scheduler task-node retirement. Keep
        // bindings AND module ownership until both have finished, across retries.
        if (!w.bindings->quiescent()) return rt::Status::invalid_state;
        w.bindings.reset();
        w.owner = nullptr;
        w.module_count = 0;
        return rt::Status::ok;
    }
public:
    using Configure = typename Registry::Configure;
    WorldsHost() = default;
    WorldsHost(const WorldsHost&) = delete;
    WorldsHost& operator=(const WorldsHost&) = delete;
    ~WorldsHost() { for (const auto& w : worlds_) if (w.owner) std::terminate(); }

    // Once a Runtime is created, output is a live cleanup token even if binding
    // attachment fails. Before that point output is preserved. Always close an
    // issued token; close_all also recovers failed configuration/start attempts.
    rt::Status create(void* owner, const rt::RuntimeConfig& config,
                      WorldHandle& output) noexcept {
        if (busy_) return rt::Status::invalid_state;
        if (!owner) return rt::Status::invalid_argument;
        for (const auto& w : worlds_) if (w.owner == owner) return rt::Status::invalid_state;
        Guard guard(busy_);
        for (auto& w : worlds_) {
            if (w.owner) continue;
            try { w.bindings.emplace(config); }
            catch (...) { return rt::Status::resource_exhausted; }
            if (!w.bindings->valid()) { w.bindings.reset(); return rt::Status::invalid_config; }
            WorldHandle h;
            auto s = registry_.create_world(owner, config, h, &w.bindings->clock());
            if (s != rt::Status::ok) { w.bindings.reset(); return s; }
            w.owner = owner; w.handle = h; w.closing = w.runtime_closed = false;
            w.module_count = 0; output = h;
            struct Context { Bindings* bindings; const rt::RuntimeConfig* config; } context{&*w.bindings, &config};
            s = registry_.configure_world(h, [](void* opaque, rt::Runtime& runtime) noexcept {
                auto& c = *static_cast<Context*>(opaque);
                return c.bindings->attach(runtime, *c.config);
            }, &context);
            if (s != rt::Status::ok) w.closing = true;
            return s;
        }
        return rt::Status::resource_exhausted;
    }
    rt::Status configure(WorldHandle h, Configure callback, void* context) noexcept {
        if (busy_) return rt::Status::invalid_state;
        auto* w = find(h);
        if (!w) return rt::Status::invalid_handle;
        if (w->closing) return rt::Status::invalid_state;
        Guard guard(busy_);
        return registry_.configure_world(h, callback, context);
    }
    rt::Status finalize(WorldHandle h) noexcept {
        if (busy_) return rt::Status::invalid_state;
        auto* w = find(h);
        if (!w) return rt::Status::invalid_handle;
        if (w->closing) return rt::Status::invalid_state;
        Guard guard(busy_); return registry_.finalize(h);
    }
    rt::Status start(WorldHandle h) noexcept {
        if (busy_) return rt::Status::invalid_state;
        auto* w = find(h);
        if (!w) return rt::Status::invalid_handle;
        if (w->closing) return rt::Status::invalid_state;
        Guard guard(busy_); return registry_.start(h);
    }
    rt::Status step(WorldHandle h, const rt::HostFrameContext& frame,
                    rt::StepResult* result = nullptr) noexcept {
        if (busy_) return rt::Status::invalid_state;
        auto* w = find(h);
        if (!w) return rt::Status::invalid_handle;
        if (w->closing) return rt::Status::invalid_state;
        Guard guard(busy_); return registry_.step(h, frame, result);
    }
    rt::Status close_world(WorldHandle h) noexcept {
        if (busy_) return rt::Status::invalid_state;
        auto* w = find(h);
        if (!w) return rt::Status::invalid_handle;
        Guard guard(busy_); return close(*w);
    }
    rt::Status close_all() noexcept {
        if (busy_) return rt::Status::invalid_state;
        Guard guard(busy_);
        auto first = rt::Status::ok;
        for (auto& w : worlds_) {
            if (!w.owner) continue;
            const auto s = close(w);
            if (first == rt::Status::ok && s != rt::Status::ok) first = s;
        }
        return first;
    }
    std::size_t live_worlds() const noexcept {
        std::size_t count = 0;
        for (const auto& w : worlds_) count += w.owner != nullptr ? 1u : 0u;
        return count;
    }
    rt::Status add_module(void* owner, rtfw_extension_entry_fn_v1 entry, ModuleHandle& output) noexcept {
        if (busy_) return rt::Status::invalid_state;
        Guard guard(busy_); return registry_.add_module(owner, entry, output);
    }
    rt::Status attach(WorldHandle h, ModuleHandle module, rt::ExtensionHandle& output) noexcept {
        if (busy_) return rt::Status::invalid_state;
        auto* w = find(h);
        if (!w) return rt::Status::invalid_handle;
        if (w->closing) return rt::Status::invalid_state;
        if (w->module_count == Attachments) return rt::Status::resource_exhausted;
        Guard guard(busy_);
        const auto s = registry_.attach(h, module, output);
        if (s == rt::Status::ok) w->modules[w->module_count++] = module;
        return s;
    }
    rt::Status release_module(ModuleHandle module) noexcept {
        if (busy_) return rt::Status::invalid_state;
        for (const auto& w : worlds_) {
            if (!w.owner) continue;
            for (std::size_t i = 0; i < w.module_count; ++i)
                if (same(w.modules[i], module)) return rt::Status::invalid_state;
        }
        Guard guard(busy_); return registry_.release_module(module);
    }
};
} // namespace rtfw_unreal
