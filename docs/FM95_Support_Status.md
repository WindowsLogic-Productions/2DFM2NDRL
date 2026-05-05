# FM95 Support — Status & Gap Audit

**Last verified:** 2026-05-05 — CPW.exe boots cleanly under FM95Hook.dll on US-locale Windows. Title screen renders Japanese, character select works, demos play.

This doc enumerates what works, what's wired but dormant, what would crash if activated, and the path to full FM95 rollback support. Companion to `FM95_Integration.h` (address tables) and `gekkonet_bsnes_reference.md` (rollback architecture reference).

---

## Status matrix

| Layer | Status | Notes |
|-------|--------|-------|
| Game discovery / engine detection | ✅ working | xxhash registry + KGT2KGAME/KGT95GAME string sniff + filesize fallback. CPW shows as `[95] CPW.exe`. |
| DLL split (FM2KHook / FM95Hook) | ✅ working | Single source, `-DENGINE_FM95=1` builds the FM95 variant. Launcher picks DLL via `FM2KGameInstance::engine_`. |
| Locale spoof (LE-style) | ✅ working | 19/19 hooks installed: codepage, LCID, DBCS, validity, path APIs, SetWindowTextA. CPW's title bar renders proper Shift-JIS. |
| Path encoding (no LE needed) | ✅ working | `WC_NO_BEST_FIT_CHARS` on `GetCommandLineA` / `GetModuleFileNameA` etc. preserves fullwidth Ｃ Ｐ Ｗ → SJIS bytes survive → CPW finds `ＣＰＷ.kgt`. |
| Per-frame function hooks | ✅ installed, ⚠ dormant | `update_game_state`, `render_game`, `process_game_inputs`, `game_rand`, `dispatch_script_sound` all hook on FM95 addresses and pass through to originals. FM2K-specific code paths inside the hooks (T4 probe, facing-fix in battle, etc.) are no-ops on FM95 because their `game_mode >= 3000` guards never fire. |
| `RUN_GAME_LOOP` hook (trampoline mount) | ❌ skipped on FM95 | FM95's `RUN_GAME_LOOP` *is* `_WinMain@16`. Detouring it intercepts the process entry point before init runs and CPW silently dies. Hook is `#if FM2K::kIsFM2K`-gated. |
| Phase classifier | ❌ broken on FM95 | `game_mode == 2000` (CSS) and `game_mode >= 3000` (Battle) never fire. FM95 keeps `g_game_mode` near 0/1/10. **Root blocker for everything below.** |
| CSS-sync handshake | ❌ dormant | depends on phase classifier; never enters CSS branch. |
| Battle entry / GekkoNet start | ❌ dormant | depends on phase classifier; never detects battle. |
| Save state capture | ❌ would crash if activated | `savestate.cpp` hardcodes FM2K addresses (`0x4701E0`, stride 382, `CHAR_SLOT_SIZE=57407`). FM95 needs different base/stride/size. |
| Rollback resimulation | ❌ no driver | Trampoline isn't running, no save/load/advance event dispatch on FM95. |
| Sound rollback mute | ✅ verified | `Hook_DispatchScriptSoundCommand` fires; `SoundRollback: mute bgm=1 se=0` appears in CPW boot log. Will work once rollback drives it. |

---

## Layer-by-layer audit

### 1. Address mapping (`FM2KHook/src/core/globals.h`)

The `#if defined(ENGINE_FM95)` block has 20+ `FM2K::ADDR_*` constants pointing at verified FM95/CPW addresses. Cross-checked against `FM95_Integration.h` and the live CPW IDB.

**Sentinels (set to 0):** `ADDR_P1_HP`, `ADDR_P2_HP`, `ADDR_ROUND_TIMER_COUNTER`, `ADDR_CSS_ACTIVE_PLAYER`, `ADDR_PLAYER_STAGE_POSITIONS`. FM95 has no global equivalents — these live in the per-object pool slot or aren't applicable.

**Risk:** `imgui_overlay.cpp:112-113` dereferences `(uint32_t*)FM2K::ADDR_P1_HP` and `ADDR_P2_HP` in the Debug → Battle tab. On FM95 = null-deref crash if the user opens that tab. **Fix:** guard with `if constexpr (FM2K::kIsFM95)` and either skip or read HP from the object-pool slot via `g_p_main_object_ptr[player_idx]` + offset 72.

### 2. Phase classifier (`THE root blocker for FM95 rollback`)

Both `IsCSSMode(uint32_t mode)` and `IsBattleMode(uint32_t mode)` in `hooks.cpp:171-176` are pure FM2K magic-number checks. On FM95 they always return false.

Sites consuming these (~30 across hooks.cpp / input.cpp / main_loop_trampoline.cpp):
- `hooks.cpp` 441-463, 519-520, 645, 669, 714, 785, 849, 885, 926, 968, 1044
- `input.cpp` 152, 181-182
- `main_loop_trampoline.cpp` 318-319, 548

**Fix shape:** make `IsCSSMode` / `IsBattleMode` engine-aware:
```cpp
static bool IsCSSMode(uint32_t mode) {
    if constexpr (FM2K::kIsFM2K) return mode == 2000;
    else return FM95::CharSelect::ClassifyPhase(
                  (const uint8_t*)FM2K::ADDR_OBJECT_POOL) == FM95::CharSelect::Phase::CSS;
}
```
`FM95::CharSelect::ClassifyPhase` is already implemented in `FM95_Integration.h` (walks the 256-slot object pool for `type==19`/`type==16` plus sub_state range check). One-time scan per call — could cache by frame counter if hot.

### 3. Save state (`savestate.cpp` / `savestate.h`)

Bypasses `FM2K::ADDR_*` ifdef entirely. Hardcoded FM2K constants:
- `savestate.cpp:30` `ADDR_OBJECT_POOL = 0x4701E0` (FM95: 0x426A40)
- `savestate.cpp:31` `SIZE_OBJECT_POOL = 0x5F800` (FM95: 0xA400)
- `savestate.cpp:17` `ADDR_RENDER_FRAME_COUNTER = 0x4456FC` (FM95: 0x4DD7A8)
- `savestate.cpp:912-913` P1/P2 obj pool base = `0x4701E0`
- Object stride literal `382` scattered throughout (FM95: 0xA4 = 164)
- `savestate.h:17,21` `CHAR_SLOT_SIZE = 57407`, `CHAR_SLOT_BASE = 0x4D1D90` (FM95: 229844, 0x509100)

**Fix shape:** delete the local `constexpr`s, use the `FM2K::ADDR_*` from `globals.h` so the engine ifdef applies. Add new constants `FM2K::OBJECT_POOL_STRIDE` (FM2K=382, FM95=164) and replace literal 382 sites. Char-slot count is 8 on FM2K vs 5 on FM95 — bound the slot iteration on a `FM2K::CHAR_SLOT_COUNT` constant.

### 4. CSS-sync handshake (`control_channel.cpp`)

Doesn't appear to hardcode CSS state addresses (grep returned zero hits). Likely consumes through `FM2K::CharSelect::Memory::*` aliases from `FM2K_Integration.h`. To support FM95 CSS sync, mirror the layout: add a parallel `FM95::CharSelect::Memory` block (per-player struct at `0x432720` stride 16: `+0` = char_cursor, `+8` = color_variant, `+0xC` = confirmed) and switch readers via `if constexpr (FM2K::kIsFM95)` or a single struct accessor that knows the layout per engine.

### 5. Trampoline / GekkoNet outer loop

`main_loop_trampoline.cpp` is what drives GekkoNet's Save / Load / Advance events. On FM2K it's mounted by detouring `RUN_GAME_LOOP` and replacing the message pump + timing + render dispatch. On FM95 we skip the hook (would intercept WinMain entry pre-init), so the trampoline never fires.

**Two paths to FM95 rollback:**

- **(A) Refactor trampoline as a per-frame coroutine.** Move its event-pump and Save/Load/Advance dispatch logic into helpers callable from inside `Hook_UpdateGameState` (start of frame) and `Hook_RenderGame` (end of frame). Lets it ride along inside FM95's natural WinMain loop. Bigger rewrite, but unifies the architecture.
- **(B) Delay-only netcode for FM95 v1.** Hook_ProcessGameInputs already injects inputs from a remote source via shared state. Skip rollback resimulation entirely on FM95; both clients run lockstep with input delay. GekkoNet still useful for transport + sync. Phase rollback in once FM95 + online with delay is shipped and tested.

**Recommendation: B for v1.** Lower risk, gets FM95 online faster, lets us validate the locale + CSS + battle entry layers in production before doing the trampoline refactor.

### 6. Input ring layout

FM95 input ring is **256 entries × 4 bytes** with 8-bit (mod 256) `buf_idx` wraparound. FM2K is **1024 entries × 4 bytes** with 10-bit (mod 1024). If `input.cpp` or anywhere assumes the FM2K size for indexing or memcmp, FM95 will desync or read garbage.

Audit candidate sites: search `input.cpp` and `savestate.cpp` for `1024`, `0x400`, `& 0x3FF`, `* 4096` (= 1024 × 4). Should be replaced with `FM2K::INPUT_HISTORY_LEN` (define in globals.h: 1024 for FM2K, 256 for FM95).

### 7. Engine ID for the launcher

Launcher discovery is done (xxhash registry + string sniff + filesize). `FM2KGameInstance::Launch(exe, engine)` injects the right DLL. Cache schema (`exe|dll|size|mtime` in `games.cache`) doesn't yet persist `engine` — every restart re-runs hashing on cache miss. Acceptable for now; add a 5th column when convenient.

---

## Path to full FM95 rollback parity (decided: Path A)

We're going for full FM2K parity — FM95 gets the same rollback netcode, not a delay-only stripped variant. The trampoline gets refactored from "owns the outer loop" to "callable from inside any frame driver" so it works both as the FM2K detour and as guest-driven hooks on FM95.

1. **Fix imgui_overlay HP deref** (Task 12). Trivial guard.
2. **Add per-engine size constants** to globals.h ifdef: `OBJECT_POOL_STRIDE`, `CHAR_SLOT_COUNT`, `INPUT_HISTORY_LEN`. Foundation for steps 3-5. (Task 13.)
3. **Engine-aware `IsCSSMode` / `IsBattleMode`** via `FM95::CharSelect::ClassifyPhase` (object-pool walk for type==19/16 with sub_state range). Unblocks every consumer site. (Task 3.)
4. **Refactor `savestate.cpp` / `.h`** to consume globals.h constants instead of hardcoded `0x4701E0` / `382` / `0x4D1D90` / `57407`. Loop bounds use `OBJECT_POOL_STRIDE` + `CHAR_SLOT_COUNT`. (Task 4.)
5. **`FM95::CharSelect::Memory` mirror namespace** wired through `control_channel.cpp` so CSS sync uses FM95's per-player struct at `0x432720` stride 16. (Task 5.)
6. **Engine-aware input ring sizing** (`INPUT_HISTORY_LEN`). Replace `1024` literals. (Task 8.)
7. **Smoke-test offline FM95** end-to-end with the above. Confirms plumbing without exercising rollback yet.
8. **Trampoline coroutine refactor** (Task 7) — DONE (split). `main_loop_trampoline.cpp`'s inner loop body extracted into `TrampolineFrameTick()` returning `LoopPhase`. `TrampolineMainLoop()` is now a thin FM2K-only wrapper. `IsCSSMode/IsBattleMode` are exported via `hooks.h`; `ClassifyPhase` consumes them so the trampoline picks up FM95 phase via the object-pool walk.
9. **FM95 host-driven trampoline activation** — REMAINING. Wire Hook_UpdateGameState (FM95) to call `TrampolineFrameTick()` and set `g_fm95_skip_next_render = true` for non-NATIVE phases. Hook_RenderGame consumes the flag to suppress the host's natural render call when the trampoline's `RenderFrameWithSnapshot` already drove it. Gate behind `FM95_TRAMPOLINE=1` env var so we can A/B against the current (working) FM95 boot without risk. Skeleton:
   ```cpp
   // Hook_UpdateGameState (top of body, before existing logic)
   if constexpr (FM2K::kIsFM95) {
       static const bool s_use_tramp = std::getenv("FM95_TRAMPOLINE") != nullptr;
       if (s_use_tramp) {
           LoopPhase phase = TrampolineFrameTick();
           if (phase == LoopPhase::NATIVE) {
               return original_update_game ? original_update_game() : 0;
           }
           g_fm95_skip_next_render = true;
           return 0;
       }
   }
   ```
10. **Online FM95 rollback parity test** vs WW baseline.

Tasks 1, 2, 12 are independent. 3 depends on 13. 4 depends on 13. 8 depends on 13. 5 depends on nothing structural. 6 depends on 3+4+5+8 + smoke test. 7 (trampoline refactor) is the final step. Tasks 1, 2, 10, 11 are complete; the rest are pending in the dependency graph.

---

## What we built to get here

The locale spoof effort is documented separately in conversation notes — short version: trace-driven (5 IDA queries to learn CPW's actual locale-API surface), not list-driven (no LE port). Final hook surface is 19 APIs across kernel32/user32. The non-obvious win was `WC_NO_BEST_FIT_CHARS` on every wide→ANSI re-encode — that's what kept fullwidth Ｃ Ｐ Ｗ alive through the path-derivation chain instead of collapsing to ASCII.

The DLL split (`FM2KHook` / `FM95Hook` from one source via `-DENGINE_FM95=1`) was chosen over a runtime `GameProfile` to minimize churn on the existing FM2K hooks. Once FM95 is fully validated end-to-end, the natural follow-up is a single-DLL with runtime profile selection, but that's a refactor, not a feature gap.
