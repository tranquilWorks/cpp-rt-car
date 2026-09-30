#include "conformance.hpp"
#include "runtime_example.hpp"
#include <iostream>

int main() {
    backend_kit::Backend tested;
    backend_kit::Storage storage;
    backend_kit::Profile profile{backend_kit::copy_opcode, &tested,
        [](void* p, backend_kit::Fault f) noexcept { static_cast<backend_kit::Backend*>(p)->inject(f); },
        [](void* p) noexcept { return static_cast<backend_kit::Backend*>(p)->owns_setup(); },
        [](void* p) noexcept { return static_cast<backend_kit::Backend*>(p)->registered(); }};
    const auto report = backend_kit::run(tested.api(), profile, storage);
    if (!report.passed()) {
        for (std::size_t i = 0; i < report.count; ++i)
            if (!report.checks[i].passed) std::cerr << report.checks[i].name << '\n';
        // Never unwind borrowed storage if a substituted backend retains ownership.
        if (!report.cleanup_complete) std::terminate();
        return 1;
    }
    backend_kit::Backend native;
    rt::MockDeviceBackend legacy({1, 1, 1, 1000});
    backend_kit::RuntimeState state;
    rt::Runtime runtime;
    auto check = [&](rt::Status status) {
        if (status == rt::Status::ok) return true;
        std::cerr << runtime.last_error() << '\n'; return false;
    };
    if (!check(backend_kit::configure(runtime, native, legacy, state))) return 1;
    rt::sdk::CheckedStopGuard stop(runtime);
    bool ok = check(runtime.start());
    for (std::uint64_t i = 0; ok && i < 3; ++i)
        ok = check(runtime.step({i, std::chrono::milliseconds(1)}));
    const bool closed = check(stop.close());
    if (!ok || !closed || state.verified != 3 || native.owns_setup() || native.registered() != 0) return 1;
    std::cout << "backend_authoring: conformance=ok native=3 v1=3 verified=3 ownership=0\n";
}
