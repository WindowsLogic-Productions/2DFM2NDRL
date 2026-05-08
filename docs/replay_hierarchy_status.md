# Replay Hierarchy — Current Status & C7 File-Format Design

Live tracker for the Session → Match → Round replay system. Sits between two
docs you already have:

- `docs/roadmaps/replay_hierarchy.md` — parked player-facing roadmap (UI,
  on-disk layout, hub schema). The "what we want" doc.
- `~/.claude/plans/replicated-crafting-blum.md` — 13-commit implementation
  plan (C1–C11). The "how we get there" doc.
- `docs/c3.5_round_events_ida_handoff.md` — IDA recon brief for C3.5
  (now SHIPPED — see status banner there).

This doc is the snapshot of where the chain stands today and the format
design we settled on for the next commit (C7).

---

## What's shipped

### C1–C6 — wire-format spine + spectator determinism (production)

Already landed before this thread. Input-replay lockstep spectator runs on
the `SessionEvent` event log model. `.fm2kset` (full session) and
`.fm2krep` (per-battle slice) files write today via
`SpectatorNode_WriteSessionFile` / `SpectatorNode_WriteCurrentBattleFile`
(`FM2KHook/src/netplay/spectator_node.cpp:1282-1299`). v0.2.8 ships with
this active.

### C7 + MATCH_END enrichment + SESSION_ID — file format v2 (just landed, 65/65 green)

| Piece | File | Status |
|---|---|---|
| `SessionEventType::MATCH_END` payload extended → 7 B (`MatchEndPayload`) | `spectator_node.h` | shipped |
| `SessionEventType::SESSION_ID` (=10) | `spectator_node.h` | shipped |
| `FM2KSessionFileHeader` (256 B) replaces 32 B `SessionFileHeader` | `spectator_node.cpp` | shipped |
| `WriteSessionFileImpl` populates `round_offsets[]`, char/color, winner / per-side rounds, session_id, match index | `spectator_node.cpp` | shipped |
| `LoadSessionFile` v1+v2 envelope dispatch (legacy 32 B fallback) | `spectator_node.cpp` | shipped |
| Host generates `session_id` lazily at first Netplay_StartBattle (`epoch << 32 ^ rand64`) and emits SESSION_ID event before first MATCH_START | `netplay.cpp` | shipped |
| `Netplay_EndBattle` captures winner_idx + per-side rounds for MatchEndPayload | `netplay.cpp` | shipped |
| Test mirror updated; round-trip tests for MATCH_END + SESSION_ID + v2 header offset pin | `test_spectator_protocol.cpp` | shipped |

Files written today are v2 256 B header + body; loader still reads pre-deploy v1 files via the `SessionFileHeaderV1` fallback.

### C3.5 — ROUND_START / ROUND_END events (62/62 green)

| Piece | File | Status |
|---|---|---|
| `SessionEventType::ROUND_START (=8)` / `ROUND_END (=9)` | `spectator_node.h` | shipped |
| `RoundStartPayload` (7 B) / `RoundEndPayload` (9 B) packed structs | `spectator_node.h` | shipped |
| `SessionEvent::u` widened 4→9 B; `sizeof = 10` | `spectator_node.h` | shipped |
| `EncodeRoundStart` / `EncodeRoundEnd` encoders | `spectator_node.cpp` | shipped |
| Decoder branches for both new types | `spectator_node.cpp` | shipped |
| `WirePayloadSize` + `IsValidEventTag` updates | `spectator_node.cpp` | shipped |
| `FlushBatch` + backfill chunk in-memory→wire dispatch | `spectator_node.cpp` | shipped |
| Apply-time spectator log lines (informational; sim drives banners) | `spectator_node.cpp` | shipped |
| Hop-1 daisy-chain relay branches | `spectator_node.cpp` | shipped |
| Host `Append*` APIs with internal frame-delta computation | `spectator_node.cpp` | shipped |
| `vs_round_function` MinHook detour (pre/post substate edge detect) | `FM2KHook/src/hooks/round_events.cpp` (new) | shipped |
| Hook install in `InitializeHooks`; reset on `Netplay_StartBattle` | `hooks.cpp` + `netplay.cpp` | shipped |
| Test mirror updated; round-trip tests for both event types | `test_spectator_protocol.cpp` | shipped |

**Effect today:** every round transition on the host produces an event in
`session_events`. Spectators apply them informationally + relay them
through daisy-chain. `.fm2krep` files written via
`SpectatorNode_WriteCurrentBattleFile` already contain the round events
inside the body bytes — they just aren't indexed in the header yet.

FM2K-only via `#if !defined(ENGINE_FM95)`. FM95 emit is a separate
hand-off when we get there.

---

## Current on-disk format (C1–C3.5 production)

```
.fm2kset / .fm2krep file
├── SessionFileHeader (32 B)                  ← spectator_node.cpp:1213
│   ├── magic            'FMSS' (0x53534D46)
│   ├── version          = 1
│   ├── flags            bit 0: 1=battle slice (.fm2krep), 0=full session (.fm2kset)
│   ├── unix_timestamp
│   ├── event_count
│   ├── input_count
│   └── reserved[8]
└── packed SessionEvent[] body bytes          ← same encoding as EVENT_BATCH
```

One magic, one header, one body encoder. Discriminator between
`.fm2kset` and `.fm2krep` is the flag bit. The format is correct for
playback today, but it's too thin to drive the launcher tree UI or
round-level seek — that's what C7 fixes.

---

## C7 — Proposed enriched header (next commit)

Replaces the 32 B header with a 256 B layout that carries everything the
launcher tree needs without re-parsing the body.

```c
struct FM2KSessionFileHeader {  // 256 B
    // wire envelope (preserves first 8 bytes from v1 layout)
    uint32_t magic;              // 'FMSS' (.fm2kset) or 'FMPR' (.fm2krep) — see compat note
    uint16_t version;            // = 2
    uint16_t flags;              // bit 0: battle slice, bit 1: has_round_offsets

    // descriptive
    uint64_t started_at_unix;
    uint64_t finished_at_unix;
    uint32_t event_count;
    uint32_t input_count;
    char     game_id[32];        // e.g. "pkmncc"
    char     p1_nick[32];
    char     p2_nick[32];

    // character / outcome (from MATCH_START side table + enriched MATCH_END)
    uint8_t  p1_char_id;
    uint8_t  p2_char_id;
    uint8_t  p1_color;
    uint8_t  p2_color;
    uint8_t  rounds_won_p1;
    uint8_t  rounds_won_p2;
    uint8_t  match_count;        // .fm2kset: total in session; .fm2krep: 1
    uint8_t  match_index;        // .fm2krep: 1-based; .fm2kset: 0
    uint64_t session_id;         // shared across .fm2krep slices of one .fm2kset

    // seek anchors
    uint8_t  round_count;        // 1..8 for .fm2krep, 0 for .fm2kset
    uint8_t  reserved0[3];
    uint32_t round_offsets[8];   // body-relative byte offsets, each pointing
                                 //   at a ROUND_START tag byte; unused = 0

    // future-proofing
    uint8_t  reserved[64];
};
static_assert(sizeof(FM2KSessionFileHeader) == 256, ...);
```

Body unchanged: same packed `SessionEvent[]` bytes after the header.

### Compat strategy (settled)

Bump `version` to 2; readers fall back to v1 when `version == 1`.
v0.2.8 hasn't deployed any reader for these files, so collected v1
`.fm2kset` files from existing users can be detect-and-upconvert
(treat as session, no `round_offsets`, infer nicks from MATCH_START
side-table parse). One clean break window.

### What `WriteSessionFileImpl` needs to change (C7 work)

1. Replace `SessionFileHeader` (32 B) with `FM2KSessionFileHeader`
   (256 B). Bump version to 2.
2. After encoding the body, single forward pass to populate
   `round_offsets[]` from ROUND_START tag positions in the body bytes.
3. Pull `p1_nick`/`p2_nick`/char IDs from the most-recent MATCH_START's
   side-table header (`g_state.match_headers`).
4. For `.fm2krep` writes (`WriteCurrentBattleFile`), populate
   `match_index` from a hook-side counter; `rounds_won_p1` /
   `rounds_won_p2` from the latest MATCH_END payload (needs the
   enrichment below).
5. Add `session_id` generation at `Netplay_OnPeerConnected`
   (broadcast as a new SESSION_ID event, or derive deterministically —
   see Open Question).
6. Update `SpectatorNode_LoadSessionFile` to parse v2 header and expose
   `round_offsets[]` to the C8 seek path.

---

## Why no per-round files

A 4-match BO5 night with avg 2.5 rounds/match = ~30 round files.
Round-as-separate-file is wrong for two reasons:

1. **Bytes duplicate.** A round can't replay without the
   MATCH_START + initial PIN_RNG + RESET_INPUT_STATE + SOUND_INIT
   prefix, so each per-round file would carry a copy of those.
2. **Tree explodes.** The launcher's index scan has to fread every
   replay header at boot; 30 round files × 1000-replay corpus = 30k
   freads instead of 1k.

Round-as-byte-offset gives instant seek (per the fast-catchup-drain
mechanism below) without either cost.

---

## Seek-with-fast-forward (C8, planned)

```
User clicks "Round 3 of Match 2"
   ↓
Replay_LoadSessionFile("..._M2.fm2krep", seek={ROUND_START, idx=3})
   ↓
fread header → seek body to round_offsets[2]
   ↓ Need MATCH_START + initial state-init events from body[0..offset]
   ↓
Two-pass load:
  Pass 1: scan body[0..round_offsets[2]] — push ONLY PIN_RNG, RESET_INPUT_STATE,
          SOUND_INIT, MATCH_START into pb_queue (skip earlier rounds' INPUTs and
          ROUND_*). Rebuilds engine state at round 3's start without sim work.
  Pass 2: stream body[round_offsets[2]..end] into pb_queue normally.
   ↓
RunSpectatorTick engages C5.5 fast-catchup drain
  (render off, audio off, PGI+UG at unbounded rate)
  until queue depth ≤ LIVE_LAG_FRAMES.
   ↓
First rendered frame = round 3 starting position.
```

Wall-clock: ~50 ms for a 90-second skipped round at unbounded sim
rate. Pass 1 is essentially free (handful of memory writes).
Seek-from-MATCH_START is a strict subset (Pass 1 a no-op, since the
MATCH_START is at body[0]).

The same C5.5 drain serves three callers — live spectator backfill,
spectator failover reconnect, and now replay seek. One code path.

---

## Open follow-ups (post-C3.5)

| Item | Plan ref | Notes |
|---|---|---|
| ~~**C7** — `round_offsets[]` + enriched header~~ | shipped | 256 B `FM2KSessionFileHeader`; `round_offsets[8]` populated at write time; v1 fallback in loader. |
| ~~**MATCH_END payload extension**~~ | shipped | `MatchEndPayload` (7 B) carries winner / per-side rounds / frames_total. |
| **C8** — `Replay_LoadSessionFile(path, seek_target)` | replicated-crafting-blum.md C8 | Reuses C5.5 drain. Loader's v2 path now exposes `round_offsets[]`; seek pass needs to ingest a `seek_target` and use those offsets. |
| **C10** — hub schema v2 | replicated-crafting-blum.md C10 | Forward captured `RoundEndPayload` through 0xCC outcome message into `matches.json` `rounds[]`. Adds `session_id` + `match_index_in_session` to records. Backwards-compat: schema-version-gated. |
| **C11** — launcher Replay browser tree UI | replicated-crafting-blum.md C11 | Pure launcher work; reads C7 headers via the index.json cache. Click→`Replay_LoadSessionFile` with seek. |
| **FM95 ROUND_START/ROUND_END emit** | new hand-off | Find the equivalent of `vs_round_function` in FM95; same SessionEvent emit pattern. Separate IDA recon needed. |

The natural sequence is C7 + MATCH_END enrichment (one commit, ships
self-describing files), then C8 (seek), then C10/C11 in either order.
C10 unblocks the public stats site; C11 unblocks the in-launcher
browser. Both consume what C7 produces.

---

## Settled: `session_id` is host-generated (Option A)

Generated lazily at first `Netplay_StartBattle` of the connection:
`session_id = (epoch_seconds << 32) ^ rand64()`. Emitted as
SessionEvent::SESSION_ID before MATCH_START so subscribers + replay
files inherit it. Stored on `g_state.session_id`; the file writer
pulls it from there.

---

## Where to pick up

C8 is next. Surface area is small:

1. New entry point `Replay_LoadSessionFile(path, seek_target)` where
   `seek_target = std::optional<{event_kind, idx}>`. When set, the loader
   reads the v2 header's `round_offsets[]`, picks the requested anchor,
   does a two-pass body walk (Pass 1: state-init events only up to the
   anchor; Pass 2: stream from anchor forward into pb_queue), and engages
   the existing C5.5 catch-up drain.
2. Catch2 round-trip test asserting "open .fm2krep with `seek={ROUND_START,
   2}` lands at round 2's expected pb_queue position".

Then C10 + C11 can land in parallel — independent agents.
