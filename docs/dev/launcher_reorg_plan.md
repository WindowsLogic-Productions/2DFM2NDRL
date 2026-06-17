# Launcher reorg + final monolith cleanup -- plan

Post-decomposition, every file is <=1000 lines but the **launcher is a flat root
sprawl** (~78 files at repo root) while the hook is cleanly subdir'd. This plan
mirrors the hook's structure for the launcher, splits the last mixed-concern
file (`FM2K_GameInstance.cpp`), relocates non-compiled example clutter, and
brings the 900-990 "creep watch" files comfortably under the limit.

## Dependency facts (verified) that make the reorg safe
- The hook (`FM2KHook/src`) `#include`s exactly ONE root header: **`version_local.h`**
  (generated at root by make_version.sh). All `FM2K_Integration.h` / `FM95_Integration.h`
  references in the hook are COMMENTS (`netplay_battle.cpp:58` literally calls it
  "launcher-side header"). So those headers are launcher-only and may move.
- CMake has a GLOBAL `include_directories(` (line 135) with repo root on it; the
  launcher source list is a flat `add_executable` block of bare basenames (425-495).
- Quoted-include resolution checks the **including file's own directory first**, so
  same-subdir family headers (e.g. `launcher_ui_internal.h`) auto-resolve after a move.
- `_example` files are NOT in CMake (not compiled) -> free to relocate.

## Include strategy (minimal churn, low risk)
Add the new `launcher/*` subdirs to the GLOBAL `include_directories(`. Then every
quoted include resolves by basename regardless of which subdir the file moved to --
**no `#include` statement changes**. Only the CMake source-list PATHS change.
`version_local.h` stays at root (generated; on both targets' path). Pre-flight:
assert no two launcher files share a basename across subdirs (ambiguous on path).

## Target structure (mirrors FM2KHook/src)
```
launcher/
  core/       FM2K_RollbackClient + launcher_{cli,callbacks,init,frame} + process_manager
              + session_control + FM2K_Integration.h + FM2K_Launcher_decl.h
              + FM2K_LauncherUI_decl.h + FM95_Integration.h
              + FM2K_GameInstance.{cpp,h} (+ _launch.cpp + _ipc.cpp after split)
  ui/         FM2K_LauncherUI.cpp + launcher_ui_*.{cpp,h} (all 17, incl hub UI)
  hub/        FM2K_HubClient.{cpp,h} + FM2K_HubClient_{json,outbound,transport,dispatch}.cpp + _internal.h
  session/    ISession.h + OnlineSession.{cpp,h} + LocalSession.{cpp,h} + LocalNetworkAdapter.{cpp,h}
    examples/   LocalSessionExample.cpp + OnlineSession_gekkonet_sdl2_example.cpp (NOT compiled)
  discovery/  game_discovery.{cpp,h} + game_discovery_cache.cpp + game_discovery_internal.h
  game/       FM2K_GameIni.{cpp,h} + FM2K_KgtParser.{cpp,h} + FM2K_Locale.{cpp,h} + FM2K_Utf8Path.h
  net/        FM2K_PortMapper.{cpp,h} + FM2K_UploadQueue.{cpp,h} + FM2K_DiscordAuth.{cpp,h} + FM2K_Updater.cpp
  render/     FM2K_CncDDraw.cpp + FM2K_DDrawRedirect.cpp
```
Stays at root: `version_local.h` (generated), `launcher.rc`, `updater.rc`,
`CMakeLists.txt`, build scripts. Untouched: `FM2KHook/`, `tests/`, `vendored/`,
`tools/`, `docs/`, `old/`.

## Phases (each its own commit + build gate)

### P1 -- split `FM2K_GameInstance.cpp` (925) at root [proven member-fn pattern]
- **core** `FM2K_GameInstance.cpp` -- ctor/dtor/Initialize/Terminate + env helpers (UTF8ToWide, BuildEnvBlock) + SetNetworkConfig/SetEnvironmentVariable
- **`FM2K_GameInstance_launch.cpp`** -- Launch + SetupProcessForHooking + LoadGameExecutable + ExecuteRemoteFunction + InstallHooks/UninstallHooks (process spawn + injection, ~500)
- **`FM2K_GameInstance_ipc.cpp`** -- ProcessDLLEvents/HandleDLLEvent + Init/CleanupSharedMemory + the 21 Trigger*/Set*/Get*/Step*/Poll* command methods + SaveState/LoadState/AdvanceFrame/InjectInputs
Member-fn split; class in `FM2K_GameInstance.h`; no internal header. Gate: build + stress smoke.

### P2 -- 900-990 creep splits
- **launcher_ui_hub_events.cpp (953)** -> split the HandleHubEvent dispatch by event-family.
- **launcher_ui_hub_panel.cpp (929)** -> split RenderHubPanel sub-panels.
- **game_discovery.cpp (920)** -> pull engine-sniffing (HashFileXXH64/SniffEngine/GuessEngine/DetectPacker/FindKnownExe) into `game_discovery_sniff.cpp`; core keeps scan + async worker.
- **netplay.cpp (992)** + **netplay_battle.cpp (928)** [HOOK, determinism-critical] -> further per-concern split; gate under clumsy (replay + netplay 1499/1499).

### P3 -- launcher reorg (the big move) [LAST -- operates on final file set]
1. `mkdir` launcher subdirs; pre-flight basename-collision check.
2. `git mv` each root launcher file -> its subdir (preserves history).
3. CMake: add launcher subdirs to global `include_directories(`; rewrite the
   launcher source-list paths (bare basename -> `launcher/<subdir>/<basename>`);
   leave `FM2KHook/src/...`, `tools/...`, `*.rc` lines unchanged.
4. Relocate `_example` files to `launcher/session/examples/` (stay out of CMake).
5. Full build (all targets) = proof; deploy + stress gate (launcher must run a match).

## Gates
- Determinism-critical (netplay.cpp / netplay_battle): replay_selftest + replay_netplay_selftest
  1499/1499 IDENTICAL under clumsy.
- Launcher/UI/GameInstance: full build + stress smoke IDENTICAL.
- Reorg: full build (every includer recompiles) + stress smoke.
- Header families move together (umbrella + its decls in the same subdir).
