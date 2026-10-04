#pragma once
#include <atomic>
#include <cfenv>
#include <cstdint>
#include <cmath>
#if defined(__SSE__)
#include <immintrin.h>
#endif

namespace rt {

namespace detail {

// Build level gate controlling whether FMA may be used.  The default honours
// the compiler target (presence of __FMA__/__AVX2__) but can be overridden via
// RT_NUMERICS_FORCE_FMA / RT_NUMERICS_FORCE_NO_FMA build definitions.
#if defined(RT_NUMERICS_FORCE_NO_FMA)
inline constexpr bool kBuildAllowsFma = false;
#elif defined(RT_NUMERICS_FORCE_FMA)
inline constexpr bool kBuildAllowsFma = true;
#elif defined(__FMA__) || defined(__AVX2__)
inline constexpr bool kBuildAllowsFma = true;
#else
inline constexpr bool kBuildAllowsFma = false;
#endif

} // namespace detail

// Configure floating point environment for determinism.
// Sets rounding mode to FE_TONEAREST and enables flush-to-zero
// and denormals-are-zero on x86 targets. Intended to be called
// once per thread at startup.
inline void init_fp_env() {
    std::fesetround(FE_TONEAREST);
#if defined(__SSE__)
    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
#ifdef _MM_SET_DENORMALS_ZERO_MODE
    _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
#endif
#endif
}

// Experimental process-global policy, not per-SimCore state. Concurrent access
// uses set_use_fma/use_fma/fma; configure policy while arithmetic is quiescent
// for deterministic results. The legacy raw reference requires exclusive access.
static_assert(std::atomic_ref<bool>::is_always_lock_free);
inline bool &use_fma_flag() {
    alignas(std::atomic_ref<bool>::required_alignment)
    static bool flag = detail::kBuildAllowsFma;
    return flag;
}

inline void set_use_fma(bool on) {
    if constexpr (detail::kBuildAllowsFma) {
        std::atomic_ref<bool>(use_fma_flag()).store(on, std::memory_order_relaxed);
    } else {
        (void)on;
    }
}

inline bool use_fma() {
    if constexpr (detail::kBuildAllowsFma) {
        return std::atomic_ref<bool>(use_fma_flag()).load(std::memory_order_relaxed);
    }
    return false;
}

// Wrapper for fused multiply-add with a runtime gate. When the
// gate is disabled the operation is performed as a separate
// multiply and add which mirrors platforms lacking hardware FMA.
inline double fma(double a, double b, double c) {
    if constexpr (detail::kBuildAllowsFma) {
        if (use_fma())
            return std::fma(a, b, c);
    }
    return a * b + c;
}

} // namespace rt

