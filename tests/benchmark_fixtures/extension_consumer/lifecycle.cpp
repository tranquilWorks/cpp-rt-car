#include "provider.hpp"
#include <atomic>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace b = rtfw::benchmark;
namespace {
void check(bool value) { if (!value) throw std::runtime_error("extension contract failed"); }
bool tick(void* user, std::uint64_t& out) {
    auto& value = *static_cast<std::uint64_t*>(user);
    out = value; value += 10; return true;
}
b::Result run(example::Provider& provider, const char* id = "transform-64") {
    b::Runner runner;
    b::ProviderHandle handle;
    check(runner.register_provider(provider.table(), handle) == b::Status::ok);
    std::uint64_t time = 0;
    b::ClockV1 clock; clock.kind = b::ClockKind::fake; clock.read_ns = tick; clock.user = &time;
    return runner.run("example.transform", id, clock, b::Identity{});
}
struct Callbacks {
    unsigned calls{}, cleanup_calls{};
    bool corrupt{}, fail{}, throws{}, retry{};
    static bool transform(void* user, const std::uint64_t* input, std::uint64_t* output, std::size_t count) {
        auto& c = *static_cast<Callbacks*>(user); ++c.calls;
        if (c.throws) throw std::runtime_error("test callback");
        if (c.fail) return false;
        for (std::size_t i = 0; i < count; ++i) output[i] = 3 * input[i] + 7;
        if (c.corrupt) ++output[count-1];
        return true;
    }
    static bool cleanup(void* user) {
        auto& c = *static_cast<Callbacks*>(user); ++c.cleanup_calls;
        return !c.retry || c.cleanup_calls > 1;
    }
};
void lifecycle() {
    Callbacks callbacks;
    example::Provider provider(&callbacks, Callbacks::transform, Callbacks::cleanup);
    auto table = provider.table();
    b::Descriptor small, large;
    check(table.describe(table.user, 0, small) == b::Status::ok);
    check(table.describe(table.user, 1, large) == b::Status::ok);
    check(table.describe(table.user, 2, large) == b::Status::not_found);
    check(callbacks.calls == 0 && callbacks.cleanup_calls == 0 && !provider.owns_session());
    check(small.parameters[0].value == 64 && large.parameters[0].value == 1024);
    check(provider.prepare("invalid") == b::Status::not_found);
    for (const auto* id : {"transform-64", "transform-1024", "transform-64"}) {
        check(provider.prepare(id) == b::Status::ok);
        check(provider.prepare(id) == b::Status::busy);
        const auto result = run(provider, id);
        check(result.status == b::Status::ok && result.samples.size() == 5 && provider.completed() == 7);
        const std::uint64_t n = std::string_view(id) == "transform-64" ? 64 : 1024;
        for (std::size_t i = 0; i < result.samples.size(); ++i) {
            const auto& sample = result.samples[i];
            check(sample.observation.counters == std::vector<std::uint64_t>({n,n*8}));
            check(sample.observation.checksum == n*(3*(i+2)+7)+3*n*(n-1)/2);
        }
        check(provider.finish() == b::Status::ok && !provider.owns_session());
        check(provider.finish() == b::Status::invalid);
    }
    const auto calls = callbacks.calls;
    check(provider.prepare("transform-64", false) == b::Status::ok);
    auto missing = run(provider);
    check(missing.status == b::Status::not_run && missing.samples.empty() && missing.warmup_completed == 0);
    check(callbacks.calls == calls && provider.finish() == b::Status::ok);
    for (unsigned fault = 0; fault < 3; ++fault) {
        callbacks.corrupt = fault == 0; callbacks.fail = fault == 1; callbacks.throws = fault == 2;
        check(provider.prepare("transform-64") == b::Status::ok);
        const auto failed = run(provider);
        check(failed.status != b::Status::ok && failed.samples.empty() && provider.completed() == 0);
        b::Observation observation;
        check(table.invoke(table.user, "transform-64", 0, observation) == b::Status::provider_error);
        check(provider.finish() == b::Status::ok);
    }
    callbacks.corrupt = callbacks.fail = callbacks.throws = false;
    callbacks.cleanup_calls = 0; callbacks.retry = true;
    check(provider.prepare("transform-64") == b::Status::ok);
    check(run(provider).status == b::Status::ok);
    check(provider.finish() == b::Status::provider_error && provider.owns_session());
    check(provider.prepare("transform-64") == b::Status::busy);
    check(provider.finish() == b::Status::ok && !provider.owns_session());
    check(provider.prepare("transform-64") == b::Status::ok);
    b::Observation observation;
    check(table.invoke(table.user, "transform-64", 1, observation) == b::Status::provider_error);
    check(provider.finish() == b::Status::ok);
}
void publication() {
    const auto root = std::filesystem::temp_directory_path() /
        ("rtfw-extension-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    check(std::filesystem::create_directory(root));
    Callbacks callbacks;
    example::Provider provider(&callbacks, Callbacks::transform, Callbacks::cleanup);
    std::uint64_t time = 0;
    b::ClockV1 clock; clock.kind = b::ClockKind::fake; clock.user = &time; clock.read_ns = tick;
    const auto output = root / "result";
    callbacks.corrupt = true;
    check(example::run_to_directory(provider,"transform-64",clock,b::Identity{},output) != b::Status::ok);
    check(!std::filesystem::exists(output) && !provider.owns_session());
    callbacks.corrupt = false; callbacks.retry = true; callbacks.cleanup_calls = 0;
    check(example::run_to_directory(provider,"transform-64",clock,b::Identity{},output) == b::Status::provider_error);
    check(!std::filesystem::exists(output) && provider.owns_session());
    b::Observation observation; auto table = provider.table();
    check(table.invoke(table.user,"transform-64",7,observation) == b::Status::provider_error);
    check(provider.finish() == b::Status::ok);
    check(example::run_to_directory(provider,"transform-64",clock,b::Identity{},output) == b::Status::ok);
    check(std::filesystem::exists(output/"result.json"));
    const auto calls = callbacks.calls;
    check(example::run_to_directory(provider,"transform-64",clock,b::Identity{},output) == b::Status::exists);
    check(callbacks.calls == calls);
    check(example::run_to_directory(provider,"transform-64",clock,b::Identity{},root/"missing",false) == b::Status::not_run);
    check(std::filesystem::exists(root/"missing/result.json"));
    std::filesystem::remove_all(root);
}
}
int main() {
    try {
        lifecycle();
        publication();
        std::atomic<unsigned> failures{0};
        auto work = [&] {
            try { for (unsigned i = 0; i < 16; ++i) lifecycle(); }
            catch (...) { ++failures; }
        };
        std::thread first(work), second(work);
        first.join(); second.join();
        check(failures.load() == 0);
        std::cout << "Extension metadata, oracles, repeated/fault/cleanup and independent-owner checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
