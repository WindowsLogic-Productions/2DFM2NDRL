# Monolith decomposition plan (no 1000+ line files)

Goal: every source file human-readable and categorized -- no 1000+ line files,
and ideally no giant single functions either. Method:

- **Pure behavior-preserving moves** by default: cut a cohesive function group
  verbatim into a sibling `.cpp`, share file-scope state via an `*_internal.h`
  (extern the statics, `inline constexpr` the consts, named-namespace generic
  helpers). Build-green is the gate; the linker catches linkage mistakes.
- **Function-factoring** only where a *single function* is itself >1000 lines (a
  pure move can't shrink it). That touches control flow, so it is NOT a pure
  move -- flagged per-file below, and for hook-side code must be netplay-tested.
- Determinism-critical hook files (hooks/savestate/charstate/trampoline/
  control_channel/per_game_patches) additionally get a netplay smoke-test
  (host + spectator, a match + rematch, + one offline `.fm2krep`) before relying
  on them. Launcher/UI files just need build-green + launch.

This doc is the resume point. The proven mechanics + gotchas are in
`docs/dev/refactor_handoff.md` (read that first).

## Done + pushed
- `spectator_node.cpp` 5436 -> 558 (12 files).
- `netplay.cpp` 4458 -> 992 (9 files; `netplay_battle_phase.cpp` 1188 is one
  984-line function -- function-factor pending, see below).
- `FM2K_RollbackClient.cpp` 4071 -> 482. Now: `launcher_cli.cpp` 573,
  `launcher_callbacks.cpp` 555, `process_manager.cpp` 518, `launcher_init.cpp`
  449, `launcher_frame.cpp` 396, `session_control.cpp` 203, +
  `game_discovery.cpp` 1191 (still over -- see below).

## Remaining targets

Line counts are current. "DET" = determinism-critical (netplay-test each cluster).

### game_discovery.cpp (1191, launcher) -- SMALL
Split off the config+cache I/O cluster (`GetConfigDir`, `Load/SaveGamesRootPaths`,
`GetCacheFilePath`, `StatFile`, binary read/write helpers, `SaveGameCache`,
`LoadGameCacheMap`, `HashFileXXH64`, `FindKnownExe`) into `game_discovery_cache.cpp`
(~250). Needs `game_discovery_internal.h` to share the static `Utils::` helpers the
scan code calls. Result: game_discovery.cpp ~940, cache ~250.

### hooks.cpp (3850, DET) -- the injected hook core
7-file split via `hooks_internal.h` (extern the ~40 `original_*`/`g_*` statics,
namespace the generic VFS helpers). Clusters:
- `hooks_vfs.cpp` (~650): CreateFile*/ReadFile/SetFilePointer*/CloseHandle + FPK
  redirect + player-vfile slurp + prefetch.
- `hooks_input.cpp` (~950 AFTER factoring): SOCD, autoplay, capture, and
  `Hook_GetPlayerInput` (645 lines -- factor its source-selection branches into
  static helpers; this is the one function-factor in this file).
- `hooks_game_mode.cpp` (~630), `hooks_render.cpp` (~680: FPS/title/sound/blit),
  `hooks_update.cpp` (~270: Hook_UpdateGameState), `hooks_rng.cpp` (~120).
- `hooks.cpp` keeps FPU/determinism + ProcessGameInputs + InitializeHooks (~650).

### savestate.cpp (2292, DET) -- rollback save/load
7-file split via `savestate_internal.h` (extern `g_state_buffer`,
`g_region_checksums`, `g_replay_saves`, RNG-trace ring; keep `Fletcher32` static
per-TU -- it already collides by-name with spec_transport's). FM95/FM2K bodies are
`#if ENGINE_FM95` gated, so only one compiles -> no single TU >1000:
- `savestate_save.cpp` (~620, Save FM95+FM2K), `savestate_load.cpp` (~550),
  `savestate_checksum.cpp` (~180), `savestate_snapshot.cpp` (~120),
  `savestate_diagnostic.cpp` (~330), `savestate_rngtrace.cpp` (~55),
  `savestate.cpp` keeps lifecycle + shared infra (~450).

### main_loop_trampoline.cpp (1697, DET) -- per-frame driver
Clean pure-move into phase-tick siblings via `trampoline_internal.h` (extern
render-snapshot buffers + spectator constants):
- `trampoline_render.cpp` (~380: RenderFrameWithSnapshot + EbDiag_Dump),
  `trampoline_spectator.cpp` (~480: SpectatorSimOneFrame + RunSpectatorTick),
  `trampoline_battle.cpp` (~280), `trampoline_pacing.cpp` (~120),
  `trampoline_css.cpp` (~80), `trampoline_native.cpp` (~100).
- main_loop_trampoline.cpp keeps ClassifyPhase + PumpMessages + entry points (~200).
No function >1000; pure moves only.

### input_binder.cpp (1592, launcher+hook) -- 4 files
`input_binder_gamepads.cpp` (~270 SDL3 discovery), `input_binder_profiles.cpp`
(~220 INI/per-game), `input_binder_win32.cpp` (~170, `#ifdef _WIN32` XInput
sampler), core stays ~640. Shared state via `input_binder_internal.h` (extern
`g_players`, `g_capture`, gamepad maps, `g_config_path`). `RenderBody` (400) is the
largest remaining function -- fine.

### per_game_patches.cpp (1366, DET) -- 5 files + internal.h
By patch family: `_damage.cpp` (215), `_kof.cpp` (173), `_input.cpp` (398),
`_ai.cpp` (199), `_btb.cpp` (469). Shared atomics (`g_vs_cpu_mode` etc., used by
input+ai clusters) -> `per_game_patches_internal.h`. ENGINE_FM95 stubs stay in
per_game_patches.cpp. Largest function 187 lines.

### FM2K_HubClient.cpp (1352, launcher) -- 5 files, NO internal header
All member-fn moves (state is class members): `_JsonHelpers.cpp` (~210, pure
fns -- prefix or file-local-namespace `GetStr`/`GetInt` to avoid collisions),
`_Outbound.cpp` (~315 senders), `_Dispatch.cpp` (~440 OnMessage), `_Transport.cpp`
(~270 WinHTTP IoThread+lifecycle), stub ctor/dtor stays. Cross-thread invariants
(atomics + out/in mutexes) preserved -- just moving method bodies.

### control_channel.cpp (1187, DET) -- 4 files via internal.h
`_core.cpp` (~400 socket+keepalive+state), `_io.cpp` (~300 RawSend/RawReceive/
Poll), `_send.cpp` (~280 convenience sends + RTT), `_gekko.cpp` (~150 multiplex
adapter). Heavy shared state (`g_socket`, `g_poll_mutex`, queues) -> internal.h.
Namespace `RawSend`/`RawReceive`/`GetTimeMs`.

### imgui_overlay.cpp (1080, DET-ish UI) -- 2 files
Split on the render seam: `imgui_overlay_core.cpp` (~380: init/hotkey/lifecycle/
window-find/DirectXInit) + `imgui_overlay_render.cpp` (~700: Hook_EndScene 307 +
RenderDebugOverlay 268 + viewport math + Hook_Reset/WndProc). Shared statics ->
`imgui_overlay_internal.h`.

### dllmain.cpp (1030, DET) -- RECOMMEND LEAVE or trim lightly
Only 30 over, and it's a cohesive DLL boot/teardown. Optional: move the game-side
binary patches (`BypassMultiInstanceCheck`/`ApplyBootToCharacterSelectPatches`/
`DisableCursorHiding`/`FixGameSpeedDesync`/`ApplyPerGameRuntimePatches`, ~170) to
`dllmain_patches.cpp` -> dllmain ~860. Low value; do last or skip.

### charstate.cpp (1801, DET) -- SPECIAL: mostly ONE function
`character_state_machine` is a single ~1800-line decompiled function; its
~935-line script-opcode `switch` (lines ~655-1789) is **irreducible by pure
move**. Splitting needs real function-factoring (parameterize `v0`/object ptrs,
convert intra-case gotos) which is behavior-sensitive RE work + netplay test.
NOTE: appears NOT in any CMakeLists build target (verify -- may be a reference
decomp, not compiled). If uncompiled, skip entirely. Decide before touching.

### FM2K_Integration.h (1305, header) -- shared types
Header, not a .cpp. Could split the struct/enum/class decls into
`FM2K_Integration_types.h` + keep the launcher API. Low urgency; touches many
includers. Defer.

### FM2K_LauncherUI.cpp (8036, launcher) -- BLOCKED
DO NOT split on bleeding -- collides with the user's unfinished UI-rewrite branch.
Either do it on that branch or let the rewrite absorb the split. The per-frame
loop machinery the UI-perf fix needs is already OUT of this file (see below);
what's left here is the actual panel-drawing widgets.

## Recommended order
1. game_discovery (finish the launcher family; safe). 
2. main_loop_trampoline + control_channel (clean pure-move DET wins).
3. savestate + per_game_patches + hooks (DET, bigger; netplay-test each).
4. input_binder + FM2K_HubClient + imgui_overlay (mixed; mostly mechanical).
5. dllmain (optional), FM2K_Integration.h (defer), charstate (decide first),
   FM2K_LauncherUI (rewrite-branch).

## UI/debug-perf note (where the perf code lives after the RollbackClient split)
The event-driven-repaint perf machinery is now in small files:
- **Tier decision / render-skip** (MINIMIZED/BACKGROUND_GAME/IDLE_VISIBLE/
  ACTIVE, the `g_ui_dirty` + activity-clock logic): `SDL_AppIterate` in
  `FM2K_RollbackClient.cpp` (482 lines, easy to read).
- **IPC-critical slice that must keep ticking when paint is skipped**:
  `FM2KLauncher::Update` in `launcher_frame.cpp`.
- **The skippable ImGui build+present** (incl. multi-viewport): `Render` in
  `launcher_frame.cpp`. `UiWantsContinuousRedraw` also there.
- **Event->dirty stamping**: `SDL_AppEvent` in `FM2K_RollbackClient.cpp`.
- The actual widget drawing + `LauncherUI::WantsContinuousRedraw` still live in
  the 8036-line `FM2K_LauncherUI.cpp` (blocked). So the loop layer is clean; the
  draw layer waits on the UI rewrite.
See `docs/dev/ui_perf_software_renderer_investigation.md` for the parked hypothesis.
