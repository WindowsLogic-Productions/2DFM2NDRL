# Spectator snapshot-join smoke test

Manual procedure for validating task #18 (CCCaster-style snapshot join) end-
to-end via the live hub. No CLI flags — uses the same path users hit when
they click "Spectate" in the launcher's live-matches list.

Three machines preferred; two with two launcher instances on one machine
also works. The spectator must NOT be in the same hub session as either
peer (separate Discord login).

---

## Pre-flight (everyone)

- [ ] Latest launcher build deployed: `/mnt/c/games/FM2K_RollbackLauncher.exe`
      version pill matches the latest `LatestVersion` on github (or you've
      bumped it via `cut_release.sh`).
- [ ] Hub on `2dfm.org` is running v0.2.30 hook bits + schema-2 ingestion.
      Verify with `ssh 2dfm "sudo systemctl status fm2k-hub | head -3"`.
- [ ] Both peers + spectator have the same FM2K game installed (e.g.
      `pkmncc.exe` under `C:\games\2dfm\pkmncc\`). Game-data hash must
      agree — mismatch logs `[match] HASH_MISMATCH` in the hook log
      and the launcher pops the manifest modal.

---

## Path A — Hub-driven spectate (the one users will hit)

This is the primary code path: hub event → launcher button → spectator
process with `FM2K_SPECTATE_MODE=current` set automatically.

### Setup

1. **Peer 1 + Peer 2** sign into the launcher with their Discord accounts,
   join the same lobby (e.g. `pkmncc`), challenge each other, accept,
   start a match. Play through CSS → battle → at least one round end.
2. **Spectator** signs in with a third Discord account (or a different
   launcher instance), joins the same lobby. The active match should
   appear in the "Live Matches" panel with `P1 (Char) vs P2 (Char)
   on Stage` populated.

### Action

3. **Spectator** clicks the row → "Spectate" button.

### What to observe

The spectator's launcher should:

- [ ] Spawn a third game-window (FM2K_SPECTATOR_MODE=1).
- [ ] Hook log line: `Spectator mode ENABLED — will SPEC_JOIN_REQ host on
      startup` (NOT "Replay mode ENABLED — playing X").
- [ ] Hook log line: `Netplay (spectator): SPEC_JOIN_REQ sent to host
      (mode=CURRENT_MATCH)`.

The host's hook log should show:

- [ ] On Netplay_StartBattle (when match started, before spectator joined):
      `SpectatorNode: snapshot cached (match=N, K bytes, input_frame=F,
      fletcher32=0x...)`. K is typically ~50 KB.
- [ ] On spectator JOIN_REQ arrival: `SpectatorNode: SPEC_JOIN_REQ
      mode=CURRENT_MATCH` followed by `SpectatorNode: SendSnapshotTo
      (subscriber=ip:port, ...)` and a series of SNAPSHOT_BEGIN/CHUNK*/END
      packets.
- [ ] After snapshot: `SpectatorNode: SendBackfillFromFrame anchor=F
      events=N` (events from the snapshot's anchor frame onward, NOT
      from session start).

The spectator's hook log should show:

- [ ] `SpectatorNode: SNAPSHOT_BEGIN received (match=N, K bytes,
      anchor=F)`.
- [ ] N × `SpectatorNode: SNAPSHOT_CHUNK received (...)`.
- [ ] `SpectatorNode: SNAPSHOT_END applied (match=N, K bytes,
      fletcher32=0x...) — anchor INPUT-frame=F`.
- [ ] `SaveState_LoadFromBytes: applying K-byte snapshot ...`.
- [ ] **NO** title or CSS phase: spectator's `game_mode changed: 0 →
      1000` should jump straight to `→ 3000` after snapshot apply (or
      whatever phase the host was at). NOT `0 → 1000 → 2000 → 3000`.
- [ ] First `[SPEC-FP]` line shows `pop=0` (or very small, from
      post-snapshot trailing INPUTs) and HP/positions match the host's
      live state at that moment.

### Pass criteria

- Spectator's local game **renders the current battle live** within
  seconds of clicking Spectate, regardless of how many matches have
  already been played in the session.
- Spectator's HP / position / round counter match what the host sees
  on screen (eyeball the title bar's `<game> | Spec Subscribed | q:N`
  alongside the host's `pkmncc | P1 vs Peer 2-1 (BATTLE) | RTT 12ms ...`).
- Title bar reads `Spec Subscribed`, NOT `Spec Connecting...`. (If
  Connecting persists > 5 seconds, snapshot transfer is hung — see
  Failure modes below.)

---

## Path B — Multi-match validation

After Path A passes, validate the snapshot refresh between matches:

1. P1 + P2 play **match 1** to completion (winner crowned, both back
   to CSS for rematch).
2. P1 + P2 play **match 2** through ~round 1.
3. Spectator clicks Spectate **during match 2**.

What to verify:
- [ ] Host log at match 1 → match 2 boundary: a NEW `SpectatorNode:
      snapshot cached (match=1, ...)` line (i.e. match_index incremented
      from 0 to 1).
- [ ] Spectator's `SNAPSHOT_END applied` log shows `match=1` (the
      current match's index, not match 0).
- [ ] Spectator's first rendered frame is in match 2 — round counter
      reads matches the host's match 2 state.

### Pass criteria

- Spectator never sees match 1's gameplay. The replay is "match 2
  only", anchored at match 2's snapshot.

---

## Path C — Full session mode (regression check)

Verify FULL_SESSION still works:

1. Same setup as Path A.
2. Spectator launches a SECOND launcher instance with the env override
   in cmd.exe before starting:
   ```
   set FM2K_SPECTATE_MODE=full
   C:\games\FM2K_RollbackLauncher.exe
   ```
   Then click Spectate.

What to verify:
- [ ] Hook log: `Netplay (spectator): SPEC_JOIN_REQ sent to host
      (mode=FULL_SESSION)`.
- [ ] Host log: `SendSessionBackfillTo (subscriber=...)` — NOT
      SendSnapshotTo.
- [ ] Spectator walks title → CSS → battle from session start.

### Pass criteria

- Spectator replays the entire session, not just current match. Slower
  to "catch up to live" but identical events.

---

## Failure modes / what to look for

| Symptom | Likely cause | Where to look |
|---|---|---|
| Spectator stays at "Spec Connecting..." for >5s | Snapshot transfer hang. Host emitted SendSnapshotTo but TCP framer rejects an unknown SpecDataType, or fletcher32 mismatch on assembly. | Host log: `SpectatorTCP: unknown SpecDataType=N`. Spectator log: `SNAPSHOT_END byte-count mismatch` or `SNAPSHOT_END checksum mismatch`. |
| Spectator's first frame is at title screen, not battle | SaveState_LoadFromBytes silently failed; receive path fell through to backfill from frame 0. | Spectator log: `SaveState_LoadFromBytes failed (size mismatch...)`. Likely cause: peer's hook DLL is older; SaveState slot layout changed. Both peers + spectator need same launcher version. |
| Spectator picks wrong characters at CSS | Snapshot path skipped, fell through to FULL_SESSION mode, then auto-CSS hook had wrong target chars. | Spectator log: `Netplay (spectator): SPEC_JOIN_REQ sent to host (mode=FULL_SESSION)` instead of CURRENT_MATCH. Check FM2K_SPECTATE_MODE env var actually got set. |
| Match 2 spectator sees match 1's gameplay | StashSnapshot's `match_index` not refreshing correctly between matches, or `pb_match_headers.clear()` on apply broke a downstream consumer. | Host log: snapshot's match_index stayed 0 across two Netplay_StartBattle calls. Spectator log: `pb_queue` size after SNAPSHOT_END is non-zero. |
| Periodic "snapshot cache stays empty" warnings | `SaveState_Save(0)` failing — usually means the rollback engine's slot 0 is occupied or save_state-disabled. Or `g_is_rolling_back` was true during StashSnapshot (new gate added v0.2.31+; means an unexpected caller). | Host log: `SpectatorNode: StashSnapshot — SaveState_Save(0) failed`. Or `StashSnapshot skipped — called during rollback rewind`. |

---

## Quick log-tailing recipes

```bash
# Host hook log (from WSL)
tail -f /mnt/c/games/2dfm/<game>/logs/FM2K_P1_Debug.log | grep -E "SpectatorNode|SaveState|snapshot"

# Spectator hook log
tail -f /mnt/c/games/2dfm/<game>/logs/FM2K_P3_Debug.log | grep -E "SpectatorNode|SaveState|SPEC-FP|SPEC-TRACE"

# Hub log (live match-result + spectate-grant traffic)
ssh 2dfm "sudo journalctl -u fm2k-hub -f | grep -E 'spectate|snapshot|MATCH'"
```

---

## Known-good baselines

If snapshot path breaks, fall back to FULL_SESSION + replay-file
playback to confirm the rest of the spectator pipeline still works:

- `FM2K_SPECTATE_MODE=full` → CSS auto-confirm + INPUT pop gate exercises
  the same trampoline path as offline replay.
- `--replay <path>` CLI on a known-good `.fm2krep` should still replay
  cleanly (validated this session). If `--replay` works but live-spec
  mid-set doesn't, the divergence is in the snapshot path specifically.
