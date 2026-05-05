# FM2K RNG Audit — WonderfulWorld_ver_0946.exe

**Scope:** Exhaustive audit of every randomness source in the FM2K runtime, for the
purpose of validating the rollback savestate. If the RNG state isn't
captured + restored byte-for-byte on rollback, any `game_rand()` call between
save and restore will return a different value on replay and cause an
irrecoverable desync.

**Audit date:** 2026-04-23
**Binary:** `WonderfulWorld_ver_0946.exe` (MSVC 6.x era, Win32)
**Tools:** IDA MCP (function decompilation, xref enumeration, code-ref scanning)

---

## 1. Executive summary

- The game uses a **single custom LCG** (NOT libc `rand`/`srand`) — all randomness
  funnels through one 30-byte function `game_rand` at **0x417a22**.
- RNG state is a **single 4-byte global** `g_rand_seed` at **0x41fb1c**.
- The LCG uses the **MSVC `rand()` constants** (`0x343FD` / `0x269EC3`) but
  reimplemented as its own function — not an import of `rand`/`srand`.
- `srand()` is **NEVER called** at runtime. `time()`, `timeGetTime()`, and
  `GetTickCount()` are *not* routed to the seed. The only time `g_rand_seed`
  is written is inside `game_rand` itself.
- The binary is **statically-initialized** with `g_rand_seed = 1` in the `.data`
  section. Every game boot starts with the same seed, and every `game_rand`
  call advances it deterministically.
- **Confirmation the binary has no second RNG:** a full scan of every
  instruction immediate for alternate LCG multipliers (`glibc 1103515245`,
  `MINSTD 16807 / 48271`, `Borland 22695477`, `Numerical Recipes 1664525`,
  `MT19937 1812433253`) found **no matches**. The only LCG constants in the
  image are the two used by `game_rand`.
- **Confirmation `g_rand_seed` is not aliased:** a full `DataRefsFrom` walk
  across every function returned exactly 2 hits, both inside `game_rand`
  (read at 0x417a22, write at 0x417a32). No other function reads or writes
  the seed directly.

## 2. The RNG function and seed

### `game_rand` — 0x417a22 (30 bytes)

```c
int __cdecl game_rand()
{
  g_rand_seed = 214013 * g_rand_seed + 2531011;   // 0x417a27..0x417a32
  return (g_rand_seed >> 16) & 0x7FFF;            // 0x417a37..0x417a3a
}
```

Disassembly:

```asm
417a22  mov     eax, g_rand_seed
417a27  imul    eax, 343FDh        ; MSVC rand() multiplier
417a2d  add     eax, 269EC3h       ; MSVC rand() increment
417a32  mov     g_rand_seed, eax
417a37  sar     eax, 10h
417a3a  and     eax, 7FFFh
417a3f  retn
```

- **Range:** `[0, 32767]` — identical to MSVC CRT `rand()`.
- **Period:** ~2^32 (LCG period = modulus = 2^32).
- **Not an import:** the binary links `KERNEL32/USER32/GDI32/DDRAW/DPLAYX/DSOUND/WINMM/WSOCK32` but imports **no CRT `rand` / `srand`**.

### `g_rand_seed` — 0x41fb1c (4 bytes, DWORD)

- **Section:** `.data` (writable, not relocated)
- **Initial value in image on disk:** `0x00000001`
- **Touched by:** `game_rand` only (verified exhaustively via IDA
  `idautils.DataRefsFrom` over every instruction in every function).
- **No `srand` equivalent anywhere** — the seed is **never reset** from
  `time()`, `timeGetTime()`, `GetTickCount()`, `QueryPerformanceCounter()`,
  joystick state, mouse position, or any other nondeterministic source.

**IDA:** renamed to `g_rand_seed`; added `// RNG STATE — must be savestated`
class comment at the address and on `game_rand`.

## 3. Every `game_rand()` call site

12 call sites across 7 functions. Classified by whether they execute on the
deterministic **SIM** tick (via `update_game_state`) or on the
**RENDER** path (via `render_game`). Render-path RNG is a well-known rollback
hazard because the render loop runs every frame including during rollback
resimulation, and the number of render calls can differ between peers
(rollback overhead on P2 etc).

| # | Site addr | Function | Path | Game effect |
|---|-----------|----------|------|-------------|
| 1 | 0x40b3c5 | `camera_manager` state 21 | SIM (`update_game_object`) | Afterimage/ghost sprite RGB jitter — `(g_rand()&1)` mutates offsets `+350/+354/+358` (object dynamic color offsets for trail effect). 3rd call on 0x40b3f3 has its value discarded. |
| 2 | 0x40b3e2 | `camera_manager` state 21 | SIM | (same) |
| 3 | 0x40b3f3 | `camera_manager` state 21 | SIM | (same) — value discarded, but seed is still advanced. |
| 4 | 0x40ca5d | `ProcessShakeEffect` case 4 | **RENDER** (`render_game`) | Screen-shake random offset: `a1[1] = game_rand() % v5` when shake mode==4 ("random shake" type). |
| 5 | 0x40cacf | `ProcessColorInterpolation` mode 3 | **RENDER** (`render_game -> sprite_rendering_engine -> ProcessColorInterpolation`) | Object color-interpolation "random blend" mode: `v6 = game_rand() % 100` then blends source/target RGBA by that percentage. Sets object color components (+68, +18, +19, +20). |
| 6 | 0x40cf9b | `sprite_rendering_engine` sprite-blend case 4 | **RENDER** | Per-sprite random color jitter during per-frame blend mode 4 (sprite color-shift effect). Same `rand()%100` blend trick. |
| 7 | 0x40f3d5 | `hit_detection_system` | SIM (`finalize_game_objects`) | **Guard / auto-block probability roll.** `game_rand() % 100 >= *(target_character_data + 57185)` — if target has auto-guard flag set (offset +57181), this roll decides whether an incoming hit is auto-blocked. Called **on every hit check**, so the RNG is advanced during combat. |
| 8 | 0x411356 | `ai_input_processor` | SIM (`character_action_controller`) | AI directional-input randomization: `game_rand() % (101-edi) - edi + 50` computes a distance threshold stored at `+0xDF7D` (AI "distance jitter" for approach decisions). |
| 9 | 0x41139b | `ai_input_processor` | SIM | AI action-selection probability: `game_rand() % 100` compared against AI script entry's "probability" byte (`[ebx-1]`). Gates whether AI triggers a scripted action this tick. |
| 10 | 0x41149f | `ai_input_processor` | SIM | AI action-timer jitter: `game_rand() % (101 - target_difficulty)` added to the action-duration field at `+0xDF79`. Determines how long an AI holds its chosen input. |
| 11 | 0x411ebc | `character_state_machine` (init branch) | SIM (`update_game_object`) | **Initial HP variation at character spawn.** `hp += 100 * (game_rand() % max_hp_variance)` — applies random HP bonus on round start. This is the "HP jitter" feature. |
| 12 | 0x4139a3 | `character_state_machine` opcode `0x20` | SIM (`update_game_object`) | **Character-script random-branch opcode (the "Random" opcode from the editor).** `game_rand() % (*(script+1) + 1) <= *(script+3) ? do_random_action(*(script+6))` — this is the primary scripting primitive that character move scripts use to produce randomized behavior (e.g., "50% chance to play this animation", random afterimage state transitions, random projectile variant). Every character script that uses opcode `0x20` calls this. |

### SIM-vs-RENDER breakdown

- **SIM path (rollback savestate covers these correctly):** sites 1–3, 7, 8, 9, 10, 11, 12.
- **RENDER path (NOT covered by a naive savestate, need the existing save/restore-around-render workaround):** sites 4, 5, 6.

The FM2KHook codebase already implements the render-path workaround in
`FM2KHook/src/hooks/hooks.cpp:359` (save `g_rand_seed` before `render_game`,
restore after). This is the correct mitigation — render-path RNG consumption
is *visual only* and doesn't feed back into the sim.

## 4. Initialization and seeding

- **Static `.data` initializer:** `g_rand_seed = 0x00000001`.
- **No `srand()` call site** anywhere in the binary (no matching byte pattern,
  no import, no `time()`/`timeGetTime()`/`GetTickCount()` value ever written
  to `0x41fb1c`).
- **Implication:** a fresh process launch on both peers starts with identical
  seeds. Any divergence is purely from unsynchronized `game_rand` calls (e.g.,
  pre-CSS RNG drift), which is already handled by the launcher re-seeding the
  RNG at `CSS → battle` transition (`FM2KHook/src/netplay/netplay.cpp:376`).

## 5. How this is currently handled in FM2KHook

Confirmed already present:

- `FM2KHook/src/core/globals.h:40` — `constexpr uintptr_t ADDR_RANDOM_SEED = 0x41FB1C;`
- `FM2KHook/src/netplay/savestate.cpp:12` — `constexpr uintptr_t ADDR_RNG_SEED = 0x41FB1C;`
- `FM2KHook/src/netplay/savestate.cpp:128..129` — `state->rng_seed = *(uint32_t*)ADDR_RNG_SEED;` (save)
- `FM2KHook/src/netplay/savestate.cpp:198..199` — `*(uint32_t*)ADDR_RNG_SEED = state->rng_seed;` (restore)
- `FM2KHook/src/netplay/savestate.cpp:441` — region descriptor `{ 0x41FB1C, 4, "RNG_Seed", false }`.
- `FM2KHook/src/netplay/netplay.cpp:376..382` — re-seed after CSS sync.
- `FM2KHook/src/hooks/hooks.cpp:359..376` — save/restore RNG around `render_game`.

**Verdict:** the savestate system already captures the seed, and the
render-path leak is already patched. So if RNG is the desync cause, the issue
is likely one of:

1. **Timing of the save** — is the RNG captured *before* the frame's first
   `game_rand` call, or after? If the frame tick calls `game_rand` before the
   savestate snapshots, the saved value already reflects post-call state, and
   re-simulation on rollback will double-advance the seed.
2. **Checksum miss** — is `g_rand_seed` included in the desync-detection
   checksum? If not, a diverged RNG won't trigger detection until it manifests
   in a visible object state much later, making it hard to trace.
3. **Order of render vs netplay handler** — if `Netplay_HandleFrameTime` or
   the shared-memory monitor reads the seed *between* `render_game` and the
   restore, it snapshots the visual-only post-render value.

## 6. Test plan — how to reproduce a desync if RNG isn't savestated correctly

The goal is to force rollback during a frame that consumes `game_rand`, and
verify both peers remain in lockstep. If RNG save/restore is broken you
will see it here deterministically within ~5 seconds of gameplay.

### Test A — script opcode `0x20` (most reliable)

1. Pick any character whose move scripts use opcode `0x20` (most do — it's
   the "Random" branch).
2. Start a 2-player netplay match with >0ms simulated latency (enough to
   force input prediction + rollback every frame — e.g., 4f delay).
3. Have **both** players repeatedly mash the move that triggers a `0x20`
   branch (e.g., a super or a taunt with randomized outcomes).
4. **Expected (correct):** both clients show identical sprites/damage every
   round, and the desync checksum stays `0x00000000`.
5. **Failure signature (broken RNG restore):** within 100 frames the two
   clients diverge — one client's character plays animation path A, the
   other plays path B. Desync checksum delta appears at the exact frame the
   `0x20` opcode fires.

### Test B — guard-probability hits (faster to trigger if chars have auto-guard)

1. Pick a character with the auto-guard flag set
   (`character_data[+0x DF5D] != 0`).
2. In netplay, have the opponent mash a hit-confirm string.
3. Every hit triggers `game_rand() % 100` at 0x40f3d5. If RNG is mishandled,
   one client will show the hit landing and the other will show a guard
   within one frame of the hit connecting.

### Test C — initial HP jitter (easiest to diagnose, only fires once per round)

1. Use a character roster where at least one character has non-zero
   `max_hp_variance` (character data offset +0x0C of HP struct, see call
   site 11 above).
2. Start a netplay match. At round start, `character_state_machine` fires
   `game_rand() % max_hp_variance` to jitter HP.
3. **Failure signature:** the two clients show different starting HP for the
   jittered character. Trivially visible in the HUD — HP will differ by up
   to `max_hp_variance * 100` units between peers.

### Test D — synthetic forced-rollback

If you have a debug hook to force rollback every N frames regardless of
network conditions:

1. Hook `update_game_object` to call `game_rand()` extra times (e.g., 7
   calls per frame) before the normal sim runs.
2. Force 8 frames of rollback every second via debug toggle.
3. Compare `g_rand_seed` at `save_state` time vs `restore_state` time vs
   post-resimulation. All three must be identical. If they differ, the
   savestate ordering is wrong.

### Test E — checksum-level detection

1. Add `g_rand_seed` to the desync-detection checksum region (if not
   already). Confirm it is in `g_region_checksums.rng` at `savestate.cpp:311`.
2. Watch the shared-memory overlay (`imgui_overlay.cpp:76` already prints
   `RNG: 0x%08X | BufIdx: %u`) — both peers must display identical RNG every
   frame. Any frame where they differ is a savestate bug.

### Diagnostic telemetry to add

- Log `g_rand_seed` value at 4 points per frame: frame-start, post-sim,
  pre-render, post-render. Both peers should match at frame-start and
  post-sim; pre-render vs post-render divergence is expected (render
  consumes RNG) but the **save/restore around render** must make
  post-restore identical to pre-render.
- Log a counter of `game_rand` calls per frame. Both peers must agree.
  A divergence in call count is the smoking gun — it means one peer is
  executing a sim path the other isn't (most likely due to an input
  desync or a ClientState object not being savestated).

## 7. Renames / annotations applied in IDA

- `g_rand_seed` @ `0x41fb1c` — renamed from `g_random_seed` to match project
  naming convention, comment added: *"RNG STATE — must be savestated.
  MSVC-style LCG seed … NEVER re-seeded (no srand/time call)."*
- `game_rand` @ `0x417a22` — comment added documenting every call-site
  category and pointing at this audit.

## 8. Additional findings

- `sprite_rendering_engine` (6 KB function) has a third `game_rand` call
  that the prior audit missed (call site 6 above, inside blend mode 4).
  This is a render-path call and is already covered by the existing
  save/restore-around-render workaround.
- `camera_manager` case 21 consumes **3 rand values per frame** while the
  afterimage/ghost state is active. This drastically accelerates seed
  advancement — if either peer's afterimage state toggles on/off at a
  different frame, seed drift is immediate and irreversible.
- The `g_rand_seed` live value at the time of this audit was `0x06C97E58`
  — confirming RNG has advanced significantly from the `0x1` static initial
  value during normal gameplay.
