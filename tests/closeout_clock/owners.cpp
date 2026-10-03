#include <simcore/SimCore.hpp>
#include <array>
#include <barrier>
#include <cstdio>
#include <thread>
#include <vector>

int main() {
    std::barrier stage(2);
    std::array<bool, 2> okay{true, true};
    const auto run = [&](std::size_t index) {
        for (unsigned iteration = 0; iteration < 3; ++iteration) {
            stage.arrive_and_wait();
            {
                SimCore::Settings settings;
                settings.threads = index == 0 ? 2 : 4;
                settings.maxFrames = 16; settings.hz = 1000;
                settings.autoTuneChunks = false; settings.chunkSize = 127;
                settings.rateGovernorEnable = false; settings.driftLogInterval = 0;
                settings.useFMA = false;
                // No fixture construction mutex: owners construct, execute and
                // destruct on their own caller threads, preserving arena binding.
                SimCore owner(settings);
                const double initial = static_cast<double>(index * 100 + iteration);
                std::vector<double> values(1025, initial);
                const auto phase = owner.addPhase("increment", values.size());
                owner.addParallelRangeTask(phase, [&](std::size_t begin, std::size_t end,
                                                     std::int64_t, SimCore::Seconds) {
                    for (auto i = begin; i < end; ++i) values[i] += 1;
                });
                stage.arrive_and_wait();
                owner.run();
                for (double value : values) okay[index] = okay[index] && value == initial + 16;
                stage.arrive_and_wait();
            }
            stage.arrive_and_wait();
        }
    };
    std::thread first(run, 0), second(run, 1); first.join(); second.join();
    if (!okay[0] || !okay[1]) return 1;
    std::puts("PASS concurrent caller-thread owner construction/run/destruction, three cycles, exact independent outputs");
}
