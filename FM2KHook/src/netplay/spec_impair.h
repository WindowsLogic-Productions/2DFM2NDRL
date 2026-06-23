#pragma once

// =============================================================================
// Spectator-downlink impairment -- TEST-ONLY packet loss for the harness.
// =============================================================================
// Reproduces the "spectator gets wrong battle inputs under loss" class WITHOUT
// an OS-level filter (clumsy/WinDivert), which needs admin (it loads a kernel
// driver), is global (would also impair the P1<->P2 gekko link, where loss
// does NOT cause this bug -- just rollback noise), and is stochastic.
//
// This shim lives in our own DLL at the spectator's input-receive site, so it:
//   * needs NO admin -- it's just our code,
//   * targets ONLY the spectator's inbound input transport (UDP
//     UDP_INPUT_BATCH), never the game link,
//   * is DETERMINISTIC -- a seeded Bernoulli drop, so a failing run reproduces
//     exactly and seeds can be swept.
//
// Inert in production: every entry point is a no-op unless FM2K_SPEC_DROP is
// set in the environment. Env:
//   FM2K_SPEC_DROP=0.15        per-datagram drop chance for UDP_INPUT_BATCH (0..1)
//   FM2K_SPEC_DROP_SEED=12345  PRNG seed (default fixed) for reproducible patterns
//
// (RTT/jitter/reorder is a deliberate follow-up: it needs a hold-and-release
// buffer pumped from the recv loop. Drop alone forces the gap-recovery /
// TCP-fallback path, which is the prime repro lever; jitter is added only if
// the diagnosis shows drop is insufficient.)

#include <cstddef>

namespace fm2k::specimpair {

// True when FM2K_SPEC_DROP is configured (>0). Cheap; use to gate logging.
bool Enabled();

// True => the caller should DISCARD this inbound UDP_INPUT_BATCH datagram,
// simulating a lost packet on the spectator's downlink. Advances the seeded
// PRNG, so call it exactly once per candidate datagram. Always false (no PRNG
// advance) when not Enabled().
bool ShouldDropUdpInput();

}  // namespace fm2k::specimpair
