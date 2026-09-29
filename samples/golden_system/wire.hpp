#pragma once
#include "model.hpp"
namespace golden::cil {
enum class Code {
  ok,
  empty,
  full,
  not_ready,
  invalid,
  schema,
  generation,
  sequence,
  future,
  expired,
  correlation,
  closed,
  attached,
  exhausted,
  io,
  not_found,
  exists,
  timeout,
  no_ack
};
inline constexpr std::string_view name(Code c) noexcept {
  switch (c) {
  case Code::ok:
    return "ok";
  case Code::empty:
    return "empty";
  case Code::full:
    return "full";
  case Code::not_ready:
    return "not_ready";
  case Code::invalid:
    return "invalid";
  case Code::schema:
    return "schema";
  case Code::generation:
    return "generation";
  case Code::sequence:
    return "sequence";
  case Code::future:
    return "future";
  case Code::expired:
    return "expired";
  case Code::correlation:
    return "correlation";
  case Code::closed:
    return "closed";
  case Code::attached:
    return "attached";
  case Code::exhausted:
    return "exhausted";
  case Code::io:
    return "io";
  case Code::not_found:
    return "not_found";
  case Code::exists:
    return "exists";
  case Code::timeout:
    return "timeout";
  case Code::no_ack:
    return "no_ack";
  }
  return "unknown";
}
inline constexpr std::uint32_t magic =
    0x314c3647; // G6L1, distinct from M25 scalar CIL1.
inline constexpr std::size_t wire_size = 3200;
using Wire = std::array<std::byte, wire_size>;
enum class Kind : std::uint32_t { plant = 1, command = 2 };
struct Record {
  Kind kind = Kind::plant;
  std::uint32_t count = 16;
  std::int32_t target = 8, gain = 1;
  std::uint64_t generation = 0, sequence = 0, issued_tick = 0, expiry_tick = 1,
                correlation = 0;
  Vector values{};
};
inline Code fields(const Record &r) noexcept {
  if (r.kind != Kind::plant && r.kind != Kind::command)
    return Code::schema;
  if (!r.generation || !r.sequence || !r.count || r.count > fixed::capacity ||
      r.target < -64 || r.target > 64 || r.gain < 0 || r.gain > 4 ||
      r.issued_tick >= fixed::maximum_ticks ||
      r.expiry_tick != r.issued_tick + 1)
    return Code::invalid;
  if (r.sequence == UINT64_MAX)
    return Code::exhausted;
  if ((r.kind == Kind::plant && r.correlation) ||
      (r.kind == Kind::command && !r.correlation))
    return Code::correlation;
  for (const auto &axis : r.values)
    for (std::size_t i = 0; i < fixed::capacity; ++i) {
      const auto v = axis[i];
      if (i >= r.count && v)
        return Code::invalid;
      if (v < -4176 || v > 4176 ||
          (r.kind == Kind::command && (v < -4 || v > 4)))
        return Code::invalid;
    }
  return Code::ok;
}
inline Code encode(const Record &r, Wire &out) noexcept {
  auto c = fields(r);
  if (c != Code::ok)
    return c;
  Wire w{};
  put(w, 0, magic, 4);
  put(w, 4, 1, 4);
  put(w, 8, wire_size, 4);
  put(w, 12, static_cast<std::uint32_t>(r.kind), 4);
  put(w, 16, r.count, 4);
  put32(w, 20, r.target);
  put32(w, 24, r.gain);
  put(w, 28, 1, 4); // logical host tick domain
  put(w, 32, r.generation, 8);
  put(w, 40, r.sequence, 8);
  put(w, 48, r.issued_tick, 8);
  put(w, 56, r.expiry_tick, 8);
  put(w, 64, r.correlation, 8);
  std::size_t at = 128;
  for (const auto &axis : r.values)
    for (auto v : axis) {
      put32(w, at, v);
      at += 4;
    }
  // Digest binds header and payload with the digest word zeroed.
  put(w, 72, digest(w), 8);
  out = w;
  return Code::ok;
}
inline Code decode(std::span<const std::byte> bytes, Record &out) noexcept {
  if (bytes.size() != wire_size || get(bytes, 0, 4) != magic ||
      get(bytes, 4, 4) != 1 || get(bytes, 8, 4) != wire_size ||
      get(bytes, 28, 4) != 1)
    return Code::schema;
  for (std::size_t i = 80; i < 128; ++i)
    if (bytes[i] != std::byte{})
      return Code::schema;
  Wire w;
  std::copy(bytes.begin(), bytes.end(), w.begin());
  const auto checksum = get(w, 72, 8);
  put(w, 72, 0, 8);
  if (checksum != digest(w))
    return Code::schema;
  Record r;
  r.kind = static_cast<Kind>(get(w, 12, 4));
  r.count = static_cast<std::uint32_t>(get(w, 16, 4));
  r.target = get32(w, 20);
  r.gain = get32(w, 24);
  r.generation = get(w, 32, 8);
  r.sequence = get(w, 40, 8);
  r.issued_tick = get(w, 48, 8);
  r.expiry_tick = get(w, 56, 8);
  r.correlation = get(w, 64, 8);
  std::size_t at = 128;
  for (auto &axis : r.values)
    for (auto &v : axis) {
      v = get32(w, at);
      at += 4;
    }
  auto c = fields(r);
  if (c == Code::ok)
    out = r;
  return c;
}
struct Sequence {
  std::uint64_t next = 1;
  Code inspect(std::uint64_t value) const noexcept {
    return next == UINT64_MAX ? Code::exhausted
                              : (value == next ? Code::ok : Code::sequence);
  }
  void advance() noexcept {
    if (next != UINT64_MAX)
      ++next;
  }
};
inline Code accept(const Wire &w, Kind kind, std::uint64_t generation,
                   Sequence &sequence, std::uint64_t now, Record &out,
                   const Record *plant = nullptr) noexcept {
  Record r;
  auto c = decode(w, r);
  if (c != Code::ok)
    return c;
  if (r.kind != kind)
    return Code::schema;
  if (r.generation != generation)
    return Code::generation;
  c = sequence.inspect(r.sequence);
  if (c != Code::ok)
    return c;
  if (r.issued_tick > now)
    return Code::future;
  if (r.expiry_tick <= now)
    return Code::expired;
  if (kind == Kind::command &&
      (!plant || r.correlation != plant->sequence ||
       r.issued_tick != plant->issued_tick ||
       r.expiry_tick != plant->expiry_tick || r.count != plant->count ||
       r.target != plant->target || r.gain != plant->gain))
    return Code::correlation;
  out = r;
  sequence.advance();
  return Code::ok;
}
} // namespace golden::cil
