# Banner / screenshot pipeline

Goal: every game in the registry gets a clean, in-game-captured set
of screenshots (intro / title / CSS / battle / per-character) which
the stats site renders as banners and thumbs, the launcher renders
in its game picker, and the lobby renders next to active matches.

All bytes are produced by us, by running each game through the hook
and saving frames the hook already has access to. No third-party
image hosting, no scraping.

## State machine the hook drives, per game

```
spawn game (FM2K_AUTO_CAPTURE=1, FM2K_CAPTURE_DIR=...)
  │
  │  game_mode = 0 (publisher / intro logos)
  │     hold input, take screenshot every 3s for 15s
  │     → intro1.png … intro5.png
  │
  ▼
  first transition into game_mode == 1000 (title)
  │     hold for ~1s so any fade-in settles
  │     → title.png
  │     resume A-mash to enter VS-mode CSS
  │
  ▼
  first transition into game_mode == 2000 (CSS)
  │     hold for ~1s for the CSS layout to render
  │     → css_initial.png
  │     drive the player-1 cursor through every roster slot,
  │     screenshot the highlighted portrait at each
  │     → char_001.png, char_002.png, ...
  │     pick stage 0, confirm, wait for transition
  │
  ▼
  first transition into game_mode >= 3000 (battle)
  │     wait until intro fade completes (~120 frames)
  │     → battle.png
  │
  kill the game process, write a per-game manifest.json
```

## Output layout

```
dist/captures/<game_id>/
  manifest.json       # what was captured + frame numbers + game_id
  intro1.png ... intro5.png
  title.png
  css_initial.png
  char_001.png ... char_NNN.png
  battle.png
```

Native FM2K render is 320x240. Cnc-ddraw upscaling is OFF for capture
runs so the saved frames are pixel-accurate (no scaler artifacts);
the stats site upscales via CSS and the launcher picker uses
nearest-neighbor scaling.

## A post-processor picks the canonical banner per game

`tools/pick_canonical_banner.py` (post-capture):

- For each `<game_id>` directory, evaluate candidate frames against
  a quality heuristic — variance + saturation + aspect-ratio fit —
  and pick the best for **banner** (16:9, used on the stats grid)
  and **thumb** (square, used in the launcher game picker).
- Default rank: title.png > css_initial.png > battle.png > intro.
  Operator can override per-game in
  `games/registry_overrides.json` with a `banner_capture` field
  naming the chosen file.
- Output: `dist/banners/<game_id>.png` + `dist/banners/<game_id>.thumb.png`.
- Patches `games/registry.json` with the local `/static/banners/...`
  paths.

## Implementation surface

Three pieces, each small on its own.

### 1. Hook screenshot writer — `FM2KHook/src/ui/screenshot.{cpp,h}`

Writes the current-frame DirectDraw / cnc-ddraw backbuffer to PNG.
Reuses the same frame-access point as `RenderFrameWithSnapshot` and
the parity recorder. Triggered by either:

- A new shared-mem flag the hook polls (`capture_request_seq`,
  same pattern as `match_outcome_seq`), OR
- A new 0xCC control message `CAPTURE_REQUEST {filename}` that the
  launcher fires over the existing socket.

PNG encoding: stb_image_write (header-only, no new deps), 8-bit RGBA.

### 2. Launcher capture-runner — `FM2K_LauncherUI::RunCaptureSession`

Orchestrates one game:

- Spawns it via existing `FM2KGameInstance::Launch` with envs:
  - `FM2K_AUTO_CAPTURE=1` (hook switches to capture state machine)
  - `FM2K_CAPTURE_DIR=dist/captures/<game_id>/`
- Polls hook shared-mem `game_mode` + new `capture_phase`
  enum (INTRO / TITLE / CSS / BATTLE / DONE).
- Sends `CAPTURE_REQUEST` at each phase boundary.
- Kills the game when phase reaches DONE.
- Writes `manifest.json` summarizing what landed.

A new "Tools → Capture banners" menu entry runs this for every
installed game in sequence; right-click on a single game in the
list runs it for just that game.

### 3. Orchestrator — `tools/auto_capture_screenshots.py`

The build-time / batch driver:

- Reads installed games (or `games/registry.json` for game_ids only).
- Spawns the launcher with `--capture-game <game_id>` for each
  (or one big `--capture-all`).
- Validates output dir, retries failed runs.
- Hands off to `pick_canonical_banner.py`.

## Distribution

Built artifacts in `dist/banners/` go three places:

1. **Stats site** — rsync `dist/banners/` → `stats/static/banners/`
   at deploy time. cache_banners.py already mirrors during dev.
2. **Hub VPS** — `/srv/fm2k/banners/` mounted at
   `https://2dfm.sytes.net/banners/<game_id>.png`. The launcher
   fetches missing images on demand and caches locally.
3. **Launcher offline cache** —
   `%APPDATA%\FM2K_Rollback\banner_cache\<game_id>.png`. First-run
   warms over a few seconds from the hub URL; subsequent launches
   hit local disk.

## Open questions to settle when implementing

- **Per-character vs cropped portraits.** Driving the cursor 32+
  times per game is slow; cropping the individual portraits out of
  one CSS frame is faster but needs per-game CSS-layout coordinates.
  Start with the cursor approach for v1; revisit cropping if total
  capture time becomes a problem.
- **Battle frame timing.** Frame 120 (post-fade, fighters in idle)
  vs first input-able frame (~150) vs mid-fight. Idle pose is
  cleanest for a banner; capture multiple and pick.
- **Capture failure recovery.** Some games hang on intro on certain
  configs. Each capture run gets a hard timeout (~60s); on timeout
  the launcher kills + re-spawns + retries up to 3 times before
  giving up and recording the game as "no banner".
