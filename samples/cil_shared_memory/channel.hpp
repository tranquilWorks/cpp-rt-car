#pragma once
#include "wire.hpp"
#include <algorithm>
#include <type_traits>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif !defined(__linux__) || (!defined(__GNUC__) && !defined(__clang__))
#error "CIL reference requires Linux GCC/Clang or Windows MSVC on little-endian x86-64"
#endif
#if !defined(__x86_64__) && !defined(_M_X64)
#error "CIL reference supports only x86-64"
#endif
namespace cil {
static_assert(sizeof(void*) == 8 && std::endian::native == std::endian::little);
#if defined(_WIN32)
using Word = LONG;
inline std::uint32_t load(const Word& x) noexcept {
    return static_cast<std::uint32_t>(InterlockedCompareExchange(const_cast<Word*>(&x), 0, 0));
}
inline void store(Word& x, std::uint32_t v) noexcept { InterlockedExchange(&x, static_cast<LONG>(v)); }
inline bool claim(Word& x) noexcept { return InterlockedCompareExchange(&x, 1, 0) == 0; }
#else
using Word = std::uint32_t;
static_assert(__atomic_always_lock_free(sizeof(Word), nullptr));
inline std::uint32_t load(const Word& x) noexcept { return __atomic_load_n(&x, __ATOMIC_ACQUIRE); }
inline void store(Word& x, std::uint32_t v) noexcept { __atomic_store_n(&x, v, __ATOMIC_RELEASE); }
inline bool claim(Word& x) noexcept {
    Word expected = 0;
    return __atomic_compare_exchange_n(&x, &expected, 1, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}
#endif
static_assert(sizeof(Word) == 4);
struct alignas(64) Slot {
    Word state;
    std::array<std::byte, 60> padding;
    Wire bytes;
    std::array<std::byte, 32> tail;
};
struct alignas(64) Region {
    Word ready, controller, stopping, acknowledged;
    std::array<std::byte, 48> header;
    Slot plants, commands;
    std::array<std::byte, 3648> reserved;
};
static_assert(std::is_standard_layout_v<Region> && std::is_trivially_copyable_v<Region>);
static_assert(sizeof(Slot) == 192 && offsetof(Slot, bytes) == 64);
static_assert(sizeof(Region) == 4096 && offsetof(Region, header) == 16 &&
              offsetof(Region, plants) == 64 && offsetof(Region, commands) == 256);
// Exactly one producer and one consumer per slot. Lifecycle requires local
// callers joined; no reset/reclaim while another process can still use the map.
inline Code push(Slot& slot, const Wire& w) noexcept {
    const auto state = load(slot.state);
    if (state > 1) return Code::schema;
    if (state) return Code::full;
    slot.bytes = w; store(slot.state, 1); return Code::ok;
}
inline Code pop(Slot& slot, Wire& out) noexcept {
    const auto state = load(slot.state);
    if (state > 1) return Code::schema;
    if (!state) return Code::empty;
    out = slot.bytes; store(slot.state, 0); return Code::ok;
}
// OS-created pages are initially zero. Never memset/reset the published region:
// another process may already be atomically checking ready while we initialize.
inline Code initialize(Region& r, std::uint64_t generation) noexcept {
    if (!generation || load(r.ready) != 0) return Code::invalid;
    r.header.fill(std::byte{0});
    put(r.header, 0, magic, 4); put(r.header, 4, 1, 4);
    put(r.header, 8, sizeof(Region), 4); put(r.header, 12, wire_size, 4);
    put(r.header, 16, generation, 8); put(r.header, 24, 1, 4); // clock domain
    store(r.ready, 1); return Code::ok;
}
inline Code inspect(const Region& r, std::uint64_t generation) noexcept {
    const auto ready = load(r.ready);
    if (!ready) return Code::not_ready;
    if (ready != 1 || get(r.header, 0, 4) != magic || get(r.header, 4, 4) != 1 ||
        get(r.header, 8, 4) != sizeof(Region) || get(r.header, 12, 4) != wire_size ||
        get(r.header, 24, 4) != 1 || get(r.header, 28, 4) || get(r.header, 32, 8) || get(r.header, 40, 8)) return Code::schema;
    if (!generation || get(r.header, 16, 8) != generation) return Code::generation;
    if (load(r.stopping)) return Code::closed;
    return Code::ok;
}
inline Code attach(Region& r, std::uint64_t generation) noexcept {
    auto c = inspect(r, generation); if (c != Code::ok) return c;
    // Never clear this claim: even orderly detach requires a fresh session.
    return claim(r.controller) ? Code::ok : Code::attached;
}
struct Endpoint {
    Region* region = nullptr; // Borrowed; detach before unmapping.
    std::uint64_t generation = 0;
    Sequence sent{}, received{};
    bool host = false;
    Code send(Record r) noexcept {
        if (!region || load(region->stopping)) return Code::closed;
        if (sent.next == UINT64_MAX) return Code::exhausted;
        r.generation = generation; r.sequence = sent.next;
        r.kind = host ? Kind::plant : Kind::command;
        Wire w; auto c = encode(r, w); if (c != Code::ok) return c;
        c = push(host ? region->plants : region->commands, w);
        if (c == Code::ok) sent.advance();
        return c;
    }
    Code receive(std::uint64_t now, Record& out, const Record* plant = nullptr) noexcept {
        if (!region) return Code::closed;
        Wire w; const auto c = pop(host ? region->commands : region->plants, w);
        // Existing records can be drained during orderly stop, but not sent.
        if (c != Code::ok) return c == Code::empty && load(region->stopping) ? Code::closed : c;
        // Only the host compares against its clock. A controller echoes the
        // host time budget; its own steady_clock epoch is not a wire contract.
        Record decoded;
        const auto decoded_code = decode(w, decoded);
        if (decoded_code != Code::ok) return decoded_code;
        return accept(w, host ? Kind::command : Kind::plant, generation, received,
                      host ? now : decoded.issued_ns, out, plant);
    }
    void detach() noexcept { region = nullptr; }
};
} // namespace cil
