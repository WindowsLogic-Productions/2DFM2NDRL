# Refactor handoff: remaining oversized files

Next-session task: finish splitting the big source files into small, human-readable,
per-concern files. NO 1000+ line monoliths. This doc is the resume point.

## Goal + rules

Pure MECHANICAL, behavior-preserving splits: cut cohesive function groups verbatim
out of a god-object .cpp into sibling TUs that share state via an internal header.
- Build-green is the gate. Identical code can't change behavior; the linker catches
  linkage mistakes. Commit one cluster per commit (revertable, bisectable).
- For determinism-critical HOOK code (hooks.cpp), ALSO netplay smoke-test (host +
  spectator, a match + a rematch, + one offline .fm2krep replay) before relying on it.
- No em dashes in commits/comments (use --). Keep new files' comment style matching
  the surrounding code.

## Already done this session (bleeding, pushed, origin ccbc361)

- spectator_node.cpp 5436 -> 558. Split into spec_wire / spec_transport /
  spec_backfill / spec_health / spec_recv / spec_host_events / spec_join /
  spec_session_file / spec_playback + spectator_node_internal.h. ALL <850 lines.
- netplay.cpp 4458 -> 992. Split into netplay_chat / netplay_spectate /
  netplay_barriers / netplay_css / netplay_control / netplay_battle /
  netplay_battle_phase + netplay_internal.h. All <1000 EXCEPT netplay_battle_phase.cpp
  (1188), which is the single ~984-line Netplay_ProcessBattleInputPhase rollback
  driver -- irreducible via pure moves (see "function-refactor" note below).
- game_discovery split out of FM2K_RollbackClient.cpp (4071 -> 2927) -> game_discovery.{cpp,h}.

## The proven pattern (follow this)

For a god-object F.cpp (shared file-scope state + many functions):

1. STATE ENABLER -- create F_internal.h:
   - Move shared file-scope `static` vars out as `extern` decls; definitions stay in
     F.cpp but with `static` removed (external linkage). constexpr consts -> `inline
     constexpr` in the header (remove from F.cpp). Shared structs/enums -> the header.
   - spectator_node had ONE g_state struct (easy). netplay had ~80 flat statics; I
     bulk-externed them with a throwaway python script that rewrote the `static` var
     lines + emitted the externs. If you script it: it must SKIP `static` lines whose
     COMMENT contains `(` (those got missed and became build-driven "stragglers"),
     and handle lambda-init statics / `static constexpr` / arrays / atomics.
   - Add `#include "F_internal.h"` near the top of F.cpp.

2. COLLISION SAFETY -- wrap internal (non-public-API) helpers in a named namespace
   (e.g. `namespace specnode {}`) when names are generic (Fletcher32, AddrEqual,
   FormatAddr collide with other TUs -- savestate.cpp has its own Fletcher32). Each
   sibling TU does `using namespace specnode;` so call sites stay unqualified. The
   PUBLIC api (functions declared in F.h, e.g. SpectatorNode_* / Netplay_*) stays
   global -- no namespace, no re-declaration needed (callers already include F.h).

3. PER-CLUSTER MOVE:
   - Slice a cohesive group VERBATIM into F_<concern>.cpp. Header of the new file =
     copy F.cpp's real #include lines ONLY (e.g. `sed -n '1,32p'` up to the last
     #include -- do NOT grab a fixed range that includes code after the includes).
   - Add `#include "F_internal.h"` + `using namespace <ns>;` if it calls internal helpers.
   - Register in CMake. Hook files: FM2KHook/CMakeLists.txt FM2KHOOK_SOURCES (builds
     BOTH FM2KHook + FM95Hook). Launcher files: root CMakeLists.txt launcher target.

4. BUILD-DRIVEN DECLS: build. For each "X not declared in this scope":
   - X is a helper called cross-TU -> un-static it in its home TU + declare in F_internal.h.
   - X is a forward-decl that conflicts -> remove the old `static` forward-decl.
   - X is a straggler static -> extern it.
   Iterate to green. Then build BOTH hooks + launcher. Commit.

## Gotchas that bit me (save yourself the iterations)

- SLICE BOUNDARIES: a function ends at its closing `}`, NOT at the next `// ===`
  banner. Over-slicing by even 3 lines (grabbing the next section's banner, or cutting
  a function's tail) -> "expected }" / "function-definition not allowed here". Verify
  the exact start and end with a targeted read before sed-slicing. Delete bottom-up
  (`sed -i '<hiRange>d'` then '<loRange>d') so earlier line numbers stay valid.
- ANON-NAMESPACE functions have internal linkage -- can't be called from a sibling
  TU. Un-anon (named namespace or global) + declare in the header.
- INLINE-RAII coupling: netplay's PerfScope (a timer RAII) is instantiated inline in
  the hot loop AND used by staying code, so PerfBucket/PerfScope/PerfNowNs had to go
  in the header (inline) with the perf data externed. Watch for the same in hooks.cpp.
- <ctime>: the copied include block can miss <ctime>; add it if the TU uses std::time.

## Remaining targets (recommended order)

### A. Finish FM2K_RollbackClient.cpp (2927, launcher/SDL, NOT determinism-critical -- SAFEST, do first)
Extract:
  - process_manager.{cpp,h}: LaunchGame/TerminateGame/LaunchLocalClient/
    LaunchLocalSpectator/LaunchRemoteSpectator/LaunchReplayPlayer/LaunchLocalSpectator2/
    TerminateAllClients + ReadRollbackStatsFromSharedMemory + ApplyPendingConfigToInstance.
    Owns the game-instance unique_ptrs (game_instance_, client1/2_instance_, spectator*_).
  - session_control: StartOfflineSession/StartOnlineSession/StartStressSession/StopSession.
Mistakes are LOUD here (launcher won't start), not silent desyncs. Verify: build + launch
the launcher (`FM2K_DEV_MODE=1` to see the console). No netplay test needed.

### B. hooks.cpp (3850, the injected hook core -- DETERMINISM-CRITICAL)
Re-read its current structure first (line numbers in any old notes are stale). Likely
cleanest leaves:
  - determinism cluster: SetMXCSR / PinFPUControlWord / Hook_timeGetTime (virtual clock).
  - VFS file-I/O cluster: Hook_CreateFileA/W, Hook_ReadFile, Hook_SetFilePointer(Ex),
    Hook_CloseHandle, FpkTryRedirectOpenA, MaybeRegisterPlayerVFile(A/W), ReadWholeFileFresh
    + the VFS handle registry statics. This is the most self-contained chunk.
  - input hooks, game-mode detection (CheckGameModeTransition), the per-frame driver,
    and Hook_InstallHooks (the 40+ MinHook installs).
Same pattern (hooks_internal.h for shared statics). BUILD-GREEN + NETPLAY SMOKE-TEST each.

### C. FM2K_LauncherUI.cpp (8025, BIGGEST -- but DO NOT TOUCH on bleeding without asking)
The user has an UNFINISHED UI rewrite on another branch. Splitting this on bleeding will
massively conflict with that rewrite. Either do it ON the rewrite branch, or let the
rewrite absorb the split. CONFIRM with the user before starting.

## The one function-refactor note (separate, behavior-sensitive)
netplay_battle_phase.cpp stays ~1188 because Netplay_ProcessBattleInputPhase is a single
984-line function. To get it under 1000 you must break the FUNCTION into sub-steps
(save/advance/replay/render phases) -- that touches rollback control flow, so it is NOT a
pure move: do it only when ready to netplay-test it. Out of scope for the pure-move sweep.

## Build commands
- Configure once: `./make_build.sh`  (also self-heals submodules)
- Build: `cd build && ninja`  (or `./build.sh` -> stripped bins in dist/)
- Must link: FM2KHook.dll, FM95Hook.dll, FM2K_RollbackLauncher.exe.
- Push to bleeding only when the user asks.
