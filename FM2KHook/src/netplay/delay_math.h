// Pure delay-sizing math for the input-delay negotiation (#24).
//
// Kept dependency-free (no winsock / SDL / project headers) so the
// exact code that runs in the hook can also be exercised by the
// host-compiled unit test at tests/delay_math_test.cpp.
#pragma once

#include <cstdint>
#include <vector>
#include <algorithm>

namespace fm2k::delay {

// Representative RTT (ms) from a window of raw RTT samples.
//   mode 0 (avg)  -> arithmetic mean of the window.
//   mode 1 (peak) -> p90: sorted, index floor(0.9 * (n-1)). Sizes above
//                    the median but discards the worst ~10%, so a lone
//                    cold-start / scheduler-hiccup spike cannot dominate
//                    the pick (that outlier sensitivity is exactly why
//                    an all-time-max worst-RTT was a bad delay basis).
// Sample order is irrelevant -- both mean and percentile are order-
// independent -- so a circular ring can be passed as-is. Returns 0 when
// the window is empty.
inline uint32_t RttFromWindow(const uint32_t* samples, int n, int mode) {
    if (n <= 0 || samples == nullptr) return 0;
    if (mode != 1) {
        uint64_t sum = 0;
        for (int i = 0; i < n; ++i) sum += samples[i];
        return (uint32_t)(sum / (uint64_t)n);
    }
    std::vector<uint32_t> sorted(samples, samples + n);
    std::sort(sorted.begin(), sorted.end());
    return sorted[(size_t)((n - 1) * 9 / 10)];
}

// Input-delay candidate (frames) from a representative RTT (ms).
// FM2K runs at 100 Hz so 10 ms = 1 frame; delay covers the ONE-WAY
// latency, hence rtt/2. ceil is done via (+9)/10. Clamped to [2, 15].
// Returns -1 when rtt_ms is 0 (no measurement available yet).
inline int DelayCandidateFromRtt(uint32_t rtt_ms) {
    if (rtt_ms == 0) return -1;
    int d = (int)(((rtt_ms / 2) + 9) / 10);
    if (d < 2)  d = 2;
    if (d > 15) d = 15;
    return d;
}

}  // namespace fm2k::delay
