# Roadmap: Replay Hierarchy — Session / Match / Round

**Status**: parked. Companion to `replicated-crafting-blum.md` (the spectator-determinism + replay-format plan, in progress in another session). This doc lays out the **player-facing** model — the hierarchy, the file/index layout, the seek mechanism, the launcher's replay browser UI, and the hub schema additions.

The companion plan covers the wire protocol + storage. This doc covers the human-facing consequences: what users browse, what files exist on disk, how navigation feels.

## The model — three levels

| Level | Definition | Boundary on the wire | Storage | Typical count |
|---|---|---|---|---|
| **Session** | One connection between two players, from lobby-join to lobby-leave | start of stream → MATCH_END after final match → end of stream | `.fm2kset` (one file) | 1 |
| **Match** (or "set") | One W/L unit. CSS → battles until someone wins (FT2 typical) | `MATCH_START` → `MATCH_END` events | `.fm2krep` (one file per match) + sliced from .fm2kset | 1-5 per session |
| **Round** | One individual fight inside a match | `ROUND_START` → `ROUND_END` events | byte offsets inside .fm2krep, no separate file | 2-5 per match |

A typical evening of play might produce 1 session × 4 matches × 2.5 rounds = **10-12 round entries** in the replay browser, all from one connection.

## On-disk layout

```
%APPDATA%\FM2K_Rollback\
  ├── sessions\
  │   └── 20260507_142312_Spooder_vs_Sloth.fm2kset
  ├── replays\
  │   ├── 20260507_142312_Spooder_vs_Sloth_M1.fm2krep
  │   ├── 20260507_142312_Spooder_vs_Sloth_M2.fm2krep
  │   ├── 20260507_142312_Spooder_vs_Sloth_M3.fm2krep
  │   └── 20260507_142312_Spooder_vs_Sloth_M4.fm2krep
  └── replays\index.json    ← scan index, refreshed lazily on launcher boot
```

Two file types only. **Round-level "files" don't exist as separate entities** — they're byte offsets recorded in the match file's header (`round_offsets[8]`). This keeps the on-disk corpus simple and avoids a thousand-tiny-files problem.

## File header (the seek-anchor metadata)

```c
struct FM2KRepHeader {
    uint32_t magic;                  // 0x52504D46 "FMPR"
    uint32_t header_size;            // for forward compat
    uint32_t event_count;
    uint32_t round_count;
    uint32_t round_offsets[8];       // byte offset to each ROUND_START
    char     game_id[32];
    char     p1_nick[32];
    char     p2_nick[32];
    uint8_t  p1_char_id;
    uint8_t  p2_char_id;
    uint8_t  rounds_won_p1;
    uint8_t  rounds_won_p2;
    uint64_t started_at_unix;
    uint64_t finished_at_unix;
    uint8_t  reserved[64];
};
```

Computed at write time during the `Netplay_EndBattle` slice (companion plan's C7).

## Seek-with-fast-forward — the clever bit

Round-level navigation feels instant because we **deterministically replay the events leading up to the target**, then transition to live pace at the boundary. No scrubbing, no rewinding, no separate "round mode."

```
User clicks "Round 3 of Match 2"
   ↓
Replay_LoadSessionFile(path, seek_target = {ROUND_START, 3})
   ↓
Push every event in the .fm2krep into pb_queue
   ↓
Engage C5.5 fast-catch-up drain:
   - render disabled
   - audio dispatch muted
   - PGI + UG run at unbounded rate
   ↓
While queue depth > LIVE_LAG_FRAMES (100):
   pop event, apply, no render
   ↓
When queue depth ≤ LIVE_LAG_FRAMES:
   exit catchup mode, re-enable render + audio
   ↓
First visible frame: round 3 starting position
   ↓
Normal-pace playback continues
```

**Wall-clock budget**: skipping a 90-second round at unbounded sim rate ≈ 50 ms. From click to first rendered frame is well under one display refresh — feels instant.

This is the **same code path** the spectator system uses for mid-session join (companion plan's C5 + C5.5). The replay browser reuses it for free.

## Launcher replay browser UI

ImGui tree view. Three depth levels matching the hierarchy. Each node renders metadata + a Watch action.

```
┌─ Replays ────────────────────────────────────────────────────────────┐
│ Filter: [All games ▾]  [Last 7 days ▾]  [My matches]  Search: [___] │
├──────────────────────────────────────────────────────────────────────┤
│ ▼ 2026-05-07 14:23 · Spooder vs Sloth · pkmncc · 32m · 4 sets       │
│   ▶ Set 1 · Spooder W (2-0) · 5m12s                       [▶ Watch] │
│   ▼ Set 2 · Sloth W (2-1) · 8m04s                         [▶ Watch] │
│      • Round 1 · 0:58 · Spooder W                         [▶ Watch] │
│      • Round 2 · 1:08 · Sloth W                           [▶ Watch] │
│      • Round 3 · 1:31 · Sloth W                           [▶ Watch] │
│   ▶ Set 3 · Spooder W (2-1) · 6m48s                       [▶ Watch] │
│   ▶ Set 4 · Sloth W (2-0) · 4m52s                         [▶ Watch] │
│ ▶ 2026-05-06 19:45 · Spooder vs FlippySpatula · vanpri · 14m · 2    │
│ ▶ 2026-05-06 18:12 · Spooder vs aprl · pkmncc · 8m · 1 set          │
│ ▶ 2026-05-05 22:30 · Spooder vs Sloth · pkmncc · 22m · 3 sets       │
└──────────────────────────────────────────────────────────────────────┘
```

**Click handlers:**
- Session row → `Replay_LoadSessionFile(<.fm2kset>, nullopt)` — full session from frame 0
- Set row → `Replay_LoadSessionFile(<set_M2.fm2krep>, nullopt)` — match from MATCH_START
- Round row → `Replay_LoadSessionFile(<set_M2.fm2krep>, {ROUND_START, idx})` — fast-forward through earlier rounds, plays from boundary

**Right-click context menu per row:**
- Open file location (Windows Explorer at the replay dir)
- Export round as standalone replay → write a derived `.fm2krep` slice with PIN_RNG + MATCH_START prepended so it plays in isolation
- Copy share link (placeholder for future hub upload)

**Filter chips** are pure client-side over the loaded index — no I/O on filter change. Search box matches nick + game name fuzzy.

## Hub schema (matches.json) — per-match record additions

Existing record stays. Add three optional fields (gated on schema version 2):

```jsonc
{
  // existing
  "id": "abc123",
  "p1_nick": "Spooder",
  "p2_nick": "Sloth",
  "p1_char_id": 7,
  "p2_char_id": 12,
  "game_id": "pkmncc",
  "winner_id": "...",
  "started_at": 1714900000.0,
  "finished_at": 1714900615.0,

  // NEW (schema 2)
  "schema": 2,
  "session_id": "20260507_142312_a3f1b9c2",
  "match_index_in_session": 2,
  "rounds": [
    { "winner": "p1", "frames": 4200, "p1_hp_left": 0,   "p2_hp_left": 380 },
    { "winner": "p1", "frames": 7500, "p1_hp_left": 142, "p2_hp_left": 0 }
  ]
}
```

**Backwards-compat**: any reader that doesn't know about schema 2 ignores the new fields and renders matches as before. Stats site's `/recent` and `/g/<game>` flat-list views work unchanged. New views (`/session/<id>`, per-match page's rounds breakdown) only render when the record has the new fields.

## Public stats site — new views unlocked

Once the hub records sessions + rounds, the stats site at 2dfm.sytes.net:8765 can grow:

- **`/sessions`** — recent sessions grid, mirrors the launcher's tree at the top level
- **`/session/<id>`** — single session view, lists matches with round-by-round breakdown, links each to a per-game page
- **Per-match page enhancements** — small "Rounds" panel showing round-by-round W/L + HP-remaining, instead of just the final 2-1 / 2-0 summary
- **Per-character matchup pages** — round-level breakdown (Spooder/Pancham vs Sloth/Cinccino: 4 rounds, 3 W, 1 L)

These are downstream of the hub schema landing — not dependent on the launcher replay browser or even the replay file format.

## Wire-event ordering invariants

Strict ordering inside any session_history slice:

```
[start of session]
  PIN_RNG          ← initial CSS RNG seed
  RESET_INPUT_STATE
  INPUT × M        ← CSS inputs until both confirm
  MATCH_START      ← match 1 begin
    PIN_RNG        ← battle RNG re-pin
    SOUND_INIT
    ROUND_START 1  ← round 1 begin
      INPUT × K1
    ROUND_END 1    ← round 1 winner (round timer or HP-zero)
    ROUND_START 2
      INPUT × K2
    ROUND_END 2
    ROUND_START 3
      INPUT × K3
    ROUND_END 3
  MATCH_END        ← match 1 ends
  PIN_RNG          ← inter-match CSS RNG re-pin (returning to character select)
  RESET_INPUT_STATE
  INPUT × M2       ← CSS inputs for next match
  MATCH_START      ← match 2 begin
  ...
[end of session]
```

The replay browser parses this structure once at index time. Each ROUND_END's payload (winner, HP remaining, frames) feeds the small "Round N · 1:08 · Sloth W" tooltip. Each MATCH_END feeds the match summary. Sessions are anchored on connect/disconnect at the netplay layer, NOT inferred from event count.

## What this gives the player

1. **Browse**: open the launcher's Replays panel, see a clean tree of recent play, drill into specific moments.
2. **Watch the whole session**: click the top-level row, replay starts from the lobby-join moment and runs through every match.
3. **Watch a specific match**: skip to the W/L unit you remember.
4. **Watch a specific round**: instant fast-forward to that round's start. No scrubbing.
5. **Share a clutch round**: right-click → Export → ships a tiny `.fm2krep` (one round's worth, ~20-50 KB) to a friend who can replay it in their own launcher.
6. **Stats**: hub's site shows session-grouped match history with round-by-round HP breakdowns.

## Where this lives in the companion plan

The companion plan at `replicated-crafting-blum.md` covers the implementation. Specifically:

- **C1, C2** — event log infrastructure
- **C3, C3.5** — emit PIN_RNG, RESET_INPUT_STATE, SOUND_INIT, ROUND_START, ROUND_END events
- **C5, C5.5** — backfill ordering fence + fast-catch-up drain (the seek mechanism)
- **C6** — MATCH_START / MATCH_END events
- **C7** — write `.fm2kset` + `.fm2krep` files with `round_offsets[]` header
- **C8** — `Replay_LoadSessionFile(path, seek_target)` API
- **C10** — hub `matches.json` schema extension (session_id + rounds[])
- **C11** — launcher Replay browser tree UI

This doc adds the player-facing context. The two read together.
