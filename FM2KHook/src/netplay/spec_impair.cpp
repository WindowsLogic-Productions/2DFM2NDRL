// Spectator-downlink impairment -- test-only packet loss. See spec_impair.h.

#include "spec_impair.h"

#include <cstdlib>
#include <cstdint>

#include <SDL3/SDL_log.h>

namespace fm2k::specimpair {
namespace {

struct Cfg {
    bool     enabled = false;
    double   drop    = 0.0;                       // 0..1 per-datagram drop chance
    uint64_t seed    = 0x9E3779B97F4A7C15ull;     // golden-ratio default
};

Cfg LoadCfg() {
    Cfg c;
    if (const char* d = std::getenv("FM2K_SPEC_DROP"); d && d[0]) {
        c.drop = std::atof(d);
        if (c.drop < 0.0) c.drop = 0.0;
        if (c.drop > 1.0) c.drop = 1.0;
        if (c.drop > 0.0) c.enabled = true;
    }
    if (const char* s = std::getenv("FM2K_SPEC_DROP_SEED"); s && s[0]) {
        const uint64_t v = std::strtoull(s, nullptr, 0);
        if (v != 0) c.seed = v;
    }
    return c;
}

const Cfg& cfg() {
    static const Cfg c = LoadCfg();
    return c;
}

// xorshift64* -- deterministic, self-contained (no <random>/locale/global
// state that could vary the drop pattern run-to-run).
uint64_t       g_rng    = 0;
unsigned long  g_seen   = 0;   // candidate datagrams classified
unsigned long  g_dropped = 0;  // of those, discarded

double NextUnit() {
    if (g_rng == 0) g_rng = cfg().seed;
    uint64_t x = g_rng;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    g_rng = x;
    const uint64_t r = x * 0x2545F4914F6CDD1Dull;
    // top 53 bits -> [0,1)
    return static_cast<double>(r >> 11) * (1.0 / 9007199254740992.0);
}

}  // namespace

bool Enabled() { return cfg().enabled; }

bool ShouldDropUdpInput() {
    if (!cfg().enabled) return false;
    ++g_seen;
    const bool drop = NextUnit() < cfg().drop;
    if (drop) {
        ++g_dropped;
        // Periodic summary -- per-drop spam would flood under 15% loss.
        if ((g_dropped % 25u) == 0u) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[SPEC-IMPAIR] dropped %lu/%lu UDP_INPUT_BATCH (drop=%.2f seed=0x%llX)",
                g_dropped, g_seen, cfg().drop,
                static_cast<unsigned long long>(cfg().seed));
        }
    }
    return drop;
}

}  // namespace fm2k::specimpair
