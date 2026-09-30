#include "../../samples/cil_shared_memory/plant_runtime.hpp"
#include <atomic>
#include <thread>
#include <iostream>
#include <string>
#include "../cuda_physics/allocation_guard.hpp"
#define CHECK(x) do { if (!(x)) { std::cerr << __func__ << ':' << __LINE__ << " failed: " #x << '\n'; return false; } } while (false)
using cil::Code;
cil::Record sample() { return {cil::Kind::command, 0x0102030405060708ULL, 9, 1000, 2000, 7, 6, -3}; }
bool wire() {
    constexpr std::array<unsigned char, 96> golden{
#include "wire_golden.inc"
    };
    auto r = sample(); cil::Wire w{}; CHECK(cil::encode(r, w) == Code::ok);
    for (std::size_t i = 0; i < w.size(); ++i) CHECK(std::to_integer<unsigned char>(w[i]) == golden[i]);
    cil::Record out; CHECK(cil::decode(w, out) == Code::ok);
    CHECK(out.generation == 0x0102030405060708ULL && out.sequence == 9 && out.value == -3 && out.frame == 6 && out.correlation == 7);
    for (std::size_t length = 0; length < 96; ++length) {
        out.value = 123; CHECK(cil::decode(std::span<const std::byte>(w).first(length), out) == Code::schema && out.value == 123);
    }
    std::array<std::byte, 97> extra{}; CHECK(cil::decode(extra, out) == Code::schema);
    for (const auto offset : {0u, 4u, 8u, 12u, 72u, 76u, 80u, 88u, 95u}) {
        auto bad = w; bad[offset] = std::byte{0xff};
        CHECK(cil::decode(bad, out) == Code::schema);
    }
    auto invalid = r; invalid.issued_ns = 0; CHECK(cil::encode(invalid, w) == Code::invalid);
    invalid = r; invalid.expiry_ns = 999; CHECK(cil::encode(invalid, w) == Code::invalid);
    invalid = r; invalid.expiry_ns = UINT64_MAX; CHECK(cil::encode(invalid, w) == Code::invalid);
    invalid = r; invalid.value = 257; CHECK(cil::encode(invalid, w) == Code::invalid);
    invalid = r; invalid.sequence = UINT64_MAX; CHECK(cil::encode(invalid, w) == Code::exhausted);
    invalid = r; invalid.generation = 0; CHECK(cil::encode(invalid, w) == Code::invalid);
    invalid = r; invalid.sequence = 0; CHECK(cil::encode(invalid, w) == Code::invalid);
    invalid = r; invalid.correlation = 0; CHECK(cil::encode(invalid, w) == Code::correlation);
    CHECK(cil::encode(r, w) == Code::ok);
    cil::Sequence sequence{9}; cil::Record plant{cil::Kind::plant, r.generation, 7, 1000, 2000, 0, 6, 0};
    CHECK(cil::accept(w, cil::Kind::command, r.generation + 1, sequence, 1000, out, &plant) == Code::generation && sequence.next == 9);
    CHECK(cil::accept(w, cil::Kind::command, r.generation, sequence, 999, out, &plant) == Code::future && sequence.next == 9);
    CHECK(cil::accept(w, cil::Kind::command, r.generation, sequence, 2000, out, &plant) == Code::expired && sequence.next == 9);
    ++plant.sequence; CHECK(cil::accept(w, cil::Kind::command, r.generation, sequence, 1500, out, &plant) == Code::correlation);
    --plant.sequence; ++plant.issued_ns; CHECK(cil::accept(w, cil::Kind::command, r.generation, sequence, 1500, out, &plant) == Code::correlation);
    --plant.issued_ns; ++plant.expiry_ns; CHECK(cil::accept(w, cil::Kind::command, r.generation, sequence, 1500, out, &plant) == Code::correlation);
    --plant.expiry_ns; ++plant.frame; CHECK(cil::accept(w, cil::Kind::command, r.generation, sequence, 1500, out, &plant) == Code::correlation);
    --plant.frame; CHECK(cil::accept(w, cil::Kind::command, r.generation, sequence, 1500, out, &plant) == Code::ok && sequence.next == 10);
    CHECK(cil::accept(w, cil::Kind::command, r.generation, sequence, 1500, out, &plant) == Code::sequence);
    sequence.next = 8; CHECK(cil::accept(w, cil::Kind::command, r.generation, sequence, 1500, out, &plant) == Code::sequence);
    sequence.next = UINT64_MAX; CHECK(sequence.inspect(UINT64_MAX) == Code::exhausted);
    return true;
}
bool channel() {
    cil::Region r{}; CHECK(cil::inspect(r, 1) == Code::not_ready);
    CHECK(cil::initialize(r, 0) == Code::invalid);
    CHECK(cil::initialize(r, 1) == Code::ok);
    CHECK(cil::initialize(r, 1) == Code::invalid);
    CHECK(cil::inspect(r, 2) == Code::generation);
    CHECK(cil::attach(r, 1) == Code::ok && cil::attach(r, 1) == Code::attached);
    auto header = r.header; r.header[8] = std::byte{0xff}; CHECK(cil::inspect(r, 1) == Code::schema); r.header = header;
    cil::Wire a{}, b{}, out{}; a.fill(std::byte{0x51}); b.fill(std::byte{0xa3});
    CHECK(cil::pop(r.plants, out) == Code::empty);
    CHECK(cil::push(r.plants, a) == Code::ok && cil::push(r.plants, b) == Code::full);
    CHECK(cil::pop(r.plants, out) == Code::ok && out == a);
    CHECK(cil::push(r.plants, b) == Code::ok && cil::pop(r.plants, out) == Code::ok && out == b);
    r.commands.bytes = a; CHECK(cil::pop(r.commands, out) == Code::empty); // Unpublished bytes cannot escape.
    cil::store(r.commands.state, 3); CHECK(cil::pop(r.commands, out) == Code::schema && cil::push(r.commands, a) == Code::schema);
    cil::store(r.commands.state, 0);
    cil::Endpoint host{&r, 1, {}, {}, true};
    cil::Record plant{cil::Kind::plant, 1, 1, 1000, 2000, 0, 0, 0};
    CHECK(host.send(plant) == Code::ok && host.sent.next == 2);
    CHECK(host.send(plant) == Code::full && host.sent.next == 2);
    CHECK(cil::pop(r.plants, out) == Code::ok);
    host.sent.next = UINT64_MAX; CHECK(host.send(plant) == Code::exhausted && cil::load(r.plants.state) == 0);
    cil::store(r.stopping, 1); CHECK(host.send(plant) == Code::closed && cil::inspect(r, 1) == Code::closed);
    host.detach(); CHECK(host.receive(1500, plant) == Code::closed);
    return true;
}
bool concurrency_and_allocation() {
    cil::Slot slot{};
    constexpr std::uint64_t total = 10000;
    std::atomic<bool> failed{false}; std::uint64_t produced = 0, consumed = 0;
    std::thread producer([&] {
        for (std::uint64_t attempt = 0; attempt < 5'000'000 && produced < total; ++attempt) {
            cil::Wire w{}; cil::put(w, 0, produced + 1, 8);
            for (std::size_t i = 8; i < w.size(); ++i) w[i] = static_cast<std::byte>((produced + i) & 255);
            const auto c = cil::push(slot, w);
            if (c == Code::ok) ++produced;
            else if (c != Code::full) { failed.store(true); break; }
            else std::this_thread::yield();
        }
    });
    std::thread consumer([&] {
        for (std::uint64_t attempt = 0; attempt < 5'000'000 && consumed < total; ++attempt) {
            cil::Wire w{}; const auto c = cil::pop(slot, w);
            if (c == Code::empty) { std::this_thread::yield(); continue; }
            if (c != Code::ok || cil::get(w, 0, 8) != consumed + 1) { failed.store(true); break; }
            for (std::size_t i = 8; i < w.size(); ++i)
                if (w[i] != static_cast<std::byte>((consumed + i) & 255)) failed.store(true);
            ++consumed;
        }
    });
    producer.join(); consumer.join(); CHECK(!failed.load() && produced == total && consumed == total);
    bool success = true; auto record = sample(); cil::Wire w{}, out{};
    rtfw_physics_allocation::begin();
    for (unsigned i = 0; i < 1000; ++i) {
        cil::Record decoded;
        success = success && cil::encode(record, w) == Code::ok && cil::push(slot, w) == Code::ok &&
            cil::pop(slot, out) == Code::ok && cil::decode(out, decoded) == Code::ok && decoded.value == -3;
    }
    const auto allocated = rtfw_physics_allocation::end(); CHECK(success && allocated == 0);
    return true;
}
std::string unique_key() {
    static unsigned counter = 0;
#if defined(_WIN32)
    const auto pid = GetCurrentProcessId();
#else
    const auto pid = getpid();
#endif
    return "unit_" + std::to_string(pid) + "_" + std::to_string(cil::now_ns()) + "_" + std::to_string(++counter);
}
bool mappings() {
    CHECK(!cil::valid_key("bad/name") && !cil::valid_key("") && !cil::valid_key(std::string(49, 'x')));
    cil::Mapping owner, peer, duplicate;
    const auto key = unique_key();
    CHECK(owner.open(key, true) == Code::ok && owner.owns_resources());
    CHECK(peer.open(key, false) == Code::ok);
    CHECK(cil::inspect(*peer.region(), 1) == Code::not_ready);
    CHECK(cil::initialize(*owner.region(), 1) == Code::ok);
    CHECK(cil::attach(*peer.region(), 2) == Code::generation);
    CHECK(cil::attach(*peer.region(), 1) == Code::ok);
    CHECK(duplicate.open(key, true) == Code::exists);
    CHECK(duplicate.close() == Code::ok);
    CHECK(duplicate.open(key, false) == Code::ok);
    CHECK(cil::attach(*duplicate.region(), 1) == Code::attached);
    CHECK(duplicate.close() == Code::ok);
    auto* held = owner.region(); owner.fail_next_close_for_test();
    CHECK(owner.close() == Code::io && owner.owns_resources() && owner.region() == held);
    cil::store(owner.region()->stopping, 1);
    CHECK(owner.close() == Code::ok && !owner.owns_resources());
    CHECK(cil::inspect(*peer.region(), 1) == Code::closed); // Peer view independently owns physical lifetime.
    cil::Mapping fresh;
    CHECK(fresh.open(unique_key(), true) == Code::ok && cil::initialize(*fresh.region(), 2) == Code::ok);
    cil::Wire old{}; old.fill(std::byte{0x37}); CHECK(cil::push(peer.region()->commands, old) == Code::ok);
    CHECK(cil::load(fresh.region()->commands.state) == 0 && fresh.region()->commands.bytes[0] == std::byte{0});
    CHECK(peer.close() == Code::ok && fresh.close() == Code::ok);
    CHECK(duplicate.open(key, false) == Code::not_found && duplicate.close() == Code::ok);
#if !defined(_WIN32)
    // A separately created wrong-sized OS object must never be mapped as Region.
    const auto small_key = unique_key(); const auto native = "/rtfw_cil_" + small_key;
    const int fd = shm_open(native.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600); CHECK(fd >= 0);
    CHECK(ftruncate(fd, 128) == 0);
    CHECK(duplicate.open(small_key, false) == Code::schema && duplicate.close() == Code::ok);
    CHECK(::close(fd) == 0 && shm_unlink(native.c_str()) == 0);
#endif
    return true;
}
bool runtime() {
    cil::Region r{}; CHECK(cil::initialize(r, 71) == Code::ok);
    cil::Plant plant; plant.endpoint = {&r, 71, {}, {}, true}; plant.ttl = 1000; plant.now = 1000;
    rt::Runtime runtime; CHECK(cil::configure(runtime, plant) == rt::Status::ok);
    rt::sdk::CheckedStopGuard stop(runtime); CHECK(runtime.start() == rt::Status::ok);
    CHECK(runtime.step({0, std::chrono::milliseconds(1)}) == rt::Status::ok);
    cil::Wire w; cil::Record output;
    CHECK(cil::pop(r.plants, w) == Code::ok && cil::decode(w, output) == Code::ok && output.value == 0);
    bool success = true; rtfw_physics_allocation::begin();
    for (std::uint64_t i = 1; i <= 8; ++i) {
        cil::Record command{cil::Kind::command, 71, i, output.issued_ns, output.expiry_ns,
                            output.sequence, output.frame, (256 - output.value) / 2};
        success = success && cil::encode(command, w) == Code::ok && cil::push(r.commands, w) == Code::ok;
        plant.now = 1000 + i;
        success = success && runtime.step({i, std::chrono::milliseconds(1)}) == rt::Status::ok &&
            plant.status == Code::ok && cil::pop(r.plants, w) == Code::ok && cil::decode(w, output) == Code::ok;
    }
    const auto allocations = rtfw_physics_allocation::end();
    CHECK(success && allocations == 0 && plant.position == 255 && plant.applied == 8);
    CHECK((plant.history == std::array<std::int64_t, 8>{128,192,224,240,248,252,254,255}));
    CHECK(stop.close() == rt::Status::ok); plant.endpoint.detach();
    // Pure callback boundary oracle: full output never consumes input or advances state.
    cil::Plant blocked; blocked.bootstrap = false; blocked.endpoint = {&r, 71, {}, {}, true};
    CHECK(cil::push(r.plants, w) == Code::ok && cil::push(r.commands, w) == Code::ok);
    rt::Runtime blocked_runtime;
    CHECK(cil::configure(blocked_runtime, blocked) == rt::Status::ok);
    rt::sdk::CheckedStopGuard blocked_stop(blocked_runtime);
    CHECK(blocked_runtime.start() == rt::Status::ok);
    CHECK(blocked_runtime.step({0, std::chrono::milliseconds(1)}) == rt::Status::ok);
    CHECK(blocked.status == Code::full && blocked.applied == 0 && blocked.position == 0 && cil::load(r.commands.state) == 1);
    CHECK(blocked_stop.close() == rt::Status::ok); blocked.endpoint.detach();
    return true;
}
bool runtime_rejection_boundaries() {
    for (unsigned kind = 0; kind < 4; ++kind) {
        cil::Region r{}; CHECK(cil::initialize(r, 81) == Code::ok);
        cil::Plant plant; plant.endpoint = {&r, 81, {}, {}, true}; plant.ttl = 1000; plant.now = 1000;
        rt::Runtime runtime; CHECK(cil::configure(runtime, plant) == rt::Status::ok);
        rt::sdk::CheckedStopGuard stop(runtime); CHECK(runtime.start() == rt::Status::ok);
        CHECK(runtime.step({0, std::chrono::milliseconds(1)}) == rt::Status::ok);
        cil::Wire w; cil::Record published;
        CHECK(cil::pop(r.plants, w) == Code::ok && cil::decode(w, published) == Code::ok);
        cil::Record command{cil::Kind::command, 81, 1, 1000, 2000, 1, 0, 128};
        Code expected = Code::expired;
        plant.now = 2000;
        if (kind == 1) { plant.now = 999; expected = Code::future; }
        if (kind == 2) { plant.now = 1500; command.generation = 80; expected = Code::generation; }
        if (kind == 3) { plant.now = 1500; command.sequence = 2; expected = Code::sequence; }
        CHECK(cil::encode(command, w) == Code::ok && cil::push(r.commands, w) == Code::ok);
        CHECK(runtime.step({1, std::chrono::milliseconds(1)}) == rt::Status::ok);
        CHECK(plant.status == expected && plant.position == 0 && plant.applied == 0 && plant.effort == 0);
        CHECK(plant.endpoint.received.next == 1 && cil::load(r.commands.state) == 0 && cil::load(r.plants.state) == 0);
        CHECK(stop.close() == rt::Status::ok); plant.endpoint.detach();
    }
    return true;
}
int main() {
    if (!wire() || !channel() || !concurrency_and_allocation() || !mappings() || !runtime() || !runtime_rejection_boundaries()) return 1;
    std::cout << "CIL wire, channel, mapping, Runtime, ownership and allocation passed\n";
}
