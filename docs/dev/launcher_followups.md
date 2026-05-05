# Launcher pass — what landed + what's deferred

Companion to the broader UI/UX cleanup pass. Tracks what shipped in this
session and what's still on the docket, so we don't lose threads.

## Landed in this session

### Quick fixes
- **Input binder window title** now reads "Player 1" / "Player 2" (was
  "Player 0" / "Player 1" — 0-indexed off-by-one).
  `FM2KHook/src/ui/input_binder.cpp:918`.
- **Solo-Alt no longer freezes the game.** Discarding `WM_SYSCOMMAND` with
  `wParam == SC_KEYMENU && lParam == 0` in the trampoline's pump prevents
  Windows from entering modal menu mode when the user taps Alt. Alt+F4,
  Alt+Tab, Alt+letter (system menu accelerators) still work.
  `FM2KHook/src/core/main_loop_trampoline.cpp::PumpMessages`.

### Hub identity
- **Self appears in the lobby user list.** Self-row hides the Challenge
  button (challenging yourself isn't a thing). Tier color still applies
  so the user can confirm at a glance which tier the hub thinks they are.
  `FM2K_LauncherUI.cpp` user-table loop.
- **Editable nick + persisted across sessions.**
  - Hub: nick priority is now `launcher-supplied → discord_nick → "anon"`
    (was Discord-first). Hub also caps nicks to 32 codepoints and strips
    control chars before storing.
  - Launcher: nick input buffer bumped to 128 bytes (covers 32 UTF-8
    codepoints at worst case). Persists into `discord_auth.json` next to
    the existing token / discord_user_id fields.
- **"Use Discord name" checkbox.** Default on for fresh installs — sends
  the user's Discord global_name to the hub. Off → editable custom nick.
  Toggling preserves both values; the checkbox just picks which one ships
  on Connect. State persisted to `discord_auth.json`.

### Hub list ergonomics
- **Rooms sort by player count descending.** Stable secondary sort by
  room name so empty rooms have a deterministic order between renders.
  Sort runs on a copy so the broadcast handler can mutate `hs.rooms`
  asynchronously without racing.

### Challenge notifications
- **Three independent channels, all default-on, toggleable in
  Settings → Notifications:**
  1. **Taskbar flash** via `FlashWindowEx(FLASHW_ALL | FLASHW_TIMERNOFG)` —
     flashes the launcher's taskbar button + window caption until the
     user focuses the window. No-ops if the launcher is already focused.
  2. **Sound** via `MessageBeep(MB_ICONINFORMATION)` — uses the user's
     Windows default Asterisk system sound. No assets to ship.
  3. **Windows toast** via `Shell_NotifyIcon` balloon (NIIF_INFO). The
     OS routes balloons into the Action Center on Windows 10/11. Body
     reads "X wants to play."
- **Settings persist** in `%APPDATA%\FM2K_Rollback\settings.ini` keys
  `notify_flash`, `notify_sound`, `notify_toast` (each `1`/`0`). Loaded
  lazily on first menu-bar render.

### Repo hygiene
- **`Armonte/fm2k-hub`** private repo created and pushed
  (`hub.py + auth.py + requirements.txt + .gitignore`). Hub source is no
  longer "lives only on Armonte's home box" — it's properly versioned.
- **`Armonte/fm2k-bot`** private repo created and pushed (`bot.py +
  requirements.txt + .gitignore`).
- Both repos have `.env` in `.gitignore` — Discord secrets stay local.
  Migration doc (`docs/dev/hub_vps_migration.md`) walks through the
  bring-up sequence on a VPS.

## Deferred — needs follow-up session

### Multiple game-folder support
Right now `LauncherUI` has a single `games_root_path_` and a single
`games_root_path` settings entry. User has FM2K games scattered across
multiple folders (different drive letters, different mods kept apart) and
wants to add several roots without manually copying everything to one
place.

**Design sketch for next session:**

- Replace `games_root_path_` (string) with `games_roots_` (vector of
  strings). Persist as a multi-line block in settings.ini under a
  `[GamesRoots]` section, one path per line.
- Settings → Games Folders panel: list of paths with **Add** /
  **Remove** / **Move up** / **Move down** controls. Same scan logic
  runs across each in turn.
- Scan dedupes by canonical path so the same game showing up in two
  roots only shows once in the list (probably preferring the first-listed
  root for that game's `exe_path`).
- The launcher's existing `games_root_path` migration: on first launch
  after the upgrade, if the legacy single-folder key is present and the
  new multi-folder block is absent, copy the single value into the new
  list. Drop the legacy key once everyone has migrated.

**Surface-area estimate:** ~150 LOC in `FM2K_LauncherUI.cpp` +
`FM2K_RollbackClient.cpp` (the persistence/load path). Half a day's work,
mostly mechanical once the data shape is settled.

### Faster game scanning
Current scanner walks `games_root_path_` recursively and does string
matching on every file it sees. On a large home directory the recursion
goes everywhere — into Steam libraries, pip caches, etc. — and takes
forever for a payoff of "find one or two FM2K_Player.exe instances".

**Design sketch:**

- Cap recursion depth to 3 by default (game folders rarely live deeper
  than `<root>/<game>/<exe>`). Make it a Settings toggle if a power user
  has a deeper layout.
- Skip-list common no-fun directories early: `node_modules`,
  `.git`, `__pycache__`, `Steam/steamapps/common` (where there are no
  FM2K games anyway), `Windows`, `Program Files*`. Match folder name only
  — fast.
- Parallelize the walk across threads (one worker per top-level
  subdirectory of each games root). Discovery is I/O-bound; parallelism
  is a real win on disks that aren't mechanical.
- Cache the result. Settings.ini stores a hash of (root path, last
  modified time of the root). Only re-scan if the hash differs — the
  user explicitly clicks "Rescan" — or it's been more than 24 hours
  since last scan.

**Surface-area estimate:** ~100 LOC in the scanner + a small thread pool
helper. Pairs naturally with the multiple-folder work above; do them in
the same session to share the test surface.

### Scoreboard / W-L-D in the lobby
**Goals:**
- A scoreboard panel pinned to the top of the launcher (above Rooms),
  similar to LilithPort's bracket view. Shows recent matches with results.
- W-L-D record per opponent in the user list (you've played X, won Y, lost Z).
- W-L-D per game in the room list (you have N matches in this room).

**Hub-side changes needed:**
- New `match_result` event from peers at end-of-match: `{type:
  match_result, winner_id, loser_id, game_id, finished_at}`. Hub
  validates both peers report the same result before persisting.
- Persistent stats DB (sqlite is fine for our scale; ~thousands of users).
  Schema: `matches(id, p1_id, p2_id, game_id, winner_id, finished_at)`.
- New hub message `query_record(opponent_id?, game_id?)` returns
  match counts the launcher uses to render the W-L-D cells.

**Launcher-side changes:**
- Scoreboard panel widget with most-recent-N matches, click-to-spectate.
- W-L-D rendering in the user-list table — extra column hidden when
  there's no match history with that opponent.
- Caching layer so re-rendering doesn't spam the hub with `query_record`.

**Surface-area estimate:** ~400 LOC across hub + launcher + a small
sqlite migration. Probably 1.5 days. **Blocks on:** match-result agreement
between peers — both sides need to report the same result. This is
non-trivial during desync windows.

### Long-term: in-game lobby (game proxies launcher)
We talked about this in an earlier session — the long-term direction is
that the LAUNCHER becomes a thin shim and the GAME is the lobby client
(challenge accept auto-skips menus, sync-enters CSS). Separate from this
batch but worth tracking. Memory file: `project_in_game_lobby_direction.md`.

## Files touched this session

```
FM2K_LauncherUI.cpp         — most of the work
FM2K_Integration.h          — class members + method decls
FM2K_DiscordAuth.{h,cpp}    — CachedAuth.discord_global_name + use_discord_name
FM2KHook/src/ui/input_binder.cpp  — P0/P1 → P1/P2 fix
FM2KHook/src/core/main_loop_trampoline.cpp — solo-Alt menu suppression
hub/hub.py                  — nick priority + sanitization (32-char cap, strip control)
locales/{en,ja,es}.ini      — keys for hub_use_discord_name + notification toggles
docs/dev/launcher_followups.md  — this file
```

## How to land next

1. **Test the current build end-to-end.** Most-impactful checks:
   - Solo-Alt no longer freezes (just press Alt with nothing else).
   - Get a challenge with the launcher tabbed out — taskbar should flash
     + sound should play + Windows toast should appear.
   - Lobby user list shows you with your tier color.
   - Toggle "Use Discord name" off, type a custom nick, Connect, exit
     launcher, restart launcher → custom nick still in the field, checkbox
     still off.
2. **Commit the lot** as one or two commits on `dev`:
   - `Launcher: nick UX, challenge notifications, solo-Alt fix, lobby sort`
   - `repos: split hub + bot into private repos (fm2k-hub, fm2k-bot)`
3. **Defer the multi-folder + scoreboard work** to its own session — both
   need design + new state shapes more than they need code.
