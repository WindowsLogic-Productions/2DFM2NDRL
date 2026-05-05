# todo

Pending feature work + known issues, organized by area. Not
prioritized — pick what fits the day. Mark items done as you go,
or split into separate notes when they grow.

---

## Launcher / client UX

### $10 Special Thanks tier — golden names
**Status:** not implemented. Tier role exists (`1500624820012843218`)
and grants hub access, but the gold-name visual isn't wired up.

**Where it goes:**
- Hub side: when issuing a hub token via OAuth (`hub/auth.py`,
  `handle_oauth_callback`), check if the user's Discord roles
  include `1500624820012843218`. Stash a `tier` flag on the
  `HubToken` and include it in the user's broadcast payload.
- User struct in hub.py: add `tier: str = "tester"` (or `"thanks"`),
  serialize in `User.to_dict()`.
- Launcher side: in the room user-list (`RenderHubPanel`'s users
  table around line 1500ish in `FM2K_LauncherUI.cpp`), when
  rendering a row, push a yellow style color if `u.tier == "thanks"`.
- Tweak: also color their name in the active matches row + in chat
  whenever the chat UI lands.

Color suggestion: `ImVec4(1.0f, 0.85f, 0.2f, 1.0f)` (matches the
discord-pill warm orange, just brighter).

### Challenge gating + notifications
A few asks rolled together:

- **Don't allow challenges to/from people who are mid-match.** Hub
  already tracks `status="in_match"`; the `Challenge` button in the
  user table needs to grey out for those rows. Already partially
  done (`can_challenge = (u.status == "idle")`) — verify the path
  for the OUTGOING side too (user shouldn't be able to send a
  challenge while they themselves are in_match).

- **Flash the launcher's titlebar when a challenge arrives**, even
  if the launcher isn't focused. Win32: `FlashWindowEx` with
  `FLASHW_TIMERNOFG | FLASHW_TRAY`. Stops on focus.
  Add to the `K::ChallengeReceived` event handler in
  `FM2K_LauncherUI.cpp`. Pseudo:
  ```cpp
  FLASHWINFO fi = { sizeof(fi), GetActiveWindow(),
                    FLASHW_TIMERNOFG | FLASHW_TRAY, 0, 0 };
  FlashWindowEx(&fi);
  ```
  Also play a short attention sound — SDL3_mixer is already linked,
  drop a 200ms ping.wav next to the launcher.

- **Toast/notification highlight inside the lobby** even if the
  modal is somehow dismissed. Right now we have a popup; if user
  clicks "later" or the popup doesn't appear (Discord has stolen
  focus etc.), the challenge silently sits on the hub. Add a
  badge/banner on the Hub panel header: `Challenge from X (click
  to view)`.

### Sticks Quanba PC mode bind doesn't survive game launch (?)
Verify: does an XInput-mode binding still work after closing and
reopening the launcher? If the launcher's `RefreshGamepadList`
re-enumerates differently across runs, the saved `gamepad_index`
could point at the wrong device. The binder writes the device's
position in the enumerated list, not a stable identifier. Switch
the saved index to a {GUID, name} pair so binds survive plug
order changes.

---

## Core rollback / sim

### `[EB]` opcode rendering bugs (palette flash + screen shake)
**Reported by users.** Each effect plays for 1 frame then ends;
screen shake also breaks stage parallax going forward.

**Probable cause:** our render-side state protection in
`main_loop_trampoline.cpp::RenderFrameWithSnapshot` snapshots
ObjectPool / Afterimage / InputTracking before `original_render_game`
and restores them after. The 40-byte SHAKE_EFFECTS block has a
carve-out so the timer survives, but:
- Palette flash state isn't carved → restored to "no flash" every render.
- Stage parallax progression lives somewhere we restore over.

**Investigation steps:**
1. Find palette flash state. Search FM2K binary for `g_palette_*`
   writes during render path. Likely a small struct at fixed addr.
2. Find stage parallax state. Probably part of stage-render data
   structure, possibly outside the snapshotted regions.
3. Decide: either add carve-outs (narrow), OR drop the broad
   Afterimage/ObjectPool restores entirely and lean on RNG-only
   protection. Latter is simpler if it doesn't break stress mode.
4. Test against stress mode (`FM2K_STRESS_MODE=1`) — it was the
   original reason the broad restores were added.

### Cross-machine sim desync
Observed on Strip Fighter Zero match (`f=982 onward`) and a separate
Wonderful World match — both peers show identical visible state but
divergent RNG. Implies a code path that calls `game_rand()` runs a
different number of times on each peer, somewhere earlier in the
match.

**Plan:** use the existing RNG-trace tooling (already shipped).
- `FM2K_RNG_TRACE=1` env var on both peers.
- `FM2K_PARITY_RECORD_PATH=parity.pty` for state fingerprints.
- `tools/rng_trace_diff.py` finds the first divergent call.
- Look up the caller PC in IDA — that's the function whose RNG-call
  count differs between peers.

Likely candidates: AI fallback, hit-spark random selection, audio
random pitch, character-script `[CR]` random opcodes.

### Input buffer investigation
**Suspected** by user: input buffer state may be off in some cases.
Specific symptoms not yet documented. To investigate:
- Verify `g_input_buffer_index` (0x447EE0) advances correctly on
  every sim frame (no double-tick, no skip).
- Check rollback path — when GekkoNet rolls back, does the input
  buffer index get restored to its pre-rolled-back value? Save state
  should cover this.
- Add diagnostic: log buf_idx + p1/p2 input bits on every sim tick
  for the first 600 frames of battle. Compare across peers.

### `[EB]` rendering vs sim ordering
Tangentially related to the carve-out issue: render runs AFTER
sim's update_game in our trampoline. Effects scripted at end of a
frame might be visible-but-not-applied-yet on the same frame. This
usually doesn't matter but factor it in when fixing palette/shake.

---

## Hub / matchmaking

### Active-match list polish
Currently the active matches block lives at the bottom of the Hub
panel and only renders if the room has matches. When viewing a room
with 50 idle users + 1 active match, the match scrolls off-screen.
Pin it to the top above the user list.

### Spectate button (re-enable)
Currently disabled because spectator desyncs were under
investigation. Re-enable once the cross-machine RNG desync above is
resolved (related root cause).

### Hub TLS
Currently the hub serves WS unencrypted. Long term, run behind
nginx + Let's Encrypt → wss://. Not urgent — traffic is mostly hub
control + match metadata, not chat with private content.

---

## Build / release infra

### Auto-updater silently overwrites in-place launcher
The current `FM2KUpdater.exe` extracts to `app_dir`, replacing
files. If the user has UNSAVED state in the launcher (mid-match,
mid-binding), they lose it. Mitigation: detect "match in progress"
before allowing the user to click Apply.

### Better update prompt UX
Currently the blue update pill is the only prompt. After a release
with breaking-change notes, users have no easy way to read those
notes. Add a small modal that shows the release notes (pulled from
the GitHub release body) before they click Apply.

### Auto-update doesn't sign binaries
We don't code-sign the EXE/DLL. SmartScreen will warn on first run
of a downloaded zip. Long term: get a code-signing cert (~$200/year
for an EV cert that bypasses SmartScreen entirely, or $50 for a
self-signed one that builds reputation slowly). Not blocking but
worth tracking.

---

## Known operational gotchas

- WSL drvfs `git status` is glacial — already worked around in
  `make_version.sh` (skipped dirty check). Don't reintroduce.
- Hub `tokens.json` persists across restarts — see `auth.py`.
  Deleting the file forces every signed-in user to re-Discord-auth.
- Launcher's `imgui.ini` persists window layout. Deleting it
  resets dock positions to the first-run defaults. Drop a copy of
  it in your "fresh install" testing folder so you always start
  from the default layout.
