#pragma once

#include "aligned_storage.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>

#include <rt/runtime.hpp>

namespace rt::detail {

static_assert(
    std::atomic<std::uint64_t>::is_always_lock_free,
    "live-control actions require lock-free 64-bit atomics");

#if defined(_MSC_VER)
#    pragma warning(push)
#    pragma warning(disable : 4324)
#endif

class LiveControlActionRing final {
public:
    explicit LiveControlActionRing(std::size_t capacity);
    ~LiveControlActionRing();

    LiveControlActionRing(const LiveControlActionRing&) = delete;
    LiveControlActionRing& operator=(const LiveControlActionRing&) = delete;

    [[nodiscard]] bool reserve_sequence(std::uint64_t& sequence) noexcept;
    [[nodiscard]] bool publish_reserved(LiveControlActionRecord record,
                                        std::uint64_t sequence) noexcept;
    [[nodiscard]] bool emit(
        LiveControlActionRecord record,
        std::uint64_t* assigned_sequence = nullptr) noexcept;
    [[nodiscard]] bool read_sequence(
        std::uint64_t sequence,
        LiveControlActionRecord& record) const noexcept;
    [[nodiscard]] std::uint64_t oldest_sequence(
        std::uint64_t end_sequence) const noexcept;
    [[nodiscard]] bool gap_free(
        std::uint64_t first_sequence,
        std::uint64_t count) const noexcept;
    void restore_sequence(std::uint64_t next_sequence) noexcept;

    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] const void* slot_data() const noexcept { return slots_; }
    [[nodiscard]] std::size_t slot_storage_bytes() const noexcept {
        return capacity_ * sizeof(Slot);
    }
    [[nodiscard]] std::uint64_t next_sequence() const noexcept {
        return next_sequence_.load(std::memory_order_acquire);
    }
    [[nodiscard]] std::uint64_t emitted() const noexcept {
        return emitted_.load(std::memory_order_acquire);
    }
    [[nodiscard]] std::uint64_t overwritten() const noexcept {
        return overwritten_.load(std::memory_order_acquire);
    }
    [[nodiscard]] std::uint64_t dropped() const noexcept {
        return dropped_.load(std::memory_order_acquire);
    }

    [[nodiscard]] static constexpr std::size_t slot_size() noexcept;
    [[nodiscard]] static constexpr std::size_t slot_alignment() noexcept;

private:
    static constexpr std::uint64_t invalid_sequence =
        std::numeric_limits<std::uint64_t>::max();
    static constexpr std::size_t word_count =
        sizeof(LiveControlActionRecord) / sizeof(std::uint64_t);
    static_assert(sizeof(LiveControlActionRecord) % sizeof(std::uint64_t) == 0);

    struct alignas(64) Slot {
        std::atomic_flag writer = ATOMIC_FLAG_INIT;
        std::atomic<std::uint64_t> committed_sequence{invalid_sequence};
        std::array<std::atomic<std::uint64_t>, word_count> words{};
    };

    AlignedStorage storage_{};
    Slot* slots_ = nullptr;
    std::size_t capacity_ = 0;
    alignas(64) std::atomic<std::uint64_t> next_sequence_{0};
    alignas(64) std::atomic<std::uint64_t> emitted_{0};
    alignas(64) std::atomic<std::uint64_t> overwritten_{0};
    alignas(64) std::atomic<std::uint64_t> dropped_{0};
};

#if defined(_MSC_VER)
#    pragma warning(pop)
#endif

constexpr std::size_t LiveControlActionRing::slot_size() noexcept {
    return sizeof(Slot);
}

constexpr std::size_t LiveControlActionRing::slot_alignment() noexcept {
    return alignof(Slot);
}

[[nodiscard]] bool live_control_action_valid(
    const LiveControlActionRecord& record) noexcept;

} // namespace rt::detail
