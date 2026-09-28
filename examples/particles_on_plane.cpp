// Legacy research experiment; not the Runtime SDK entry path. See README.md.
#include <simcore/SimCore.hpp>
#include <simcore/worker_pool.hpp>
#include <simcore/soa/aosoa.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iostream>
#include <latch>
#include <span>
#include <thread>
#include <type_traits>
#include <vector>

namespace {

constexpr std::size_t kCacheLineSize = 64;
constexpr std::size_t kMinParallelBytes = 1u << 15; // 32 KiB

constexpr std::size_t align_up(std::size_t value, std::size_t alignment) {
    return ((value + alignment - 1) / alignment) * alignment;
}

template <typename T>
void parallel_first_touch(std::span<T> buffer, WorkerPool &pool) {
    static_assert(!std::is_const_v<T>, "parallel_first_touch requires mutable data");
    static_assert(std::is_trivially_copyable_v<T>,
                  "parallel_first_touch requires trivially copyable elements");

    const std::size_t totalBytes = buffer.size_bytes();
    if (totalBytes == 0 || totalBytes < kMinParallelBytes || pool.thread_count() <= 1) {
        return;
    }

    const std::size_t threads = pool.thread_count();
    const std::size_t chunkBytes = std::max<std::size_t>(
        kCacheLineSize, align_up((totalBytes + threads - 1) / threads, kCacheLineSize));
    const std::size_t taskCount = (totalBytes + chunkBytes - 1) / chunkBytes;

    std::latch done(taskCount);
    auto *base = reinterpret_cast<std::byte *>(buffer.data());

    for (std::size_t task = 0; task < taskCount; ++task) {
        const std::size_t begin = task * chunkBytes;
        const std::size_t end = std::min(totalBytes, begin + chunkBytes);
        pool.enqueue([base, begin, end, &done]() {
            auto *data = reinterpret_cast<volatile std::byte *>(base);
            if (end > begin) {
                for (std::size_t offset = begin; offset < end; offset += kCacheLineSize) {
                    data[offset] = std::byte{0};
                }
                data[end - 1] = std::byte{0};
            }
            done.count_down();
        });
    }

    done.wait();
}

} // namespace

int main() {
    using ParticleAoSoA = soa::Vec3AoSoA<double, 256>;
    constexpr std::size_t kParticleCount = 8192;
    const std::size_t tileCount = (kParticleCount + ParticleAoSoA::tile_size - 1) / ParticleAoSoA::tile_size;

    unsigned hw = std::thread::hardware_concurrency();
    if (hw < 2)
        hw = 2;
    SimCore::Settings settings;
    settings.hz = 120.0;
    settings.maxFrames = 240;
    settings.threads = hw;
    settings.autoTuneChunks = false;
    settings.chunkSize = 4;

    WorkerPool pool(settings.threads, 2048);

    std::vector<ParticleAoSoA::Tile> positionTiles(tileCount);
    std::vector<ParticleAoSoA::Tile> velocityTiles(tileCount);

    parallel_first_touch(std::span{positionTiles}, pool);
    parallel_first_touch(std::span{velocityTiles}, pool);

    for (auto &tile : positionTiles) {
        std::fill_n(tile.x, ParticleAoSoA::tile_size, 0.0);
        std::fill_n(tile.y, ParticleAoSoA::tile_size, 0.0);
        std::fill_n(tile.z, ParticleAoSoA::tile_size, 0.0);
    }
    for (auto &tile : velocityTiles) {
        std::fill_n(tile.x, ParticleAoSoA::tile_size, 0.0);
        std::fill_n(tile.y, ParticleAoSoA::tile_size, 0.0);
        std::fill_n(tile.z, ParticleAoSoA::tile_size, 0.0);
    }

    ParticleAoSoA positions{positionTiles.data()};
    ParticleAoSoA velocities{velocityTiles.data()};

    for (std::size_t idx = 0; idx < kParticleCount; ++idx) {
        const std::size_t tile = idx / ParticleAoSoA::tile_size;
        const std::size_t lane = idx % ParticleAoSoA::tile_size;
        positions.tiles[tile].x[lane] = static_cast<double>(idx) * 0.02;
        positions.tiles[tile].y[lane] = 10.0;
        positions.tiles[tile].z[lane] = 0.0;
        velocities.tiles[tile].x[lane] = 1.0;
        velocities.tiles[tile].y[lane] = 0.0;
        velocities.tiles[tile].z[lane] = 0.0;
    }

    SimCore sim(settings);
    sim.setWorkerPool(&pool);

    auto physics = sim.addPhase("AoSoA physics");
    sim.setPhaseElementCount(physics, tileCount);

    constexpr double gravity = -9.81;
    constexpr double restitution = 0.35;

    sim.addParallelRangeTask(
        physics,
        [positions, velocities, gravity, restitution, particleCount = kParticleCount](std::size_t begin, std::size_t end, std::int64_t,
                                                                                      SimCore::Seconds dt) {
            const double g = gravity;
            const double bounce = restitution;
            const double step = dt.count();
            for (std::size_t tileIndex = begin; tileIndex < end; ++tileIndex) {
                auto &posTile = positions.tiles[tileIndex];
                auto &velTile = velocities.tiles[tileIndex];
                for (std::size_t lane = 0; lane < ParticleAoSoA::tile_size; ++lane) {
                    const std::size_t idx = tileIndex * ParticleAoSoA::tile_size + lane;
                    if (idx >= particleCount)
                        break;

                    velTile.y[lane] += g * step;
                    posTile.x[lane] += velTile.x[lane] * step;
                    posTile.y[lane] += velTile.y[lane] * step;
                    posTile.z[lane] += velTile.z[lane] * step;

                    if (posTile.y[lane] < 0.0) {
                        posTile.y[lane] = 0.0;
                        velTile.y[lane] = -velTile.y[lane] * bounce;
                        if (std::abs(velTile.y[lane]) < 0.05)
                            velTile.y[lane] = 0.0;
                    }
                }
            }
        });

    sim.run();
    pool.drain();
    pool.stop();
    sim.setWorkerPool(nullptr);

    double avgHeight = 0.0;
    for (std::size_t idx = 0; idx < kParticleCount; ++idx) {
        const std::size_t tile = idx / ParticleAoSoA::tile_size;
        const std::size_t lane = idx % ParticleAoSoA::tile_size;
        avgHeight += positions.tiles[tile].y[lane];
    }
    avgHeight /= static_cast<double>(kParticleCount);

    std::cout << "Particles settled. First particle y=" << positions.tiles[0].y[0]
              << ", average height=" << avgHeight << "\n";
    return 0;
}
