# Input pipeline architecture

> Determinism contract for online play, spectator streams, replay
> playback, and stress autoplay. Maintained 2026-05-16 alongside the
> replay-self-test harness landing.

## The contract

**For replays and spectators to reproduce a recorded match deterministically, the input value the engine consumes on frame N must equal the input value recorded in the .fm2krep stream for frame N.**

That sounds obvious. It's not, because FM2K's engine reads inputs via
its own `get_player_input` callback (`Hook_GetPlayerInput`) which we
intercept. We can return *any* value from that hook. Multiple sources
have historically supplied that value, and a divergence between "what
the engine consumed" and "what the recorder stored" causes the replay
to desync on frame 0.

## The pipeline

There is **one** pipeline. Sources feed it; sinks read from it.
Anything that bypasses the pipeline breaks determinism.

```
                   ┌─ keyboard / controller (Input_CaptureLocal)
SOURCES            ├─ autoplay (Hook_ComputeAutoplayBattleInput)
                   └─ stress-mode synthetic loopback
                                  │
                                  ▼
                          gekko_add_local_input
                                  │  (gekko handles prediction, rollback,
                                  │   delivery to the right frame)
                                  ▼
                          GekkoAdvanceEvent
                                  │
                                  ▼
                       g_p1_input / g_p2_input   ◄── PIPELINE STATE
                                  │
                ┌─────────────────┼────────────────┐
                ▼                 ▼                ▼
SINKS    engine consumes      .fm2krep         SaveState
         via netplay branch   records via      fingerprint
         of Hook_GetPlayer-   SpectatorNode_   includes
         Input                OnFrameConfirmed  (g_p?_input)
```

**Determinism falls out automatically** if all three sinks read from
the same pipeline state (`g_p1_input` / `g_p2_input`). Bugs creep in
when a sink reads from somewhere else.

## Where the contract was broken (the bug the harness found)

The pre-2026-05-16 autoplay block inside `Hook_GetPlayerInput` (`hooks.cpp:1261+`)
computed autoplay input *directly* from `g_input_buffer_index` and
returned it to the engine without touching `g_p1_input` /
`g_p2_input`. Meanwhile, `gekko_add_local_input` was being fed
`Input_CaptureLocal()` (keyboard), which returns 0 when the launcher
window isn't focused — typical during a `--stress` headless run.

Result:
- Engine sims with autoplay values (`0x001`, `0x009`, ...)
- gekko delivers `(0, 0)` to AdvanceEvent → `g_p?_input = 0`
- `SpectatorNode_OnFrameConfirmed` writes `(0, 0)` to .fm2krep
- Replay re-runs with `(0, 0)` → engine produces a different sim state
- Harness diff: "FIRST DIVERGENCE at frame 0"

## The fix (2026-05-16, in `tools/replay_selftest.py` first run)

Two coordinated changes:

1. **`FM2KHook/src/hooks/hooks.cpp`** — extracted the autoplay-input
   computation into a public `Hook_ComputeAutoplayBattleInput(player_id)`.
   The autoplay block in `Hook_GetPlayerInput` still uses it for the
   offline / pre-gekko path, but **when a netplay session is active
   (`Netplay_IsActive()` is true), the autoplay return short-circuit is
   skipped**. Control falls through to the netplay branch that returns
   `g_p?_input`. This makes the engine consume gekko-delivered input,
   not autoplay-computed input, when gekko is in the loop.
2. **`FM2KHook/src/netplay/netplay.cpp`** — the stress-mode
   `gekko_add_local_input` call now calls `Hook_ComputeAutoplayBattleInput`
   per slot when `FM2K_PARITY_AUTOPLAY_BATTLE` is set. gekko receives
   the autoplay values directly; the engine consumes the same values
   via the netplay branch.

Net effect: engine input == `g_p?_input` == .fm2krep value, on every
frame, in every mode. The harness diff at frame 3 now shows
`p1=0x001 p2=0x00A` on **both** sides (matches exactly).

## What the harness still reveals (task #33)

Inputs match. RNG still diverges starting frame 0:
- Record: pre-sim rng = 0x12345678, post-sim rng = 0xE0A1FCBC
- Replay: pre-sim rng = 0x12345678, post-sim rng = 0xAEA69ED3

Same pre-sim rng + same input → different post-sim rng means
**engine state at battle entry differs between stress-mode boot-to-battle
and spectator/replay-mode boot-to-battle**. Suspects: character data
loading order, object pool initial bytes, render_frame_counter timing
relative to mode=3000 transition. Investigation in progress on task #33.

## How the harness validates the contract

`tools/replay_selftest.py`:

1. Record phase: stress + autoplay, terminates at frame N, writes
   `.fm2krep` + `record.pty` (parity snapshots: rng, inputs,
   match_phase, all 16 game vars + per-player struct).
2. Replay phase: `--replay <record.fm2krep>`, writes `replay.pty`.
3. Diff phase: `parity_diff.py record.pty replay.pty`. First DIFF
   line names the failing field and frame.

Run: `python3 tools/replay_selftest.py --frames 300`. Should report
`ALL ALIGNED FRAMES IDENTICAL` once task #33 lands.

## What does NOT belong in the pipeline

- **Auto-mash** in `Hook_GetPlayerInput` that doesn't write to
  `g_p?_input`. (Removed for battle phase; CSS phase still has its
  own pre-battle nav inputs — those don't go through gekko because
  the session isn't created yet.)
- **Side-channel input** read directly from keyboard inside the
  engine's sim loop, bypassing the netplay frame boundary.
- **Per-source SOCD application** with different modes for record
  vs replay. The host applies once (`Hook_ApplySOCD_Public` before
  `SpectatorNode_OnFrameConfirmed`). Spec re-applies, but on an
  already-resolved input it's a no-op.

## Future sources to plug in

- **Network peer (remote player)**: already plugged in — gekko's
  remote-input delivery feeds `g_p?_input` on AdvEvent.
- **Replay file**: `SpectatorNode_GetCurrentP?Input()` reads from the
  pb_queue. Hook_GetPlayerInput consults this when `g_spectator_mode`.
- **Future: scripted-test input source for unit harnesses** — would
  call `gekko_add_local_input` with predetermined values per frame.
  No engine change needed; the pipeline absorbs it transparently.
