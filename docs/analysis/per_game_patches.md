# FM2K Per-Game Patches — Reference

End-to-end documentation for the per-game patch system that lets each FM2K
game opt into hook-side fixes and gameplay tweaks independently. Targets
the WonderfulWorld v0.946 binary; addresses should hold for other vanilla
FM2K builds (they share the engine source).

Companion to `docs/analysis/gamespeed_csm_decompile.md` (the Vanpri /
GameSpeed write-up).

## Architecture

```
┌────────────────────────────────────────────────────────────────┐
│ launcher: FM2K_LauncherUI.cpp                                  │
│   Host Config tab → "Per-game experimental patches" panel      │
│   reads/writes %APPDATA%\FM2K_Rollback\game_patches\<id>.ini   │
│   on launch, ApplyGamePatchEnvVars() sets FM2K_* env vars      │
└──────────────────────────┬─────────────────────────────────────┘
                           │
                           ▼
┌────────────────────────────────────────────────────────────────┐
│ hook DLL: FM2KHook                                             │
│   DllMain → ApplyPerGameRuntimePatches() reads env vars,       │
│   forwards to module setters (CssAutoConfirm_SetTeamDupeLock,  │
│   RoundEvents_SetKofRetention, PerGamePatches_ApplyRuntime,    │
│   PerGamePatches_InstallKofHpInitPatch, etc.)                  │
│   InitializeHooks → installs MinHook detours + SafetyHook      │
│   MidHooks gated on env vars                                   │
└────────────────────────────────────────────────────────────────┘
```

**Per-game INI** is the source of truth. Format:
```ini
# %APPDATA%\FM2K_Rollback\game_patches\WonderfulWorld_ver_0946.ini
gs_pic_fix=1
team_kof_retention=1
team_size=3
damage_multiplier_pct=100
team_css_dupe_lock=1
vs_cpu_mode=0
cpu_vs_cpu_mode=0
training_mode=0
option_mode_selector=1
```

Edit by hand or via the UI. Missing keys = hardcoded default (mostly off).

## Patches

| INI key                    | Type | Default | Hook site                                | Implementation                                                 |
|----------------------------|------|---------|------------------------------------------|----------------------------------------------------------------|
| `gs_pic_fix`               | bool | true    | byte patch @ `0x40649A`                  | `FixGameSpeedDesync` in `dllmain.cpp`                          |
| `team_css_dupe_lock`       | bool | false   | extends `Hook_GameStateManager`          | `ApplyTeamDupeLock` in `css_autoconfirm.cpp`                   |
| `team_kof_retention`       | bool | false   | extends `Hook_vs_round_function` + MidHook @ `0x411CB1` | snapshot in `round_events.cpp`, apply in `per_game_patches.cpp` |
| `team_size`                | int  | 0 (off) | extends `Hook_GameStateManager`          | per-frame write to `g_team_round` / `g_team_round_setting`     |
| `damage_multiplier_pct`    | int  | 100     | MinHook @ `health_damage_manager (0x40E7C0)` | caller-gated to `hit_detection_system` range                 |
| `vs_cpu_mode`              | bool | false   | extends `Hook_GetPlayerInput`            | CSS pipes P1→P2; battle zeros P2                               |
| `cpu_vs_cpu_mode`          | bool | false   | extends `Hook_GetPlayerInput`            | CSS pipes P1→P2; battle zeros both                             |
| `training_mode`            | bool | false   | extends `Hook_GetPlayerInput` + overlay  | P2 behavior cycle via FN2 hotkey                               |
| `option_mode_selector`     | bool | false   | extends `Hook_GetPlayerInput` (title)    | OPTION button cycles submode → applies flag on title→CSS edge  |

### gs_pic_fix — Vanpri / script-wait scaling
- **What**: pins `g_gamespeed_pic_step` (0x445704) to 100 so `OP_PIC` waits stay frame-exact regardless of the GameSpeed INI value. MOVE/ACCEL scalars still scale with gs.
- **Why**: see `docs/analysis/gamespeed_csm_decompile.md`. Engine adds `pic_step * keepTime` per wait but dispatch loop decrements by hardcoded 100; only equal at gs=10.
- **Patch**: 6 NOP bytes overwriting `mov ds:[g_gamespeed_pic_step], ecx` at `0x40649A`. Initial value 100 written once at hook init.

### team_css_dupe_lock — block duplicate character per team slot
- **What**: in team-mode CSS, suppresses confirm bits when the cursor would re-select a character already locked into one of the player's prior team slots.
- **Where**: `ApplyTeamDupeLock()` in `css_autoconfirm.cpp`, called from `Hook_GameStateManager`. Reads `g_player_move_history` (P1 @ 0x47006C) and `g_p2_round_history_chars` (P2 @ 0x47007C); masks `g_input_changes[player] & ~0x3F0` (confirm bits) when a match is found.

### team_kof_retention — winner carries HP & meter across rounds
- **What**: at round-end edge (substate 521 → 900), snapshots winner's slot HP+meter. At the engine's HP-init write (`mov [ebx+0xDF05], eax` @ `0x411CB1`), substitutes the snapshotted value via SafetyHook MidHook.
- **Why**: vanilla resets HP to max for both sides at round init. KOF style retains winner's surviving HP.
- **Implementation**:
  - Snapshot: `Hook_vs_round_function` in `round_events.cpp` (any → 900 edge, team mode only, winner_idx < 2)
  - Apply: `OnKofHpInit` mid-hook in `per_game_patches.cpp` modifies `ctx.eax` so the engine's own instruction writes our value; side-writes meter at offset `+0xDF5D`
- **Earlier substate-edge implementation** (write at 100→110) had a visible HP-bar jitter; MidHook lands exactly at the engine's intended write so the engine never sees a wrong value.

### team_size — override team round count
- **What**: writes user's `FM2K_TEAM_SIZE` (range 2..8) to both `g_team_round` (0x430128, INI source) and `g_team_round_setting` (0x470064, runtime copy).
- **Why**: `g_team_round` direct write at hook init was clobbered later by `hit_judge_set_function` reloading the INI default. Per-frame override in `Hook_GameStateManager` is robust.

### damage_multiplier_pct — caller-gated hit damage scaler
- **What**: MinHook detour on `health_damage_manager @ 0x40E7C0` scales the `damage` argument by `mult / 100`. Range [1, 1000].
- **Caller gate**: only fires when invoked from `hit_detection_system` (0x40F010-0x40F90D). Other callers (recursive self-calls + `OP_CHECK_MOTION` script-side variants) pass through unchanged.
- **Why the gate**: v0.2.40 scaled ALL calls and broke Vanpri's stage-trigger scripts (which depend on tightly-balanced script-encoded values to self-damage in lockstep with opponent damage). Caller gate keeps the multiplier focused on the actual hit-damage path.

### vs_cpu_mode / cpu_vs_cpu_mode / training_mode
- **CSS takeover**: when any of these three is active, `Hook_GetPlayerInput` pipes P1's raw input to P2 during CSS (game_mode==2000) so P1 picks both fighters.
- **Battle behavior**:
  - `vs_cpu_mode`: P2 input returns 0 — character's script falls through OP_RANDOM/OP_LIFE_GAUGE branches (FM2K's natural AI path).
  - `cpu_vs_cpu_mode`: both P1 and P2 inputs return 0.
  - `training_mode`: P2 input overridden based on `g_training_p2_behavior` (0=player, 1=CPU, 2=imitate P1, 3=guard, 4=jump-up). FN2 button cycles. ImGui badge top-right shows current state.
- **Mutual exclusion**: enforced in launcher UI (ImGui::BeginDisabled around conflicting checkboxes). The hook resolves all-three-on in a fixed precedence anyway, but UI exclusion prevents the confusing combination.
- **Key insight**: FM2K's "AI" is script-driven via OP_RANDOM / OP_LIFE_GAUGE / OP_GAUGE_BRANCH branches. `ai_behavior_processor` at 0x410060 is a motion-input matcher (not an AI generator). Zero input → OP_CHECK_MOTION never matches → script falls through to RNG branches.

### option_mode_selector — title-screen submode cycle
- **What**: when active, pressing OPTION (input bit 0x800 = bit 11) on title cycles `g_vs_submode` (0=VS 2P, 1=VS CPU, 2=CPU vs CPU, 3=Training). On title→CSS transition (game_mode 1000→2000), the selected submode applies its corresponding mode flag.
- **OPTION button**: dedicated bit, separate from START (0x400). Default bound to **Tab** on keyboard. Rebindable via the launcher's Input Bindings UI.

## Input layout (14 bits total)

The hook's binder produces a 14-bit input mask; the engine only sees the
lower 11 bits (`kEngineInputMask = 0x07FF`). Meta-bits are stripped before
the value is passed to the engine.

| Bit  | Value  | Name   | Engine-visible? | Default key (P1) |
|------|--------|--------|-----------------|------------------|
| 0    | 0x0001 | LEFT   | yes             | Left arrow       |
| 1    | 0x0002 | RIGHT  | yes             | Right arrow      |
| 2    | 0x0004 | UP     | yes             | Up arrow         |
| 3    | 0x0008 | DOWN   | yes             | Down arrow       |
| 4    | 0x0010 | A      | yes             | Z                |
| 5    | 0x0020 | B      | yes             | X                |
| 6    | 0x0040 | C      | yes             | C                |
| 7    | 0x0080 | D      | yes             | A                |
| 8    | 0x0100 | E      | yes             | S                |
| 9    | 0x0200 | F      | yes             | D                |
| 10   | 0x0400 | START  | yes             | Enter            |
| 11   | 0x0800 | OPTION | NO (meta)       | Tab              |
| 12   | 0x1000 | FN1    | NO (meta)       | F1               |
| 13   | 0x2000 | FN2    | NO (meta)       | F2               |

Meta-bits consumed inside FM2KHook:
- **OPTION** drives the title-screen submode cycle when `option_mode_selector` is on.
- **FN2** is the training-mode P2 behavior cycle hotkey (replacing the legacy GetAsyncKeyState polling — TODO: migrate the consumer in imgui_overlay.cpp to read FN2 from the binder).
- **FN1** is reserved.

## Hook-point register preservation

Two patches do mid-function instruction interception. Each has different
register-preservation requirements; getting them wrong corrupts unrelated
engine state that uses the surviving register values.

### gs_pic_fix (byte patch, no detour)
- Patches a `mov ds:[mem32], reg32` instruction with 6 NOPs.
- No registers touched — pure write suppression.

### team_kof_retention HP-init (SafetyHook MidHook @ 0x411CB1)
- **Original instruction**: `mov [ebx+0xDF05], eax` (6 bytes).
- **Registers SafetyHook auto-preserves**: all integer registers + flags + XMM. Our handler can read/write any without manual save/restore.
- **What our handler does**:
  - Reads `ctx.ebx` (char_data ptr) and `ctx.eax` (max_hp from props)
  - For winner's slot in team mode with pending snapshot: sets `ctx.eax = snapshot.hp` and direct-writes `*(ctx.ebx + 0xDF5D) = snapshot.meter`
  - SafetyHook then runs the original instruction (in its trampoline) with our modified ctx.eax
- **Historical note**: v0.2.42 used a manual trampoline that failed to preserve ECX, which was read at 0x411CB7 by `mov [esi+60h], ecx` (writes `obj->player_slot_id_mirror`). Garbage ECX → wrong slot ID → hit detection couldn't match attacker/defender → hits passed through. SafetyHook makes this class of bug impossible.

## SafetyHook + Zydis integration

`vendored/safetyhook/amalgamated-dist/safetyhook.{cpp,hpp}` and
`vendored/zydis-amalgamated/amalgamated-dist/Zydis.{c,h}` are compiled
directly as FM2KHook sources — no `FetchContent`, no submodule, no
toolchain-file complexity. SafetyHook's `__has_include("Zydis.h")` finds
the flat Zydis header on the include path.

The polyfill amalgamation (`amalgamate.py --polyfill`) is used because
MinGW i686-w64-mingw32 g++ 13 ships with `-std=c++20` in this codebase
and `std::expected` is C++23-only. `vendored/safetyhook/amalgamated-dist/tl/expected.hpp`
is vendored from TartanLlama/expected to satisfy the polyfill.

Re-generating after a SafetyHook upgrade:
```bash
cd vendored/safetyhook && python3 amalgamate.py --polyfill
```

Re-downloading the tl::expected polyfill:
```bash
curl -sL https://raw.githubusercontent.com/TartanLlama/expected/master/include/tl/expected.hpp \
  -o vendored/safetyhook/amalgamated-dist/tl/expected.hpp
```

DLL size grows ~1.3 MB from SafetyHook+Zydis. Acceptable.

## Known caveats

1. **`option_mode_selector` requires the binder to be active.** Default keybindings load when the binder INI is present; if the user has no `fm2k_inputs.ini`, the hook falls back to `original_get_player_input` which only produces 11 bits. OPTION won't fire from that path. The hook still calls `PerGamePatches_OnTitleInputTick` from both paths so the rising-edge tracker stays consistent.
2. **Training mode "Guard" direction is hardcoded** to bit 0x002 (RIGHT), assumes P2 starts on the right facing left. Stages where P2 starts on the left flip this. Refinement: read P2's facing-bit from char state.
3. **AI behavior depends on character scripts.** Characters whose action_table lacks OP_RANDOM / OP_LIFE_GAUGE / OP_GAUGE_BRANCH branches will idle when their input is zeroed. Per-character property, not a hook bug.
4. **Mode mutual exclusion** is enforced in the launcher UI but NOT in the hook. If all three INI keys are set to 1 (hand-edited), the hook resolves in fixed precedence: cpu_vs_cpu_mode > vs_cpu_mode > training_mode for battle behavior; CSS takeover fires if ANY of the three is active.
5. **Netplay safety**: the input override only fires on the offline path (netplay/spectator branches return earlier in `Hook_GetPlayerInput`). Do NOT enable mode toggles in a hub match — behavior undefined.
6. **F2 hotkey for training behavior cycle** currently uses `GetAsyncKeyState(VK_F2)` in `imgui_overlay.cpp`. The binder now exposes FN2 (bit 0x2000) on the same default key; future cleanup migrates the cycle to read from the binder so users can rebind F2 to any key/button.

## Deferred work

- **Phase 1.1 (1P arcade CPU meter reset)** — the original 7-item list's only outstanding bug. Exploration agent's initial guess (0x4089DA, 0x408AB7) was wrong — those addresses only fire in team / VS modes. Actual asymmetric reset in 1P arcade needs a runtime write-watch on `g_charslot1_super_meter @ 0x4EDD1C` during a round transition to identify the real write site.

## File index

| Concern                                  | Path                                                                  |
|------------------------------------------|----------------------------------------------------------------------|
| Hook DLL entry / patch install           | `FM2KHook/src/core/dllmain.cpp`                                       |
| MinHook installs                         | `FM2KHook/src/hooks/hooks.cpp`                                        |
| OPTION title hook + input override       | `FM2KHook/src/hooks/per_game_patches.cpp`                             |
| KOF retention (snapshot + MidHook)       | `round_events.cpp` (snapshot) + `per_game_patches.cpp` (MidHook)      |
| Team CSS dupe lock                       | `FM2KHook/src/hooks/css_autoconfirm.cpp`                              |
| Damage multiplier MinHook                | `FM2KHook/src/hooks/per_game_patches.cpp`                             |
| Team size override                       | `css_autoconfirm.cpp` (per-frame write) + `per_game_patches.cpp` (env intake) |
| Training overlay + F2 polling            | `FM2KHook/src/ui/imgui_overlay.cpp`                                   |
| Per-game INI persistence                 | `FM2K_LauncherUI.cpp` (LoadGamePatch*/SaveGamePatch*)                 |
| Host Config UI                           | `FM2K_LauncherUI.cpp` (RenderHostConfigBody)                          |
| Input binder + bit layout                | `FM2KHook/src/ui/input_binder.{h,cpp}`                                |
