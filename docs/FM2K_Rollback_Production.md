# FM2K Rollback — Master Specification

Single source of truth for the rollback netcode implementation on Fighter Maker 2nd (FM2K). All other `FM2K_Rollback_*.md` / `rollback_*.md` / `Launcher_Roadmap.md` docs in this directory are historical and may be out of date.

**Status (2026-04):** Rollback core works. Determinism pass complete. Trampoline architecture in place. Active testing / feature work.

---

## 1. Architecture

### 1.1 Process model (LilithPort-style)

```
 FM2K_RollbackLauncher.exe                FM2K game process
 ┌───────────────────────┐    inject    ┌───────────────────────────────────┐
 │  ImGui / SDL3 UI      │ ───────────► │  FM2KHook.dll                     │
 │  Game selection       │              │  • MinHook detours on game funcs  │
 │  Session config       │              │  • Trampoline main loop           │
 │  Stats / logs viewer  │ ◄──────────► │  • GekkoNet rollback session      │
 │  Shared-memory IPC    │   shm+mmap   │  • Control-channel (0xCC UDP)     │
 └───────────────────────┘              └───────────────────────────────────┘
```

Launcher creates FM2K suspended, injects `FM2KHook.dll`, resumes. The hook owns the main loop from the moment the game enters battle.

### 1.2 Phase-dispatched trampoline

`main_game_loop` @ `0x405AD0` is replaced wholesale. See `FM2KHook/src/core/main_loop_trampoline.{h,cpp}`.

| Phase | When | Drive model |
|-------|------|-------------|
| `NATIVE` | Menus, intro, results | Reproduces original loop structure so gameplay code that depends on `main_game_loop` prologue writes still works |
| `CSS` | Character select | Control-channel lockstep, **no** GekkoNet session yet |
| `TRAMPOLINE_BATTLE` | Active match | GekkoNet drives sim + save + load + advance |

Why wholesale replacement vs byte-patching: forward-sim and rollback-replay were hitting subtly different code paths because the native loop had per-iteration writes (buf_idx fan-out, render-frame counter, etc.) that rollback advance wasn't reproducing. Trampoline owns the loop, so there's one code path.

### 1.3 Frame pacing

100 FPS = 10 ms/frame. QPC-based sleep with busy-wait on the last ~1 ms. `SDL_Delay` capped us at ~90 FPS due to Windows scheduler granularity.

---

## 2. Determinism

Forward-sim on peer A must produce byte-identical state to rollback-replay on peer B for a given input sequence. Everything below exists to make that true.

### 2.1 FPU / SSE pinning

At hook init: `_controlfp_s` pins x87 control word, `SetMXCSR(0x1F80u)` pins SSE rounding. See `FM2KHook/src/hooks/hooks.cpp::PinFPUControlWord`.

### 2.2 Virtual clock

`timeGetTime` is hooked. During an active session it returns `g_virtual_time_ms = frame * 10`. Rollback `LoadEvent` rewinds this value — otherwise replayed frames see different wall-clock times than forward sim did.

### 2.3 Render isolation

`original_render_game` mutates sim-visible memory (afterimage pool, screen shake). Render runs on display frames, not sim frames, so those mutations mustn't leak into rollback. Solution: snapshot afterimage pool before render, restore after — **with a carve-out for `SHAKE_EFFECTS` @ `0x447DA9` (40 bytes)**. Shake is scripted by the dev (opcode `[EB]`) and its countdown MUST tick through render so it ends on schedule. Without the carve-out it "never ends."

Carve-out (`RenderFrameWithSnapshot`):
```cpp
SHAKE_OFFSET_IN_AI = 0x447DA9 - 0x447930 = 0x479
// Save afterimage [0 .. 0x479), skip 40 B shake block, save [0x479+40 .. end]
// Restore same range after original_render_game returns
```

### 2.4 Excluded from save/CRC

- `g_last_frame_time` @ `0x447DD4` — touching it in rollback re-triggers main-loop's frame-skip check → hang.
- Render-only render state (only game-sim state is saved).

### 2.5 Inputs

Native FM2K input capture (not `GetAsyncKeyState`). `original_get_player_input(0, 0)` applies facing swap internally; `Input_CaptureLocal` **undoes** the swap using the same predicate as `Hook_GetPlayerInput` so the sim sees unswapped inputs (swap happens once, during sim, not twice).

Unified P1 controls: both local instances use P1 bindings regardless of GekkoNet slot.

---

## 3. Save state

See `FM2KHook/src/netplay/savestate.{h,cpp}`.

### 3.1 Coverage (Wave C audit)

| Region | Addr | Size | Why |
|--------|------|------|-----|
| `char_dynamic[8]` | `0x4D1D90` + `i*57407` | 8 × 57407 | Full char slot. **Fix: base was off by 16, dynamic offset was 55000 (missed 95%).** |
| `object_pool` | `0x4701E0` | ~391 KB | All 1023 game entities. |
| `afterimage_pool` | `0x447930` | ~163 KB | Dash trails, motion blur — unsaved before Wave C. |
| `object_list_heads_tails` | `0x430240` | 1024 B | List topology (next-ptrs). Payloads in object_pool but iteration order lived here. |
| `object_node_pool` | `0x4CFA20` | 8192 B | Same. |
| `current_object_ptr` | `0x4259A8` | 4 B | Value captured. |
| `input_history` | `0x4280E0` / `0x4290E0` | ~8 KB | Both players' 1024-frame ring. |
| `game_state` | — | 544 B | HPs, positions, timers, RNG. |
| `effect_sys1` | — | 42 B | Effect slot A. |
| `effect_sys2` | `0x4456B0..0x445708` | 88 B | Covers `g_global_variable_array_FM2K_SYSTEM_VARS` + effect-sys2 + timer_countdown1. **Expanded from 44 B.** |
| `shake_effects` | `0x447DA9` | 40 B | Saved AND carve-out'd from render snapshot (§2.3). |
| `round_end_flag` | `0x424718` | 4 B | Drives round transitions — unsaved before audit. |
| `sound_desired[]` | — | — | Sim's authoritative desired SFX state (rollback-safe sound layer). |
| `input_tracking_state` | — | 160 B | Tracking block. |

Total: ~420 KB per save slot.

### 3.2 Active-slot optimization

`object_pool` and `char_dynamic` save paths scan active flags and skip dormant slots. Dropped save time significantly during stress-mode.

### 3.3 Hash

xxHash3-64 via vendored `vendored/xxhash/xxhash.h` v0.8.2, folded to uint32 for on-wire compat. Replaces Fletcher32 — faster and lower collision rate.

### 3.4 Two-tier desync detection

| Tier | Freq | Cost | Purpose |
|------|------|------|---------|
| Gameplay fingerprint | Every save | ~44 B hash | Player-visible state only (HP, pos, RNG, round timer, inputs). Process-independent. |
| Full CRC | 1 / sec throttled | 420 KB hash | Catches any memory divergence. |

If full CRC diverges but fingerprint matches → false-positive (internal layout drift, no visible effect). If fingerprint diverges → real desync.

### 3.5 Per-region CRCs captured at save time

`SaveStateData::saved_region_crcs` stores per-region hashes of the forward-sim state. On desync, we compare against current (post-replay) region hashes — one log line reveals which region diverged without diffing cross-peer dumps.

Per-object-slot CRCs (1023 × 4 B) let us pin desyncs to exact object slot indices.

### 3.6 Replay-diff tooling

`SaveState_PushRngTrace` writes to a fixed-size ring buffer in memory; flushed to CSV on desync / on demand. Per-frame console logging tanked framerate.

---

## 4. Networking

### 4.1 Multiplexed UDP

Single UDP socket shared between GekkoNet packets and our control-channel packets. `0xCC` magic byte prefix marks control-channel; the adapter filters them out and queues for `ControlChannel_Poll`. GekkoNet packets pass through untouched. See `FM2KHook/src/netplay/control_channel.{h,cpp}`.

### 4.2 Peer address learning

Host binds to the configured local port with `remote_addr` empty. First authenticated HELLO packet received by `RawReceive` latches the peer's source address. Client fills in `remote_addr` from config (host's advertised address). No separate handshake server needed.

### 4.3 Control channel packet types

HELLO / HELLO_ACK, PING / PONG, CSS inputs + cursor + char-select + char-lock + start, BATTLE_READY / BATTLE_ACK / BATTLE_ENTERING / BATTLE_START / BATTLE_END, DISCONNECT.

PING/PONG: RTT sample, used for auto-delay suggestion.

### 4.4 Input delay

**Per-peer local delay via GekkoNet's native per-player delay API.** Each player sets their own preferred delay. A laggy-connection player can add delay to smooth their experience without forcing it on their opponent.

(Earlier design had a `BATTLE_READY proposed_local_delay` max() handshake to enforce symmetric delay — this was redundant and is being removed.)

### 4.5 CSS lockstep

CSS runs **before** GekkoNet session start. Sync via control channel: cursor positions, char/color highlight, char/color lock, start signal. Both peers must hit BATTLE_READY before session starts.

CSS residue divergence fix: initial state sync zeros `buf_idx`, `render_fc`, input state before forward-sim begins so both peers start from a clean frame 0.

### 4.6 GekkoNet config

```
num_players = 2
input_size = sizeof(uint16_t)   // 11-bit FM2K input mask
state_size = ~420 KB            // full save
input_prediction_window = 3     // 30 ms @ 100 FPS
desync_detection = true
```

---

## 5. Build

Prereqs: MinGW-w64 (i686-w64-mingw32), CMake 3.20+, Ninja.

```bash
./make_build.sh   # configure
./go.sh           # build + deploy to C:/games/
```

Outputs: `FM2K_RollbackLauncher.exe`, `FM2KHook.dll` (32-bit, must match FM2K arch).

---

## 6. Test plan

### 6.1 Determinism (regression tests)

- [x] Forward-sim + rollback-replay produce byte-identical state on same inputs (stress mode)
- [x] CSS → battle transition without residue divergence
- [x] Float determinism across peers (FPU + MXCSR pinning)
- [x] `timeGetTime` rewinds correctly on `LoadEvent`
- [x] Render-time mutations stay out of sim (afterimage snapshot)
- [x] Shake countdown ticks through render (carve-out)

### 6.2 Functional (needs verification with human opponent)

- [ ] Real P2P LAN test with per-player delay (both sides set own value)
- [ ] Rematch flow (round end → round start, multiple rounds)
- [ ] Match end → return to CSS → re-enter battle
- [ ] Mid-match disconnect → clean teardown (not just HELLO teardown)
- [ ] Shake behavior sweep across characters (different shake opcode durations)
- [ ] SFX rollback integration — no double-fire on rollback
- [ ] Sustained 100 FPS under worst-case rollback depth (8+ frames during effect-heavy sequences)
- [ ] Alt-tab / focus loss / pause
- [ ] Game variants beyond WonderfulWorld

### 6.3 GekkoNet feature enablement

- [ ] Runahead (speculative local sim to hide input delay)
- [ ] Migrate to GekkoNet native stress session API (replaces our custom stress mode)
- [ ] Limited Saving — evaluate only if save perf regresses

---

## 7. LilithPort feature matrix

What we have, what we don't, where we exceed.

| Feature | LilithPort | This | Notes |
|---------|-----------|------|-------|
| Core netcode | Delay-based | **Rollback (GekkoNet)** | Strict upgrade |
| UI | WinForms (.NET Framework) | ImGui + SDL3 | Modern, cross-platform-ready |
| Desync detection | Custom checksum | xxHash3 + gameplay fingerprint + per-region + per-object-slot | Stronger diagnostics |
| Input delay | Fixed symmetric | Per-player (GekkoNet native) | More flexible |
| Spectator | ✗ | Transport ✓, playback driver pending | Daisy-chain (CCCaster-style hub-with-redirect), 0xCE batched input stream over multiplexed UDP. Set-scoped (survives CSS between matches), unlike GekkoNet's match-scoped spectator session. See `docs/FM2K_Spectator_Design.md`. |
| Replays | ✓ (record/playback) | **Recording ✓**, playback wiring pending | `.fm2krep` files written to `replays/` per match. Format includes initial RNG + char selects + per-frame inputs. |
| Chat | ✓ | Transport ✓, UI pending | In-game overlay (Fightcade-style via DirectX hook), not launcher UI |
| Lobby / server browser | ✓ (hosted) | ✗ | Planned — NAT design TBD |
| Rematch flow UI | ✓ | ✗ (manual reconnect) | Planned |
| Match history / stats | Minimal | ✗ | Planned |
| NAT traversal | UPnP | **Planned: hole-punch + relay** | UPnP fails on ~50% of routers; hole-punch + relay is more reliable |
| Training / frame data overlay | Basic | ✗ | Future |

---

## 8. Roadmap

Ordered. Dependencies noted.

1. **Consolidate master doc** — this document. *(in progress)*
2. **Revert symmetric delay handshake** → use GekkoNet per-player delay. *Blocks #3.*
3. **Real P2P LAN test pass** — see §6.2.
4. **Main-branch merge decision** — main has 3 divergent commits (`9dc4054`, `532f3eb`, `e3f45e2`). Merge commit vs rebase vs leave.
5. **Enable GekkoNet runahead** — free latency win, expose in launcher UI.
6. **Migrate to GekkoNet stress session API** — replace custom stress mode.
7. **NAT traversal design** — hole-punch over control channel + relay fallback. Open: relay hosting, lobby model (hosted matchmaker vs peer-hosted).
8. **Local replays (record/playback)** — *blocks #9.* Record: char selects, start frame, input history, initial RNG, game hash. Playback: trampoline driven by recorded inputs.
9. **Spectator mode** — replay streamer that survives across CSS between matches in a set. GekkoNet's native spectator session is match-scoped; ours is set-scoped because we drive viewers through CSS ourselves via CSS_UPDATE packets. Sub-tasks: spectator playback driver (LoopPhase), CSS state mirror, set persistence (heartbeat). See `docs/FM2K_Spectator_Design.md` §8.5.
10. **Chat (in-game DirectX overlay) + match history.** Transport already in tree; UI lives in the overlay hook.

---

## 9. Known bugs / outstanding

*(None currently blocking — `obj[96]`/`obj[157]` stress-mode desync from earlier was resolved.)*

---

## 10. Files of note

```
FM2KHook/src/
  core/
    main_loop_trampoline.{h,cpp}   — phase-dispatched main loop replacement
  hooks/
    hooks.cpp                      — MinHook detours, FPU pin, timeGetTime virtual clock
    input.cpp                      — native input capture + facing-swap undo
  netplay/
    netplay.cpp                    — GekkoNet Save/Load/Advance handlers
    savestate.{h,cpp}              — save coverage, xxHash3, per-region CRCs
    control_channel.{h,cpp}        — multiplexed UDP, control packets, peer learning
    netplay_state.h                — CtrlPacket types
  audio/
    sound_rollback.{h,cpp}         — Mike Z-style rollback-safe sound layer

vendored/
  xxhash/xxhash.h                  — v0.8.2 single-header

FM2K_RollbackClient.cpp            — launcher main controller
FM2K_LauncherUI.cpp                — ImGui UI
FM2K_NetworkSession.cpp            — launcher-side session management
FM2K_Integration.h                 — FM2K memory map
```

---

## 11. Archived / superseded docs

The following remain in `docs/` for historical reference but should **not** be consulted as current spec:

- `FM2K_Rollback_Plan.md`
- `FM2K_Rollback_TestPlan.md` (superseded by §6)
- `rollback_architecture.md`
- `LilithPort_SDL3_Migration_Strategy.md` (superseded by §7)
- `Launcher_Roadmap.md`
- root-level `FM2K_CCCaster_Style_Implementation.md`, `cccaster_*.md`, `pure_gekkonet_implementation.md`, `FM2K_Implementation_Plan.md` (exploration / design history)
