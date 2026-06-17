# Replay support -- status, findings, and the host-only gap

Investigation 2026-06-17 (verified against shipped stable v0.2.71 + bleeding +
real `.fm2krep` files on disk). Prompted by "is replay separated per round?"

## TL;DR

- **Replays are full-MATCH, not per-round.** One `.fm2krep` per match (best-of-N
  rounds), all rounds inside one file. Full replays work.
- **Round-level seek works** -- the v2 header carries a `round_offsets[]` table,
  populated on the **host's** file (confirmed: today's host recordings have the
  round_offsets flag set; guest recordings do not).
- **The real gap: only the HOST writes a local replay.** In a real netplay match
  the guest gets **no** `.fm2krep` at all. A player who mostly accepts challenges
  builds a replay library; one who mostly sends them gets none. This is almost
  certainly the limitation that prompted the question.

## How recording works (current)

- Recorder writes **one file per match**, slicing the SessionEvent log between
  the most recent `MATCH_START` and the just-appended `MATCH_END`. Fires at
  battle exit (game_mode `3000 -> 2000`, i.e. match over -- NOT between rounds).
  `FM2KHook/src/netplay/netplay.cpp` ~2973-2992, call
  `SpectatorNode_WriteCurrentBattleFile("replays/<ts>.fm2krep")`.
- Rounds are **inside** the file as `ROUND_START` / `ROUND_END` session events;
  the v2 header's `round_offsets[8]` table holds their byte positions so the
  player can seek to a round. `FM2KHook/src/netplay/spectator_node.cpp` ~2306-2440.
- `.fm2krep` = one match (battle slice, header flag bit 0). `.fm2kset` = a full
  session of several matches (flag bit 0 clear).
- A rematch = a new match = a new `.fm2krep`. So multiple files from one sitting
  is **one-per-match** (a FT5 set = 5 files), NOT per-round. Easy to misread as
  "per round."

## The host-only gap (the actual bug)

Two host-only gates combine to deny guests any replay:

1. **Round events are host-only.** `round_events.cpp`'s `vs_round_function`
   detour emits `ROUND_START`/`ROUND_END` only when `g_player_index == 0`, so a
   non-host peer's session_events has no round markers.
2. **The file write is host-only.** `netplay.cpp` ~2985 gates the write on
   `if (g_player_index == 0)`. The guest (player 1) skips it entirely.

The write gate cites a filename **race** -- but that race only exists in
**local 2-instance testing** (both processes writing the same `replays/` dir on
one machine). In real netplay the two players are on **different machines**, so
there is no collision. The gate is overly broad and, as a side effect, gives
guests nothing.

### Evidence (real file headers)

```
2026-06-13_..._p0_harness.fm2krep   flags=0x03   ROUND_OFFSETS_SET   <- host
2026-06-13_..._p1_harness.fm2krep   flags=0x01   no round_offsets    <- guest
```
(p0 = host writes a full file with round seek; p1 = guest's file lacks round
markers -- and in a REAL match the guest does not write at all, only in the
local 2-instance harness do both write.)

Older real matches (May) showed `flags=0x01`, `rounds_won` like `1-0` / `1-1`,
`round_count=0` -- all confirming per-match battle slices.

## Fix options (pick one)

- **Cheap:** lift the write gate so the guest also writes (peer-suffix the
  filename, e.g. `_p1`, so the local-test race can't return). The guest's file
  would be a complete, playable full-match replay but WITHOUT round-seek markers
  (round events are host-only).
- **Complete (recommended):** also emit `ROUND_START`/`ROUND_END` on the guest
  side -- drop the `g_player_index == 0` gate in `round_events.cpp`. Both sims
  hit the round function deterministically, so emitting on both is safe. Then
  lift the write gate. Result: both players get full replays WITH round markers.
  Verify on the local 2-instance loopback harness (the exact place the original
  race lived) so the fix does not reintroduce a same-dir collision.

## Reference

### v2 header (`SpectatorNode` `WriteCurrentBattleFile`)
- magic `'FMSS'` (0x53534D46) @0, `version`=2 @4, `flags` @6
  (bit0: 1=battle slice `.fm2krep`; bit1: 1=`round_offsets[]` populated).
- `rounds_won_p1` @132, `rounds_won_p2` @133, `round_count` @144 (offsets per the
  launcher parse in `FM2K_LauncherUI.cpp`).
- `round_offsets[8]` table = per-round byte positions for seek.

### Inspect a file
```python
import struct
b = open("replays/<name>.fm2krep","rb").read(160)
print(b[0:4], struct.unpack_from("<H",b,4)[0],            # magic, version
      hex(struct.unpack_from("<H",b,6)[0]),               # flags (0x03 = round seek)
      f"{b[132]}-{b[133]}")                                # rounds won
```

### Key locations
- recorder + write gate: `FM2KHook/src/netplay/netplay.cpp` ~2973-2992
- round-event emit (host-only gate): `FM2KHook/src/netplay/round_events.cpp`
  (`vs_round_function` detour)
- format / round_offsets: `FM2KHook/src/netplay/spectator_node.cpp` ~2306-2440
- replay browser (list / round-level seek / round-as-standalone export):
  `FM2K_LauncherUI.cpp` `RenderReplayBrowser` ~1454, ~1915
