# Hook-side decomposition plan (FM2K/FM95-aware)

Continuation of the monolith breakdown, now on the determinism-critical hook
DLL. The launcher side is done (RollbackClient 4071->482, LauncherUI 8036->618,
both harness-validated). This plan covers the hook batch the user named:
hooks.cpp, savestate.cpp, main_loop_trampoline.cpp, per_game_patches.cpp.

## The two-axis rule (the "be smart" part)

Both FM2KHook.dll and FM95Hook.dll compile ONE shared source list
(FM2KHook/CMakeLists.txt `FM2KHOOK_SOURCES`); FM95Hook just adds `ENGINE_FM95=1`.
So a file's body can be `#if defined(ENGINE_FM95)` (active only in FM95Hook) or
`#if !defined(ENGINE_FM95)` (FM2K) -- the "wrong" engine just gets an empty TU.

Split on TWO axes, but only use the engine axis where it actually earns its keep:

- **Concern axis (ALWAYS):** cut cohesive function groups into per-concern TUs,
  sharing file-scope state via a `*_internal.h` (extern statics, inline-constexpr
  consts, named-namespace generic helpers). Same proven pattern as the launcher.
- **Engine axis (ONLY where there's a real dual-impl or engine-exclusive body):**
  put FM2K-only code in `#if !defined(ENGINE_FM95)` files and FM95-only code in
  `#if defined(ENGINE_FM95)` files, so FM95 (still WIP) lives in obvious, finishable
  `*_fm95.cpp` files. Do NOT engine-split files that are mostly shared with a few
  inline `kIsFM95` constexpr branches -- that just makes near-empty files and
  scatters shared logic. Keep those branches inline.

Measured engine-coupling (grep, 2026-06-17):
- main_loop_trampoline.cpp: 0 ENGINE_FM95, 0 kIsFM95 -> CONCERN-ONLY.
- savestate.cpp: 1 top-level `#if ENGINE_FM95` = full dual impl -> CONCERN + ENGINE.
- per_game_patches.cpp: whole FM2K body under `#if !ENGINE_FM95` + FM95 stubs -> ENGINE-exclusive split + concern-split the FM2K body.
- hooks.cpp: 0 #if, 11 inline kIsFM95 -> CONCERN-ONLY, engine branches stay inline.

## Per-file plan

### 1. main_loop_trampoline.cpp (1697) -- engine-agnostic, lowest risk, DO FIRST
Pure concern-split (no engine files) via `trampoline_internal.h` (extern the
render-snapshot buffers + spectator constants; inline RAII if any):
- trampoline_render.cpp (~380: RenderFrameWithSnapshot + EbDiag_Dump)
- trampoline_spectator.cpp (~480: SpectatorSimOneFrame + RunSpectatorTick)
- trampoline_battle.cpp (~280: RunBattleTick), trampoline_pacing.cpp (~120),
  trampoline_css.cpp (~80), trampoline_native.cpp (~100)
- main_loop_trampoline.cpp keeps ClassifyPhase + PumpMessages + TrampolineFrameTick
  + TrampolineMainLoop (~200). All <1000, pure moves.
Reason for first: zero engine complexity + re-confirms the harness catches a
hook-side change before we touch determinism-heavier files.

### 2. savestate.cpp (2292) -- the engine-split showcase
Structure today: `#if defined(ENGINE_FM95)` <FM95 save/load/...> `#else` <FM2K
save/load/...> `#endif`, around shared lifecycle/infra.
- savestate_internal.h: shared globals (g_state_buffer, g_region_checksums,
  g_replay_saves, RNG-trace ring, profiler SBucket/SScope inline, addr constants).
  Keep Fletcher32 `static` per-TU (collides by-name with spec_transport's).
- savestate.cpp (keep, engine-agnostic): Init / DoInitialSync / PatchPostRenderRng /
  GetPostRenderRng / GetSlotByteSize / PeekLastSavedSlotBytes / LoadFromBytes +
  RNG-trace push/flush (~450). VERIFY each is truly outside the `#if` before keeping.
- savestate_fm95.cpp (`#if defined(ENGINE_FM95)`): FM95 Save/Load/CalcFingerprint/
  CalcFullChecksum/Capture/Compare/TestRoundtrip/DumpDesync/GetRegionChecksums
  (~258, ONE file -- the FM95 savestate home).
- savestate_fm2k_save.cpp (`#if !defined(ENGINE_FM95)`, ~620: Save FM2K),
  savestate_fm2k_load.cpp (~550: Load FM2K),
  savestate_fm2k_diag.cpp (~600: fingerprint/full-checksum/snapshot/roundtrip/
  DumpDesyncDiagnostic FM2K). FM2K side is too big for one file, so concern-split it.

### 3. per_game_patches.cpp (1366) -- FM2K body + FM95 stubs
- per_game_patches_internal.h: shared atomics (g_vs_cpu_mode etc. used by input+ai).
- FM2K body, each `#if !defined(ENGINE_FM95)`, concern-split per the earlier map:
  _damage.cpp (215), _kof.cpp (173), _input.cpp (398), _ai.cpp (199), _btb.cpp (469).
- per_game_patches_fm95.cpp (`#if defined(ENGINE_FM95)`): the no-op stubs for all
  public PerGamePatches_* fns. This becomes the FM95 patch home as FM95 matures.
- per_game_patches.cpp shrinks to just the shared install entry (if any) or is
  retired into the above. Largest fn ~187 lines.

### 4. hooks.cpp (3850) -- biggest, mostly shared, DO LAST (best with FRESH context)
Concern-split via `hooks_internal.h`. Engine differences stay INLINE (`if constexpr
(FM2K::kIsFM95)`, 11 sites -- compile-time, no engine files needed).
KEY DE-RISK (avoids the one risky function-factor): put **Hook_GetPlayerInput (645
lines) in its OWN file** `hooks_getinput.cpp` -- it's already <1000 alone, so NO
factoring of its source-selection logic is needed. Then SOCD+autoplay+capture go in
hooks_input.cpp (~536). All clusters end up <1000 with PURE moves.

`hooks_internal.h` contents (the cross-TU surface):
  - extern the ~15 `original_*` trampoline pointers (set by InitializeHooks in the
    hooks.cpp shell, CALLED by the Hook_* detours in the cluster TUs).
  - declare the Hook_* detours that InitializeHooks installs by address (so the
    shell can `MH_CreateHook(target, &Hook_X, &original_X)` across TUs).
  - a few cross-cluster g_* (g_last_game_mode etc.) -- most statics are
    CLUSTER-LOCAL (VFS ~18, render ~12, rng ~5) and travel with their TU (stay
    static/anon, NOT in the header). Namespace the generic VFS helpers (VFile,
    ends_with_asset, ReadWholeFileFresh) inside hooks_vfs.cpp's anon namespace.
Files: hooks_vfs.cpp (~650), hooks_getinput.cpp (Hook_GetPlayerInput, 645),
hooks_input.cpp (SOCD+autoplay+capture, ~536), hooks_game_mode.cpp (~630),
hooks_render.cpp (~680), hooks_update.cpp (~270), hooks_rng.cpp (~120); hooks.cpp
keeps FPU/determinism + ProcessGameInputs + InitializeHooks (~650). NO function-factor.
Sequence: build hooks_internal.h first, then extract the self-contained leaves
(rng, update, vfs) one commit at a time (build+harness each), GetPlayerInput +
input + game_mode + render last. Harness gate (replay + netplay) after EACH.

## STATUS (2026-06-17, committed, NOT pushed)
DONE + harness-green (replay + netplay both 599/599 IDENTICAL after each):
  - main_loop_trampoline.cpp 1697 -> 334 + 6 phase TUs + trampoline_internal.h.
  - savestate.cpp 2292 -> common 299 + fm95 + fm2k_{save,load,diag} + internal.h (ENGINE SPLIT).
  - per_game_patches.cpp 1366 -> input/modes + battle + fm95 stubs + internal.h (ENGINE SPLIT).
REMAINING (this batch): hooks.cpp (above). Recommended fresh context -- it's the
core hook plumbing (49 statics, ~40 detours); a context-constrained half-split is
the one dangerous outcome. Then the non-batch leftovers (control_channel 1187,
imgui_overlay 1080, dllmain 1030, input_binder 1592, FM2K_HubClient 1352,
game_discovery 1191, netplay_battle_phase 1188 single-fn) per monolith_decomposition_plan.md.

## Harness gate (run after EACH file's split, before moving on)
1. Build BOTH DLLs green: `./make_build.sh && (cd build && ninja)`.
2. FM2K determinism: `python3 tools/replay_selftest.py --frames 600`
   -> expect "ALL ... ALIGNED FRAMES IDENTICAL".
3. Netplay determinism: `python3 tools/replay_netplay_selftest.py --frames 600`
   -> expect "ALL ... ALIGNED FRAMES IDENTICAL".
   (Both proven green on the launcher refactor 2026-06-17.)
4. FM95: build-green of FM95Hook.dll is the gate (FM95 is WIP; no clean FM95
   determinism harness yet -- note if/when an FM95 test game is wired).
A divergence or build break = stop, bisect to the last green commit, fix.

## Discipline (lessons paid for)
- Slice boundaries: a function ends at its `}`, NOT the next `// ===` banner.
  The off-by-N bit me 3x on the launcher. Verify exact end lines, or use a
  brace-aware extractor (tools/split_launcher_ui.py is a template).
- One file (or one safe cluster) per commit; build + harness green is the gate.
- Engine guards: every FM2K-only TU body wrapped `#if !defined(ENGINE_FM95)`,
  every FM95-only TU body `#if defined(ENGINE_FM95)`. Verify the empty-TU side
  still compiles (it will -- empty after preprocessing).
- Order: trampoline -> savestate -> per_game_patches -> hooks.
- Concern-cluster line maps for all four are in docs/dev/monolith_decomposition_plan.md.
