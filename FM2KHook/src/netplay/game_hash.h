#pragma once

#include <cstdint>

// Game-data fingerprint sent in HELLO so peers reject mismatched setups
// up front (#57). Approach (a) — sorted list of all *.player + *.kgt +
// the .exe's stem in the game directory, each entry "lowercase_name|size"
// joined by newlines, hashed with xxhash3-64 truncated to 32 bits.
//
// Catches the common "we have different rosters" desync trigger (different
// .player file count or different filenames). Doesn't catch silent edits
// to existing files — content hashing would, but at the cost of reading
// every .player byte on every connect; that's a v2 toggle if tournament
// strict mode wants it.
//
// Computed once on first call, cached. Returns 0 if the game directory
// can't be enumerated (the receiver treats hash=0 as "older client, skip
// the check" so the rollout is backwards-compatible).

namespace fm2k::game_hash {

uint32_t Compute();

// Friendly debug string ("3 .player + 1 .kgt + game.exe — 0xabcd1234").
// Cached alongside the hash. Empty when Compute() has never been called.
const char* DescribeLocal();

}  // namespace fm2k::game_hash
