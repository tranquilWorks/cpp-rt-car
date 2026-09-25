#include "provider.hpp"
#include <iostream>
#include <set>
#include <string>

namespace b = rtfw::benchmark;
namespace {
bool fake_tick(void* user, std::uint64_t& value) {
    auto& tick = *static_cast<std::uint64_t*>(user);
    value = tick;
    tick += 100;
    return true;
}
}
int main(int argc, char** argv) {
    try {
        std::string selected = "transform-64", kind = "fake", destination;
        auto identity = b::capture_identity();
        identity.backend = "host-cpu-example";
        identity.driver = "not-applicable";
        std::set<std::string> options;
        for (int i = 1; i < argc; i += 2) {
            const std::string option = argv[i];
            if (i + 1 >= argc || !options.insert(option).second) return 2;
            const std::string value = argv[i + 1];
            if (option == "--case") selected = value;
            else if (option == "--clock") kind = value;
            else if (option == "--output") destination = value;
            else if (option == "--host-label") identity.host_label = value;
            else if (option == "--thread-policy") identity.thread_policy = value;
            else if (option == "--memory-policy") identity.memory_policy = value;
            else return 2;
        }
        if (destination.empty() || (kind != "fake" && kind != "steady") ||
            b::validate(identity) != b::Status::ok || b::check_destination(destination) != b::Status::ok) return 2;
        example::Provider provider;
        std::uint64_t tick = 0;
        auto clock = b::steady_clock();
        if (kind == "fake") {
            clock.kind = b::ClockKind::fake;
            clock.user = &tick;
            clock.read_ns = fake_tick;
        }
        const auto status = example::run_to_directory(provider, selected, clock, identity, destination);
        // This default owns arrays only. A custom cleanup that fails requires
        // the host to retain provider/user and retry finish before releasing them.
        std::cout << b::status_name(status) << '\n';
        if (status == b::Status::not_found || status == b::Status::invalid) return 2;
        return status == b::Status::ok ? 0 : status == b::Status::not_run ? 3 : 1;
    } catch (...) { return 1; }
}
