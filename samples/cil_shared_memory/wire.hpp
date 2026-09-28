#pragma once
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>

namespace cil {
enum class Code { ok, empty, full, not_ready, invalid, schema, generation, sequence,
                  future, expired, correlation, closed, attached, exhausted, io,
                  not_found, exists, timeout, no_ack };
inline constexpr std::string_view name(Code c) noexcept {
    switch (c) {
    case Code::ok: return "ok"; case Code::empty: return "empty";
    case Code::full: return "full"; case Code::not_ready: return "not_ready";
    case Code::invalid: return "invalid"; case Code::schema: return "schema";
    case Code::generation: return "generation"; case Code::sequence: return "sequence";
    case Code::future: return "future"; case Code::expired: return "expired";
    case Code::correlation: return "correlation"; case Code::closed: return "closed";
    case Code::attached: return "attached"; case Code::exhausted: return "exhausted";
    case Code::io: return "io"; case Code::not_found: return "not_found";
    case Code::exists: return "exists"; case Code::timeout: return "timeout";
    case Code::no_ack: return "no_ack";
    }
    return "unknown";
}
inline constexpr std::uint32_t magic = 0x314c4943; // bytes C I L 1
inline constexpr std::uint64_t max_ttl_ns = 10'000'000'000;
inline constexpr std::size_t wire_size = 96;
using Wire = std::array<std::byte, wire_size>;
enum class Kind : std::uint32_t { plant = 1, command = 2 };
struct Record {
    Kind kind = Kind::plant;
    std::uint64_t generation = 0, sequence = 0, issued_ns = 0, expiry_ns = 0,
                  correlation = 0, frame = 0;
    std::int64_t value = 0;
};
inline void put(std::span<std::byte> out, std::size_t offset, std::uint64_t value,
                std::size_t bytes) noexcept {
    for (std::size_t i = 0; i < bytes; ++i) out[offset + i] = static_cast<std::byte>((value >> (i * 8)) & 255);
}
inline std::uint64_t get(std::span<const std::byte> in, std::size_t offset,
                         std::size_t bytes) noexcept {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < bytes; ++i) value |= std::uint64_t(std::to_integer<unsigned>(in[offset + i])) << (i * 8);
    return value;
}
inline Code fields(const Record& r) noexcept {
    if (r.kind != Kind::plant && r.kind != Kind::command) return Code::schema;
    if (!r.generation || !r.sequence || !r.issued_ns || r.expiry_ns <= r.issued_ns ||
        r.expiry_ns - r.issued_ns > max_ttl_ns) return Code::invalid;
    if (r.sequence == UINT64_MAX || r.frame == UINT64_MAX) return Code::exhausted;
    if (r.value < -1'000'000 || r.value > 1'000'000 ||
        (r.kind == Kind::command && (r.value < -256 || r.value > 256))) return Code::invalid;
    if ((r.kind == Kind::plant && r.correlation != 0) ||
        (r.kind == Kind::command && r.correlation == 0)) return Code::correlation;
    return Code::ok;
}
inline Code encode(const Record& r, Wire& out) noexcept {
    const auto code = fields(r); if (code != Code::ok) return code;
    Wire w{};
    put(w, 0, magic, 4); put(w, 4, 1, 4); put(w, 8, wire_size, 4);
    put(w, 12, static_cast<std::uint32_t>(r.kind), 4);
    put(w, 16, r.generation, 8); put(w, 24, r.sequence, 8);
    put(w, 32, r.issued_ns, 8); put(w, 40, r.expiry_ns, 8);
    put(w, 48, r.correlation, 8); put(w, 56, r.frame, 8);
    put(w, 64, std::bit_cast<std::uint64_t>(r.value), 8);
    put(w, 76, 1, 4); // Host monotonic nanosecond domain, no cross-process clock conversion.
    out = w; return Code::ok;
}
inline Code decode(std::span<const std::byte> w, Record& out) noexcept {
    if (w.size() != wire_size || get(w, 0, 4) != magic || get(w, 4, 4) != 1 ||
        get(w, 8, 4) != wire_size || get(w, 72, 4) != 0 || get(w, 76, 4) != 1 ||
        get(w, 80, 8) != 0 || get(w, 88, 8) != 0) return Code::schema;
    Record r{static_cast<Kind>(get(w, 12, 4)), get(w, 16, 8), get(w, 24, 8),
        get(w, 32, 8), get(w, 40, 8), get(w, 48, 8), get(w, 56, 8),
        std::bit_cast<std::int64_t>(get(w, 64, 8))};
    const auto code = fields(r); if (code != Code::ok) return code;
    out = r; return Code::ok;
}
inline Code fresh(const Record& r, std::uint64_t now) noexcept {
    if (r.issued_ns > now) return Code::future;
    return now >= r.expiry_ns ? Code::expired : Code::ok;
}
struct Sequence {
    std::uint64_t next = 1;
    Code inspect(std::uint64_t value) const noexcept {
        if (next == UINT64_MAX) return Code::exhausted;
        return value == next ? Code::ok : Code::sequence;
    }
    void advance() noexcept { if (next != UINT64_MAX) ++next; }
};
// Transactional acceptance: invalid data never advances sequence or mutates out.
inline Code accept(const Wire& w, Kind kind, std::uint64_t generation,
                   Sequence& sequence, std::uint64_t now, Record& out,
                   const Record* plant = nullptr) noexcept {
    Record r; auto code = decode(w, r); if (code != Code::ok) return code;
    if (r.kind != kind) return Code::schema;
    if (r.generation != generation) return Code::generation;
    code = sequence.inspect(r.sequence); if (code != Code::ok) return code;
    code = fresh(r, now); if (code != Code::ok) return code;
    if (kind == Kind::command && (!plant || r.correlation != plant->sequence ||
        r.frame != plant->frame || r.issued_ns != plant->issued_ns || r.expiry_ns != plant->expiry_ns)) return Code::correlation;
    out = r; sequence.advance(); return Code::ok;
}
} // namespace cil
