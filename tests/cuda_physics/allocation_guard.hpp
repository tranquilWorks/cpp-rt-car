#pragma once
// Test-only allocation instrumentation, preserving the existing sample guard.
#include <atomic>
#include <cstdlib>
#include <limits>
#include <new>
#if defined(_MSC_VER)
#include <malloc.h>
#endif
namespace rtfw_physics_allocation {

std::atomic<bool> tracking{false};
std::atomic<std::size_t> count{0};

void record() noexcept {
    if (tracking.load(std::memory_order_relaxed)) {
        count.fetch_add(1, std::memory_order_relaxed);
    }
}

void* allocate(std::size_t bytes) {
    if (bytes == 0) {
        bytes = 1;
    }
    if (void* storage = std::malloc(bytes)) {
        return storage;
    }
    throw std::bad_alloc();
}

void* allocate_aligned(std::size_t bytes, std::size_t alignment) {
    alignment = std::max(alignment, alignof(std::max_align_t));
    if (bytes == 0) {
        bytes = alignment;
    }
    const auto remainder = bytes % alignment;
    if (remainder != 0) {
        if (bytes > std::numeric_limits<std::size_t>::max() -
                        (alignment - remainder)) {
            throw std::bad_alloc();
        }
        bytes += alignment - remainder;
    }
#if defined(_MSC_VER)
    if (void* storage = _aligned_malloc(bytes, alignment)) {
        return storage;
    }
    throw std::bad_alloc();
#else
    void* storage = nullptr;
    if (posix_memalign(&storage, alignment, bytes) == 0) {
        return storage;
    }
    throw std::bad_alloc();
#endif
}

void deallocate_aligned(void* storage) noexcept {
#if defined(_MSC_VER)
    _aligned_free(storage);
#else
    std::free(storage);
#endif
}

void begin() noexcept {
    count.store(0, std::memory_order_relaxed);
    tracking.store(true, std::memory_order_release);
}

std::size_t end() noexcept {
    tracking.store(false, std::memory_order_release);
    return count.load(std::memory_order_acquire);
}

} // namespace rtfw_physics_allocation

void* operator new(std::size_t bytes) {
    rtfw_physics_allocation::record();
    return rtfw_physics_allocation::allocate(bytes);
}

void* operator new[](std::size_t bytes) {
    rtfw_physics_allocation::record();
    return rtfw_physics_allocation::allocate(bytes);
}

void* operator new(std::size_t bytes, std::align_val_t alignment) {
    rtfw_physics_allocation::record();
    return rtfw_physics_allocation::allocate_aligned(
        bytes, static_cast<std::size_t>(alignment));
}

void* operator new[](std::size_t bytes, std::align_val_t alignment) {
    rtfw_physics_allocation::record();
    return rtfw_physics_allocation::allocate_aligned(
        bytes, static_cast<std::size_t>(alignment));
}

void* operator new(std::size_t bytes, const std::nothrow_t&) noexcept {
    try {
        return ::operator new(bytes);
    } catch (...) {
        return nullptr;
    }
}

void* operator new[](std::size_t bytes, const std::nothrow_t&) noexcept {
    try {
        return ::operator new[](bytes);
    } catch (...) {
        return nullptr;
    }
}

void* operator new(std::size_t bytes, std::align_val_t alignment,
                   const std::nothrow_t&) noexcept {
    try {
        return ::operator new(bytes, alignment);
    } catch (...) {
        return nullptr;
    }
}

void* operator new[](std::size_t bytes, std::align_val_t alignment,
                     const std::nothrow_t&) noexcept {
    try {
        return ::operator new[](bytes, alignment);
    } catch (...) {
        return nullptr;
    }
}

void operator delete(void* storage) noexcept {
    std::free(storage);
}

void operator delete[](void* storage) noexcept {
    std::free(storage);
}

void operator delete(void* storage, std::size_t) noexcept {
    std::free(storage);
}

void operator delete[](void* storage, std::size_t) noexcept {
    std::free(storage);
}

void operator delete(void* storage, std::align_val_t) noexcept {
    rtfw_physics_allocation::deallocate_aligned(storage);
}

void operator delete[](void* storage, std::align_val_t) noexcept {
    rtfw_physics_allocation::deallocate_aligned(storage);
}

void operator delete(void* storage, std::size_t, std::align_val_t) noexcept {
    rtfw_physics_allocation::deallocate_aligned(storage);
}

void operator delete[](void* storage, std::size_t,
                       std::align_val_t) noexcept {
    rtfw_physics_allocation::deallocate_aligned(storage);
}

