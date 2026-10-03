#pragma once
#include <atomic>
#include <cstdint>
#include <cmath>
#include <limits>
#include <chrono>
#include <thread>
#include <functional>
#include <mutex>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

// High resolution monotonic clock with optional TSC backing.
// Calibrates TSC frequency at init and falls back to std::chrono on drift.
class HighResClock {
public:
    using tsc_reader_t = uint64_t (*)();

    // Initialise off the read/RT path. Readers continue using the previous
    // complete calibration while a writer calibrates. Injected callbacks must
    // remain valid and support concurrent calls when the clock is used concurrently.
    static void init(tsc_reader_t reader = nullptr) {
        std::lock_guard<std::mutex> writer(initialization_);
        const auto selected = reader ? reader : &default_tsc_reader;
        const uint64_t t0 = selected();
        const auto ref0 = std::chrono::steady_clock::now();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const uint64_t t1 = selected();
        const auto ref1 = std::chrono::steady_clock::now();
        const double secs = std::chrono::duration<double>(ref1 - ref0).count();
        const double frequency = secs > 0.0 && t1 > t0
            ? static_cast<double>(t1 - t0) / secs : 0.0;
        publish(selected, frequency, t1, to_ns(ref1),
                std::isfinite(frequency) && frequency > 0.0);
        // Reinitialization must not reset a floor already observed by readers.
        (void)ensure_monotonic(to_ns(ref1));
    }

    // Nanoseconds since an arbitrary epoch (monotonic). A single snapshot
    // attempt uses atomic fields; concurrent publication falls back immediately.
    // No writer mutex, allocation, or publication retry loop on this path.
    static uint64_t now() {
        const uint64_t ns_ref = to_ns(std::chrono::steady_clock::now());
        const uint64_t generation = publication_.load();
        if ((generation & 1u) != 0 || generation == 0 ||
            enabled_generation_.load() != generation)
            return ensure_monotonic(ns_ref);
        const auto reader = tsc_reader_.load();
        const double frequency = tsc_freq_.load();
        const uint64_t start = tsc_start_.load();
        const uint64_t reference = ref_start_ns_.load();
        if (publication_.load() != generation)
            return ensure_monotonic(ns_ref);
        if (!reader || !std::isfinite(frequency) || frequency <= 0.0) {
            disable(generation);
            return ensure_monotonic(ns_ref);
        }
        const uint64_t t = reader();
        // A callback may overlap a replacement. Its sample cannot disable or
        // publish a timestamp using the newly selected calibration.
        if (publication_.load() != generation ||
            enabled_generation_.load() != generation)
            return ensure_monotonic(ns_ref);
        if (t < start) {
            disable(generation);
            return ensure_monotonic(ns_ref);
        }
        const double elapsed = static_cast<double>(t - start) * 1e9 / frequency;
        if (!std::isfinite(elapsed) || elapsed >= static_cast<double>(
                std::numeric_limits<uint64_t>::max() - reference)) {
            disable(generation);
            return ensure_monotonic(ns_ref);
        }
        const uint64_t ns = reference + static_cast<uint64_t>(elapsed);
        // Preserve the existing 1ms bound before every floor publication.
        const uint64_t difference = ns > ns_ref ? ns - ns_ref : ns_ref - ns;
        if (difference > 1000000) {
            disable(generation);
            return ensure_monotonic(ns_ref);
        }
        return ensure_monotonic(ns);
    }

    // Helper: convert nanoseconds to milliseconds.
    static double to_ms(uint64_t ns) {
        return static_cast<double>(ns) / 1'000'000.0;
    }

    static bool using_tsc() {
        const uint64_t generation = publication_.load();
        return generation != 0 && (generation & 1u) == 0 &&
               enabled_generation_.load() == generation &&
               publication_.load() == generation;
    }
    // Preserve reader replacement without recalibration; the existing drift
    // validation decides whether the replacement matches the current scale.
    static void set_tsc_reader(tsc_reader_t reader) {
        std::lock_guard<std::mutex> writer(initialization_);
        publish(reader, tsc_freq_.load(), tsc_start_.load(), ref_start_ns_.load(),
                using_tsc() && reader != nullptr);
    }

private:
    static void disable(uint64_t generation) {
        // A delayed failed read cannot disable a later successful initialization.
        (void)enabled_generation_.compare_exchange_strong(generation, 0);
    }
    static void publish(tsc_reader_t reader, double frequency, uint64_t start,
                        uint64_t reference, bool enabled) {
        const uint64_t previous = publication_.load();
        // Refuse generation reuse rather than permitting an ABA on exhaustion.
        if (previous > std::numeric_limits<uint64_t>::max() - 2) {
            enabled_generation_.store(0);
            return;
        }
        // All sequence and field operations are sequentially consistent. Equal
        // even sequence reads bracket one complete publication; fields remain
        // atomic even when a reader observes a concurrent writer and falls back.
        publication_.store(previous + 1);
        tsc_reader_.store(reader);
        tsc_freq_.store(frequency);
        tsc_start_.store(start);
        ref_start_ns_.store(reference);
        enabled_generation_.store(enabled ? previous + 2 : 0);
        publication_.store(previous + 2);
    }

    static uint64_t to_ns(std::chrono::steady_clock::time_point tp) {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(tp.time_since_epoch()).count());
    }
    static uint64_t ensure_monotonic(uint64_t ns) {
        uint64_t last = last_ns_.load();
        for (;;) {
            const uint64_t next = ns > last ? ns : last + 1;
            if (last_ns_.compare_exchange_weak(last, next))
                return next;
        }
    }

    // Default TSC reader (x86); returns 0 on unsupported platforms.
    static uint64_t default_tsc_reader() {
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
        return __rdtsc();
#elif defined(__i386__) || defined(__x86_64__)
        unsigned int lo, hi;
        __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
        return (static_cast<uint64_t>(hi) << 32) | lo;
#else
        return 0;
#endif
    }

    static_assert(std::atomic<tsc_reader_t>::is_always_lock_free);
    static_assert(std::atomic<double>::is_always_lock_free);
    static_assert(std::atomic<uint64_t>::is_always_lock_free);
    static inline std::mutex initialization_;
    static inline std::atomic<uint64_t> publication_{0};
    static inline std::atomic<uint64_t> enabled_generation_{0};
    static inline std::atomic<tsc_reader_t> tsc_reader_{nullptr};
    static inline std::atomic<double> tsc_freq_{0.0};
    static inline std::atomic<uint64_t> tsc_start_{0};
    static inline std::atomic<uint64_t> ref_start_ns_{0};
    static inline std::atomic<uint64_t> last_ns_{0};
};
