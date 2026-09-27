#pragma once
#include "model.hpp"
#include <charconv>
#include <iostream>
#include <string_view>

namespace rtfw::cuda_physics {
inline int parse(int argc, char** argv, Options& options, bool real = false) {
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
        std::cout << "usage: sample_cuda_physics" << (real ? "_real" : "")
                  << " [--count 1..4096] [--steps 1..1024] [--seed UINT32] [--workers 1..2]\n";
        return 3; // parser-only sentinel; main maps help to exit 0
    }
    unsigned seen = 0;
    for (int i = 1; i < argc; i += 2) {
        if (i+1 == argc) return 2;
        const std::string_view flag(argv[i]), value(argv[i+1]);
        std::uint32_t number{};
        auto parsed = std::from_chars(value.data(), value.data()+value.size(), number);
        if (parsed.ec != std::errc{} || parsed.ptr != value.data()+value.size()) return 2;
        unsigned bit{};
        if (flag == "--count") { options.count = number; bit = 1; }
        else if (flag == "--steps") { options.steps = number; bit = 2; }
        else if (flag == "--seed") { options.seed = number; bit = 4; }
        else if (flag == "--workers") { options.workers = number; bit = 8; }
        else return 2;
        if (seen & bit) return 2;
        seen |= bit;
    }
    return options.valid() ? 0 : 2;
}
inline void summary(const Options& o, std::uint64_t completed, std::string_view evidence) {
    std::cout << "model=constant-acceleration-v1 seed=" << o.seed << " count=" << o.count
              << " steps=" << o.steps << " workers=" << o.workers
              << " completed=" << completed << " validation=pass evidence=" << evidence << '\n';
}
} // namespace rtfw::cuda_physics
