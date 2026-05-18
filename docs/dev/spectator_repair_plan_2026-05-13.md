# Spectator repair plan — 2026-05-13

Snapshot from the post-v0.2.41 spectator desync incident on vanpri.
Three logs in `/mnt/d/Games/fm2k/_NODEV/vanguard-princess/logs/`
(P1 host, P2 client, P3 spectator) demonstrated:

- P3 connected to **P2** (not P1) because P1's spec TCP listener
  failed to bind on its own gameplay port (51130).
- P3 received clean backfill (~5700 events) in FULL_SESSION mode.
- P3's local sim **diverged from the host before frame 4000**: host
  reported p2_hp=1, P3 reported p2_hp=87 frozen. Match ended on host,
  P3 froze.

The connection layer works. The input-replay determinism doesn't.

This document is the staged plan to fix the spectator path. Goals:

1. Spectator joining mid-CSS lands in CSS lockstep with host.
2. Spectator joining mid-battle lands in battle without visibly
   walking title + CSS.
3. Multi-round matches (round 2, round 3, …) keep the spectator in
   sync without re-divergence.
4. We have tools to diagnose new spectator desyncs without manual
   log diffing.

---

## Phases

Each phase is a self-contained commit that builds + tests on its
own. Tag phases A-F so we can refer to them in commits.

### Phase A — `parity-diff` dev tool (diagnostic foundation)

**Why first:** every other phase needs a way to verify "did the
spectator actually stay in sync this time?" Currently we eyeball
log timestamps. With the auto-upload pipeline, host + spectator
fingerprints land in the hub as separate uploads under the same
match_id. A diff tool that pulls both and aligns by frame answers
"first divergence at bf=N, which field went wrong" in one command.

**What:**
- New subcommand `fm2k_logs.py parity-diff <match_id>`
  1. Pull both peers' bundles via `/by_match/<match_id>` (or
     accept --host-log + --spec-log paths for local files).
  2. Extract `[HOST-FP] bf=N rng=X hp=Y pos=Z timer=T ...` lines
     from host log.
  3. Extract `[SPEC-FP] bf=N rng=X hp=Y pos=Z ...` from spec log.
  4. Sync by `bf` (host's authoritative frame counter; spec's
     SPEC-FP also uses bf).
  5. Print a table: frame, host fields, spec fields, **first
     divergent field highlighted**.
  6. Print first-divergence summary at the top.

**Done when:** running `parity-diff` against the vanpri logs we
already have shows the divergence frame + field. Catches every
future spec desync as long as both sides auto-uploaded.

**Effort:** ~half day. Pure Python, no client-side changes.

### Phase B — Fix P1's spec TCP listener port bind

**Why:** even though P3 connected to P2 by accident, the bind
failure is masking real production behavior. On separate machines
where there's no other process owning port 51130, the spec listener
should bind on the host's chosen port; if it can't, spectators can't
join AT ALL on that side.

**Diagnose:** `SpectatorTCP: NET_CreateServer(port=51130) failed:
Failed to bind listen socket: access socket forbidden`. Likely
causes (in order of probability):
1. **Same port double-bind** — gameplay UDP socket already has
   51130, and the spec listener is trying to bind the same number
   for TCP. Windows allows TCP+UDP on the same port for different
   sockets, but our `NET_CreateServer` may be using `SO_REUSEADDR`
   incorrectly or the socket is being created with the wrong flags.
2. **Privilege/firewall** — Windows is rejecting the bind. Less
   likely since gameplay socket binds fine on the same port.

**Fix paths:**
1. Use a separate port for spec listener (e.g. gameplay_port + 100).
   Hub already routes spec-incoming via STUN'd addr, so port choice
   on the host side is internal. **Simplest, least-risk option.**
2. Or: drop `SO_REUSEADDR` if it's the culprit; rebind cleanly.

**Done when:** P1 log shows `SpectatorTCP: listening on TCP port X`
where X is whatever offset port we picked, not the bind-failed line.

**Effort:** ~2 hours. Touch `spectator_tcp.cpp` only.

### Phase C — Wire `/F` into spectator launch (CURRENT_MATCH only)

**Why:** the existing CURRENT_MATCH mode has a "spectator visibly
walks title + CSS, then jumps to battle" UX problem (called out
inline in the 2026-05-08 commit body). The `/F` boot-to-battle
path we shipped in v0.2.40 reaches `game_mode=3000` in ~150 ms —
exactly the gate the snapshot apply waits for.

**What:**
- In `FM2K_RollbackClient.cpp:LaunchRemoteSpectator`, when
  `mode == SpecJoinMode::CURRENT_MATCH`, set `FM2K_BOOT_TO_BATTLE=1`
  for the spectator child's environment.
- Hook side already primes `g_iniFile_nameOverride` from exe basename
  at `InitializeGameFromCommandLine` entry — no change needed.
- The deferred-snapshot apply (`SpectatorNode_ApplyPendingSnapshot`)
  already gates on `game_mode >= 3000`. With `/F` the engine reaches
  that state in the first frame, so snapshot apply fires immediately.

**Risk:** the kgt's TestPlay defaults pick char 0 vs char 0 as the
placeholder matchup. The snapshot overwrites the 8×57407-byte
character-data pool wholesale, including the "which char file is
loaded" tracking (`g_player_loaded_char_slot`). The `.player` files
for the host's *actual* matchup must exist on disk on the spectator's
machine (almost always true — same game install — but add a sanity-
check log line).

**Done when:** spectator joining a live match with mode=current sees
their hook log go `game_mode 0 → 1000 → 3000` (no `→ 2000` CSS step).

**Effort:** ~2 hours. ~50 lines.

### Phase D — Per-round snapshot refresh

**Why:** the existing `SpectatorNode_StashSnapshot` is called once
from `Netplay_StartBattle` — at the CSS→battle transition. For a
best-of-5 match, by round 4 the snapshot is from round 1's frame 0,
and CURRENT_MATCH joiners have to replay through rounds 1-3 inputs
to catch up. That's the exact path that just desynced for vanpri
(input replay isn't deterministic enough). Refreshing the snapshot
at every round boundary makes the "current match" frame the actual
current frame — minimal replay tail.

**What:**
- Hook into `vs_round_function`'s round-end → round-start substate
  transition (round_events.cpp already detours this function).
- At the substate edge that resets HP for the next round, call
  `SpectatorNode_StashSnapshot()` again. Replaces the stale
  snapshot in `g_state.current_snapshot`.
- New JOIN_REQ between rounds → host ships the refreshed snapshot,
  not the round-1 snapshot.
- Existing in-flight spectators don't get the refresh (they're
  past the join phase); they continue with whatever method they
  joined under.

**Subtlety:** rollback re-entry. If round N starts, snapshot is
captured at frame 0 of round N, then a rollback rewinds to round
N-1, then re-simulates forward through round-start — does
`SpectatorNode_StashSnapshot` get called twice? The 2026-05-08
fix #3 added a `!g_is_rolling_back` gate. Verify that gate still
covers the round-boundary path.

**Done when:** mid-set CURRENT_MATCH joiner at round 4 sees a
snapshot from round 4 frame ~0 (small replay tail), not round 1.

**Effort:** ~half day. The round-boundary hook is already wired
for KOF HP retention; we just add the StashSnapshot call.

### Phase E — Mid-CSS join (CSS snapshot)

**Why:** today, a spectator joining while host is mid-CSS:
- FULL_SESSION mode: replays CSS from frame 0 (catchup FF's, but
  visible).
- CURRENT_MATCH mode: snapshot doesn't exist yet (StashSnapshot
  fires at CSS→battle entry). Falls back to FULL_SESSION.

User vision: spectator joins CSS, gets the host's CSS state, walks
the rest of CSS naturally to battle entry.

**What:**
- Extend `SnapshotMetadata` (or add a separate `CssSnapshot`
  variant) with:
  - `css_frame` (host's CSS sub-frame count)
  - `game_mode_flag` (0/1/2 = 1P/VS/Team)
  - `p1_selected_char_idx`, `p2_selected_char_idx`
  - `p1_cursor_pos[2]`, `p2_cursor_pos[2]`
  - `p1_action_state`, `p2_action_state` (0 unselected, 1 confirmed)
  - `p1_round_count`, `p2_round_count` (team-mode)
  - `team_chars[2][4]` (team-mode picks)
- Host stashes a CSS-state snapshot at title→CSS transition, and
  refreshes every ~30 CSS frames (so late joiners get a recent
  state, not frame 0).
- Spectator receiving a CSS-mode snapshot:
  - Boots WITHOUT `/F` (need real CSS state to apply onto).
  - Once `game_mode == 2000`, applies the CSS state fields via
    direct writes to `g_p*_selected_char_idx` etc.
  - Arms `CssAutoConfirm` to drive the cursor + confirms from
    the forwarded inputs, but starting from the snapshot frame
    rather than frame 0.

**Risk:** loading mid-state CSS via direct memory writes may not
sit cleanly with the CSS state machine (CSS state at `obj+338`
expects to walk 0→1→4 in order). May need to drive via synthesized
inputs rather than direct writes. The fallback is "skip Phase E
for now; mid-CSS join always uses FULL_SESSION" which is fine
because the catchup loop FF's through CSS fast enough.

**Done when:** spectator joining mid-CSS lands in CSS at the host's
current cursor positions + selections, walks remainder naturally.

**Effort:** 1-2 days. Bigger than Phases A-D because CSS state
machine semantics are involved.

### Phase F — Find + fix the input-replay determinism bug

**Why:** even with snapshot delivery, post-snapshot inputs still
replay deterministically. If replay drifts (as we just saw on
vanpri), CURRENT_MATCH joiners desync after some frames too. This
is the long-term hygiene fix.

**Approach:** the parity-diff tool from Phase A pinpoints which
field diverged first. From there:

1. Identify divergent region: RNG vs char state vs effect pool vs
   stage table vs game timer vs something else.
2. Track the call sites that mutate that region.
3. Find the non-deterministic input — most common causes:
   - `timeGetTime()` reads (we already hook this — verify spec
     side reads the same value).
   - `rand()`/`srand()` outside the engine's deterministic RNG.
   - Uninitialized memory reads (very common in FM2K-era games).
   - File-load ordering (cnc-ddraw / 2dfmd may load resources at
     different timing on different boxes).

**Done when:** spec parity matches host parity across a multi-round
match without divergence.

**Effort:** unknown. Could be a half-day if it's a single
non-deterministic call site, or weeks if it's a memory-residue
issue that needs region-by-region zeroing.

---

## Test matrix

Once Phases A-D land, run this matrix end-to-end. Three peers
(two physical machines if possible, otherwise three launcher
instances on one machine). Match ID is the cross-peer identifier
that ties their uploads together.

| # | Host state when P3 joins | P3 mode | Pass criterion |
|---|---|---|---|
| 1 | mid-CSS (cursor moving) | full | P3 walks title + CSS via FF, lands in battle synced |
| 2 | mid-CSS (cursor moving) | current | Phase E land: P3 lands in CSS at host's cursor state. Phase E off: falls back to FULL_SESSION (= test #1). |
| 3 | mid-battle, round 1, frame ~500 | full | P3 walks title + CSS via FF, simulates frames 0..now, ends at current host frame **without divergence** |
| 4 | mid-battle, round 1, frame ~500 | current | P3 boots via /F directly to battle, snapshot applies, simulates ~500 inputs forward |
| 5 | mid-battle, round 3 of 5 | full | Same as #3 but with more replay tail. **This is the case that desynced today.** |
| 6 | mid-battle, round 3 of 5 | current | With Phase D: snapshot is from round 3 frame 0. P3 replays ~current-frame inputs only. No multi-round drift. |
| 7 | mid-battle, round 1, frame ~500, P3 already running | (rejoin) | Reconnect path: P3 was a spec, lost connection, reconnects. Currently broken? Verify. |

The auto-upload pipeline shipped in v0.2.41 means every test case
auto-publishes its host/spec/peer logs to the hub. After each test,
run `tools/fm2k_logs.py parity-diff <match_id>` to confirm sync.

---

## Phasing recommendation

**Iteration 1 (1-2 days): A + B + C**
- Parity tool → diagnostic baseline
- Port bind fix → P1 spec listener actually works
- `/F` into CURRENT_MATCH → mid-battle UX fix
- Ship as v0.2.42

**Iteration 2 (1 day): D**
- Round-boundary snapshot refresh
- Ship as v0.2.43

**Iteration 3 (1-2 days): E**
- CSS snapshot for mid-CSS join
- Ship as v0.2.44

**Iteration 4 (open-ended): F**
- Investigate determinism bug using parity tool data from
  Iterations 1-3 user testing
- Ship fixes as they come

After Iteration 1 we already have a measurably better spectator
experience (port bind works, mid-battle joiners skip CSS). After
Iteration 2 multi-round matches stop accumulating drift via the
fresh-per-round snapshot. After Iteration 3 mid-CSS joiners are
clean. After Iteration 4 we don't need snapshots to mask non-
determinism — but until then, snapshots are the workaround.

---

## What we KNOW vs what we GUESS

| Known | Guess |
|---|---|
| Input replay diverges before frame 4000 on vanpri | Whether the divergence is RNG-side or state-side (parity tool will tell us) |
| P1 spec listener fails to bind on its own gameplay port | Whether it's SO_REUSEADDR or a different port-collision (need code read) |
| CURRENT_MATCH snapshot apply works once spectator is in battle | Whether `/F` + snapshot apply have any interaction we haven't tested |
| Round-end → round-start transition is detectable via vs_round_function detour | Whether we'll trip the rolling-back gate at the round boundary |
| 1.08 MB snapshots transmit fine across loopback | How they perform across real internet RTT (chunking is 16 KB per packet; ~70 chunks per snapshot) |

---

## Open questions

1. **Match-id propagation to spec.** When P3 joins host, does P3
   inherit the host's match_id (so its auto-uploaded crash/desync
   reports group with the same match)? Need to verify the
   spectator's `SpectatorNode_GetSessionId()` returns the host's
   session, not its own.
2. **Snapshot version field.** Current SnapshotMetadata doesn't
   carry an explicit version. If we extend it for Phase E, the
   spec side needs a graceful "version 2 snapshot from a future
   host, fall back to FULL_SESSION" path.
3. **Bandwidth.** 1 MB per snapshot × per-round refresh × N spectators
   could be a real cost on a budget VPS. Snapshot sends are
   direct host→spec (not via hub), but worth measuring.
