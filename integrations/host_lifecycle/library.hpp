#pragma once
#include "lifecycle.hpp"
#include <cstring>
#include <exception>
#include <filesystem>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace rtfw_host {
// Explicitly selected trusted module only. The host must successfully release
// its Registry module record before close(); this object is not a global lease.
template<class HostRegistry>
class Library {
#if defined(_WIN32)
    HMODULE handle_ = nullptr;
#else
    void* handle_ = nullptr;
#endif
    HostRegistry& registry_;
    ModuleHandle module_{};
    bool registered_ = false;
public:
    explicit Library(HostRegistry& registry) noexcept : registry_(registry) {}
    Library(const Library&) = delete;
    Library& operator=(const Library&) = delete;
    ~Library() { if (handle_) std::terminate(); }
    bool open(const std::filesystem::path& path) noexcept {
        if (handle_ || !path.is_absolute()) return false;
#if defined(_WIN32)
        handle_ = LoadLibraryW(path.c_str());
#else
        handle_ = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
        if (!handle_) return false;
        rtfw_extension_entry_fn_v1 entry = nullptr;
        if (!symbol(RTFW_EXTENSION_ENTRY_SYMBOL_V1, entry) ||
            registry_.add_module(this, entry, module_) != rt::Status::ok) {
            if (!close()) std::terminate();
            return false;
        }
        registered_ = true;
        return true;
    }
    ModuleHandle module() const noexcept { return module_; }
    template<class Function>
    bool symbol(const char* name, Function& output) const noexcept {
        if (!handle_ || !name) return false;
#if defined(_WIN32)
        auto address = GetProcAddress(handle_, name);
#else
        auto address = dlsym(handle_, name);
#endif
        if (!address) return false;
        static_assert(sizeof(output) == sizeof(address));
        std::memcpy(&output, &address, sizeof(output));
        return true;
    }
    bool close() noexcept {
        if (!handle_) return false;
        if (registered_) {
            if (registry_.release_module(module_) != rt::Status::ok) return false;
            registered_ = false;
        }
#if defined(_WIN32)
        if (!FreeLibrary(handle_)) return false;
#else
        if (dlclose(handle_) != 0) return false;
#endif
        handle_ = nullptr;
        return true;
    }
};
}
