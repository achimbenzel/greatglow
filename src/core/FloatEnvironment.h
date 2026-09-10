#pragma once

#if defined(_MSC_VER)
#include <float.h>
#elif defined(__SSE2__) || defined(_M_X64) || defined(__x86_64__)
#include <xmmintrin.h>
#define ABGLOW_HAS_MXCSR 1
#endif

namespace abglow {

// A glow's tails decay towards zero, and denormal arithmetic there can cost an
// order of magnitude. Flushing denormals for the duration of a render pass
// avoids that; the previous mode is restored so the host is unaffected.
class ScopedFlushDenormals {
public:
    ScopedFlushDenormals() {
#if defined(_MSC_VER)
        unsigned int previous = 0;
        if (_controlfp_s(&previous, _DN_FLUSH, _MCW_DN) == 0) {
            previous_ = previous;
            active_ = true;
        }
#elif defined(ABGLOW_HAS_MXCSR)
        previous_ = _mm_getcsr();
        _mm_setcsr(previous_ | 0x8040u);  // FTZ | DAZ
        active_ = true;
#endif
    }

    ~ScopedFlushDenormals() {
        if (!active_) return;
#if defined(_MSC_VER)
        unsigned int scratch = 0;
        _controlfp_s(&scratch, previous_, _MCW_DN);
#elif defined(ABGLOW_HAS_MXCSR)
        _mm_setcsr(previous_);
#endif
    }

    ScopedFlushDenormals(const ScopedFlushDenormals&) = delete;
    ScopedFlushDenormals& operator=(const ScopedFlushDenormals&) = delete;

private:
    unsigned int previous_ = 0;
    bool active_ = false;
};

}  // namespace abglow
