# Roadmap: Hub Catalog Rework — Fightcade-Style Organization

**Status**: parked. Pick this up after the spectator/replay session completes. The Linux/ARM roadmap at `docs/roadmaps/linux_arm_steamos.md` is similarly parked.

## Context

The hub at http://2dfm.sytes.net:8765/ currently sources its game grid (`/games`) from `games/registry.json`, a 141-record file built ~6 months ago from the My Abandonware + Internet Archive scrape. It has no engine distinction (every record was hardcoded `engine: "FM2K"`), no studio grouping, no banners (placeholder monograms), and a single flat alphabetical grid.

We just built a much better data spine in `games/catalog.json` (101 records, 75 FM2K_2nd + 26 FM95, organized by `<engine>/<studio>/<game>` paths, with status / variants / source / IA ident, generated from the on-disk truth at `/mnt/d/games/{fm2k,fm95}/`). It's authoritative; registry.json is now a legacy match-history-key index that should retire.

Just-shipped quick fix: `tools/sync_engine_to_registry.py` overlays the engine field from catalog onto registry so the deployed hub stops mis-badging the 26 fm95 games as FM2K. That's a band-aid; the rework below replaces the underlying data flow.

The reference is Fightcade — but we want our own thing, not a clone. What we steal from Fightcade:
- Game grid grouped by **system / engine** as the primary organizer
- Each card is a **banner** (not a monogram), with metadata under it
- Per-game **landing page** with characters, top players, recent matches, online count
- **Player profiles** that link out to game pages they're active on
- **Search + filter** chips at the top

What we don't need:
- ROM verification / hash-checking on the hub (we already do that on the launcher side)
- Game-version selector (Fightcade has this for FBNeo variants — we have variants too but they're per-game install detail, not user-facing)
- Lobby chat in the browser (our chat lives in the launcher, not the web site)

## Phase 1 — Catalog as authoritative source (data plumbing)

Goal: hub reads catalog.json directly. Registry.json becomes a thin compatibility shim that maps legacy match-history `game_id` strings to catalog ids.

Changes:

1. **Add `match_id` field to catalog entries.** This is the stable string we'll use as the matches.json `game_id` value. Currently the launcher computes it from the .kgt / exe stem. Catalog's `id` field today is the relative path (`fm2k/Maiga858/Eight Marbles`) which is too verbose for matches. Add a separate `match_id` slug computed from a normalize-stem helper and verify it's unique across the catalog. Cross-reference with what the launcher actually emits (probably the lowercased exe stem). Spot-check a few entries in matches.json against the existing registry to find the canonical key.

2. **`tools/build_game_catalog.py` writes match_id.** Same generator that already produces 101 entries; just populate one more field. After this, catalog.json is sufficient on its own.

3. **`stats/app.py:load_registry()` becomes `load_catalog()`.** Rename, switch source path to `games/catalog.json`, restructure the in-memory shape: index by `match_id` for the `_games_grid_data` hot path. Keep `load_registry()` as a deprecated alias that reads the same file via the catalog → registry adapter.

4. **Registry → catalog migration table.** A small one-shot tool `tools/migrate_match_ids.py` that walks `hub/matches.json`, looks up each match's old `game_id` in registry, finds the corresponding catalog entry by exe_stems / kgt_filename, and updates the match record to point at the new catalog `match_id`. Idempotent; can run repeatedly. Backup matches.json first.

After Phase 1: hub serves the catalog data, fm95 / fm2k_2nd badges are real, no more mis-classification. Match history continues to work via the migration table; new matches use the canonical match_id directly.

## Phase 2 — UI rework: Fightcade-style grid

Goal: replace the flat alphabetical grid with engine-grouped sections + filters + better cards.

Changes:

1. **Three-section grid** at `/games`:
   - **FM2K_2nd** (75 games, primary section — these are rollback-compatible)
   - **FM95** (26 games, secondary — different engine, no rollback yet)
   - **Tools / SDK** (the `2D Fighter Maker` engine releases themselves — out of catalog right now, may want a third-bucket for those if we move them in)
   Each section collapsible, with a per-section game count.

2. **Studio sub-grouping** within each engine section. Maiga858 with 6 Eight Marbles games → one studio block with 6 cards; _NODEV games sorted alphabetically at the end. Studio blocks have a small header with: studio name, game count, link to a `/studio/<name>` page (Phase 3).

3. **Filter chips at the top of `/games`**:
   - Engine: All | FM2K_2nd | FM95
   - Status: All | Active (has matches) | Locked / Broken
   - Source: All | MyAbandonware | archive.org
   - Search box (live-filters by name + alt_names, fuzzy)
   The chips are URL-state (`/games?engine=fm2k_2nd&active=1`) so links are shareable.

4. **Card improvements**:
   - Replace monogram fallback with a stable **deterministic gradient** keyed off match_id (current code partially does this with `banner_color(game_id)`). Better: ship per-game banners (the auto-capture pipeline at `docs/dev/banner_pipeline.md` is the long-term path; manual placement is the short-term).
   - Show **studio name** under the game name on each card.
   - Show **variant count** badge if multi-variant ("3 versions" for Eight Marbles 2X).
   - Show **status indicator** (✅ ready / 🔒 locked / ⚠️ no_exe / ⚙️ packed) — currently catalog has these statuses but they're not surfaced.
   - **Hover state**: card lifts, shows last-played-at, shows match count more prominently.

5. **Sort options**: Most active | Recently played | Alphabetical | Year (newest) | Year (oldest). Current sort is alphabetical-only.

## Phase 3 — Per-game + per-studio pages

Goal: deepen each game page from the current basic view, add studio aggregation pages.

Changes:

1. **`/g/<match_id>` (existing page) — additions**:
   - Engine + studio + variant info from catalog
   - Variant list as tabs (Eight Marbles 2X → file-1 / Updated Version / Cleaned exe; click to see notes per variant)
   - "Other games by Maiga858" sidebar (links to other studio games)
   - Direct download link (when it's an IA item with a public URL)
   - Banner from `dist/banners/<game_id>.png` if present (existing pipeline)
   - "Currently playing" widget if the hub reports active matches on this game

2. **`/studio/<studio_name>` (NEW)**:
   - Header: studio name, total games count
   - Grid of all games by this studio (engine-tagged)
   - Total match count across studio's games
   - Top players who play this studio's games most

3. **`/engine/<fm2k_2nd|fm95>` (NEW)**:
   - Same layout as `/games?engine=X` but as a permalinkable URL
   - Description of what the engine is, FM95 page can flag "rollback not yet supported"

## Phase 4 — Banner pipeline integration

Goal: replace placeholder monograms with real per-game banners. The auto-capture pipeline at `docs/dev/banner_pipeline.md` is partially scaffolded (hook-side screenshot writer at `FM2KHook/src/ui/screenshot.cpp` exists, orchestrator at `tools/auto_capture_screenshots.py` is scaffolded but blocked on a launcher `--capture-game` flag).

Changes:

1. **Implement the launcher `--capture-game <match_id>` flag.** Hands the game-dir to the hook via env vars `FM2K_AUTO_CAPTURE=1` + `FM2K_CAPTURE_DIR=...` (already set up on the hook side), waits for the sentinel `.capture_done` file, exits.

2. **Run the orchestrator across the catalog.** `python3 tools/auto_capture_screenshots.py --catalog games/catalog.json` boots each game in turn, captures intro/title/css/battle frames, drops them in `dist/captures/<match_id>/`.

3. **Pick canonical banner per game.** New tool `tools/pick_canonical_banner.py` evaluates the captured frames, picks one for `dist/banners/<match_id>.png` and a square thumb for the launcher game picker. Heuristics: variance + saturation + aspect-ratio fit, default rank title > css > battle > intro.

4. **Patch catalog.json with banner_url + thumb_url** so the hub renders them instead of monograms. `tools/cache_banners.py` already exists for this — point it at catalog instead of registry.

5. **Hub serves banners as static**: rsync `dist/banners/` → VPS `/srv/fm2k/banners/` at deploy time. The launcher fetches missing images on-demand and caches in `%APPDATA%\FM2K_Rollback\banner_cache\<match_id>.png`.

## Phase 5 — Per-character art (long tail)

Out of scope for first rework pass but flagged. Each .player file has the character's portrait inside (FM2K convention). Once we have the .player parser working we can extract char art + render character pages with portraits, attach to /character/<game>/<char_id> URL. Current task #51 (Resolve char IDs to .player filenames) is upstream of this.

## What we deliberately won't do

- **Move matches.json into a database.** It's 650 KB, fits in memory comfortably. SQLite or Postgres adds ops burden for no win at our scale.
- **Real-time WebSocket "X playing now" updates on the hub.** The hub already pushes lobby state to the launcher; reflecting it on the public web site is nice-to-have but rebuilds when someone clicks Refresh covers 95% of users.
- **Generic "fighting game stats" framework.** This is FM2K-specific by design. Don't over-abstract for SF / Tekken / etc. — those have their own tools.
- **Migrate registry.json content to catalog.json.** Registry has legacy match-history mappings + things like `download_url` that catalog doesn't. Keep them as adjacent files; catalog wins for game list, registry kept for what it's good at.

## Files to modify (per phase)

**Phase 1:**
- `tools/build_game_catalog.py` (add match_id field generation)
- `stats/app.py:load_registry → load_catalog` + the few callsites
- `tools/migrate_match_ids.py` (NEW one-shot)
- `hub/matches.json.bak.*` (backup before migration)

**Phase 2:**
- `stats/app.py:_games_grid_data` (return engine-grouped buckets, filter params)
- `stats/templates/games_grid.html` (sectioning, filters, sort dropdown)
- `stats/static/css/...` (engine badge colors, studio block separators)

**Phase 3:**
- `stats/templates/game.html` (variant tabs, studio sidebar)
- `stats/templates/studio.html` (NEW)
- `stats/templates/engine.html` (NEW)
- `stats/app.py` (`/studio/<name>` + `/engine/<id>` routes)

**Phase 4:**
- `FM2K_RollbackClient.cpp` — implement `--capture-game` CLI flag
- `tools/pick_canonical_banner.py` (NEW)
- `tools/cache_banners.py` (re-target from registry → catalog)
- `dist/banners/*` (build artifact)

**Phase 5:**
- `tools/parse_player_files.py` (NEW — extract char portraits)
- `stats/templates/character.html` (existing — surface portrait)

## Existing utilities to reuse

- **`games/catalog.json`** — already authoritative, generated by `tools/build_game_catalog.py`. The hub just needs to point at it.
- **`tools/sync_engine_to_registry.py`** — the band-aid we just shipped. Deprecate after Phase 1 lands.
- **`stats/app.py:_games_grid_data`** — current grid data shape can be extended (don't rewrite from scratch).
- **`tools/cache_banners.py`** — existing banner-fetcher, just needs catalog support.
- **`docs/dev/banner_pipeline.md`** — already-detailed plan for in-game capture.
- **Hub `matches.json`** — keep its schema, only the `game_id` lookup-key narrative changes.

## Verification

- After Phase 1: load `/games` on the staging hub — fm95 games show with FM95 badge, total count = 101 (catalog) not 141 (legacy registry). All existing match-history pages (`/g/pkmncc`, etc.) keep working.
- After Phase 2: shareable URL like `/games?engine=fm2k_2nd&studio=Maiga858` filters correctly and renders only those games.
- After Phase 3: `/studio/Maiga858` lists all 6 Eight Marbles games. `/engine/fm95` lists all 26 fm95 games with the "rollback not supported yet" note.
- After Phase 4: at least 80% of catalog entries have a real banner instead of monogram. Launcher game picker shows the same banners.
- After Phase 5: at least the top 5 most-played games have character portraits surfaced on `/character/<game>/<id>`.
