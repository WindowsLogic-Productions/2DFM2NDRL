# Banner / screenshot pipeline

## Why we don't scrape mizuumi.wiki for banners

Mizuumi is operated by a friend of the project. Bulk-downloading
their image hosting — even for a CC-BY-SA-licensed wiki — is rude
when we have a clean alternative: capturing screenshots from the
games themselves. Doing our own captures gives us:

1. **Standardized presentation.** Every card has an in-game frame at
   the same resolution / aspect ratio.
2. **No third-party dependency.** Mizuumi can rotate filenames,
   change CDNs, deduplicate logos — none of that affects us once we
   own the bytes.
3. **No copyright / licensing ambiguity.** Game devs uploaded their
   work to be played; an in-game screenshot of running their game
   is settled fair-use territory the way a wiki re-host isn't.
4. **Coverage matches reality.** We get banners for every game in
   the launcher, not just the ~13 that happen to have wiki entries.

The earlier `tools/cache_banners.py --source mizuumi` path is left
gated behind `--i-have-permission` for emergency use only; CI never
runs it.

## Design: hook-driven auto-capture

The hook already has everything we need:

- A frame access point (the parity recorder + replay capture both
  use the rendered framebuffer at end-of-frame — same hook taps
  let us write a PNG instead).
- An input override layer (`AutoTitleSkip` already mashes A through
  intro / title / menu to reach CSS — we just need to pause it at
  each interesting state and emit a screenshot).
- A clean per-game-mode boundary (`game_mode_changed: 0 → 1000 →
  2000`) so we know exactly when the title or CSS first becomes
  visible.

State machine the script drives, per game:

```
spawn game  ─┐
             │  game_mode = 0  (intro / publisher logos)
             │     wait 15s, screenshot every 3s        → intro1.png … intro5.png
             │
             ▼
      first time game_mode == 1000 (title)
             │     hold for 1s, screenshot              → title.png
             │     then hand back input, mash A
             │
             ▼
      first time game_mode == 2000 (CSS)
             │     hold for 1s, screenshot              → css.png
             │     advance cursor through every slot,
             │     screenshot each portrait position    → char_<id>.png
             │     pick a stage, advance to battle
             │
             ▼
      first time game_mode >= 3000 (battle)
             │     screenshot first frame after intro   → battle.png
             │
      kill the game process
```

Output layout per game (driven by the launcher's `game_id`):

```
dist/captures/<game_id>/
  intro1.png    intro2.png    intro3.png  ...
  title.png
  css.png
  char_001.png  char_002.png  ...
  battle.png
```

A separate post-processor script then picks the **canonical banner**
for each game. Heuristic: title screen is usually the most readable
"this is what the game is" image; fall back to CSS when title is
just a logo. Operator can override per-game in
`games/registry_overrides.json` with a `banner_capture` field
naming the chosen file.

## Implementation surface

Three pieces, all driven from the existing launcher + hook:

### 1. New hook command — write framebuffer to PNG

`FM2KHook/src/ui/screenshot.{cpp,h}` (new). Reuses the
DirectDraw / cnc-ddraw frame access we already have for
`RenderFrameWithSnapshot`. Writes to a path supplied via env var
`FM2K_CAPTURE_DIR`. Triggered by:

- A new `0xCC` control message `CAPTURE_REQUEST {filename}` that
  the launcher can fire over the existing socket.
- OR a shared-mem flag the launcher polls — same pattern as the
  match-result publish path.

### 2. New launcher mode — capture-runner

`FM2K_LauncherUI` gains a `RunCaptureSession(game)` that:

- Spawns the game via the existing `FM2KGameInstance::Launch` path
  with the new env var `FM2K_CAPTURE_DIR=dist/captures/<game_id>/`.
- Sets `FM2K_AUTO_CAPTURE=1` so the hook knows to use the capture
  state machine instead of normal autostart.
- Polls the hook's published state (`game_mode` + a new
  `capture_phase` enum in shared mem).
- Sends `CAPTURE_REQUEST` at each phase boundary.
- Kills the game when phase reaches BATTLE_DONE.

A "Run captures" menu entry in the launcher invokes this for every
installed game in sequence. Manual per-game button for one-offs.

### 3. New tool — auto_capture_screenshots.py

Thin orchestrator that:

- Reads `games/registry.json` and the launcher's installed-games
  list.
- Spawns the launcher with `--capture-game <game_id>` for each.
- Waits for completion, validates the output dir has the expected
  files.
- Picks canonical banner / thumb (title preferred, css fallback).
- Resizes via Pillow if available (or copies as-is).
- Writes outputs to `dist/banners/<game_id>.png` + `.thumb.png`.
- Patches `games/registry.json` with `banner_url` / `thumb_url`
  pointing at the local `/static/banners/` paths.

## Hub upload / CDN flow (later)

Once `dist/banners/` is populated, the build / release script
rsyncs it to:

- `stats/static/banners/` (already mirrored automatically by
  cache_banners.py for dev)
- A public `/srv/fm2k/banners/` mount on the hub VPS, served behind
  nginx at `https://2dfm.sytes.net/banners/<game_id>.png`
- The launcher's local `%APPDATA%\FM2K_Rollback\banner_cache\<game_id>.png`
  for offline picker thumbnails

The launcher fetches missing banners from the hub URL on demand and
caches them, so a fresh install warms over a few seconds and stays
local thereafter.

## Open questions

- How many distinct CSS character-portrait captures do we want?
  Some games have 32 chars (pkmncc), some have 50+. Cropping the
  individual portraits out of the CSS frame is more efficient than
  driving the cursor 32 times.
- Resolution. FM2K's native render is 320x240; cnc-ddraw can
  upscale to 1280x960. The launcher should pin to native for
  capture and let the renderer / stats site upscale via CSS.
- Battle frame — first frame after the intro fade (probably fr=120
  or similar) gives a clean character pose; first input-able frame
  shows a more chaotic shot. Both have value; capture both.
