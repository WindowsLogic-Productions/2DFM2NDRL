# FM95 Save State Inventory

Surgical inventory of FM95 (CPW) state that mutates per-frame and must be
captured for rollback determinism. Sourced from IDA xrefs/decomp on the
live CPW IDB, not guessed.

Status: inventory complete. `SaveStateData` struct skeleton lives in
`FM2KHook/src/netplay/savestate.h` under `#if defined(ENGINE_FM95)`. The
actual `SaveState_Save/Load` bodies are stubs in `savestate.cpp` (warns
once on first call, returns `true`); wiring this inventory into real
captures is the next step.

---

## Block A — Object pool

| Address | Symbol | Size | Why save |
|---------|--------|------|----------|
| `0x426A40` | `g_object_pool` | `0xA400` (256 × 164) | bulk-mutated every frame by `update_game_object` for every active slot — physics, hitbox state, animation counters, script vars |

Object slots are 0xA4 bytes each per `FM95::OBJECT_POOL_STRIDE`. Iterator state
(`g_object_iter_ptr`, `g_object_iter_index`, `g_current_object_id`) does NOT need
saving — `update_game_state` resets all three at the top of each tick.

## Block B — Frame counters + RNG + game mode

| Address | Symbol | Size | Why save |
|---------|--------|------|----------|
| `0x4DD7A8` | `g_game_tick_counter` | 4 | incremented every frame; LSB is parity for object iter direction |
| `0x4243FC` | `g_random_seed` | 4 | LCG state; consumed by `_rand` |
| `0x425558` | `g_game_mode` | 4 | written by state machines (vs_round_function, title_screen_state_machine) |

## Block C — Timer subsystems (palette flash / shake equivalents)

Three contiguous timer blocks at `0x509080..0x5090B0` (48 B). All decremented
inside `update_game_state` when their corresponding active flag is non-zero.
Treat as one opaque blob.

| Range | Purpose |
|-------|---------|
| `0x509080..0x509093` | Timer block A: counter + 3 sub-timers (palette flash 1 analogue) |
| `0x509094..0x5090A3` | Timer block B: counter + 1 sub-timer (palette flash 2 analogue) |
| `0x5090A4..0x5090AF` | Timer block C: counter + 1 sub-timer (shake-style timer) |

## Block D — Input rings + edge-detection state

| Address | Symbol | Size | Why save |
|---------|--------|------|----------|
| `0x437700` | `g_input_buffer_index` | 4 | mod-256 ring cursor |
| `0x431720` | `g_p1_input_history` | 0x400 (256 × 4) | P1 ring |
| `0x431B20` | `g_p2_input_history` | 0x400 | P2 ring |
| `0x431320` | `g_input_history_extra` | 0x400 | combo-replay extra ring (read by character_state_machine motion lookups) |
| `0x437750` | `g_p1_input` / `g_p2_input` | 8 | current-frame combined inputs |
| `0x425500` | `g_p1_input_persistent` | 4 | persistent (held) input state |
| `0x4255A8..B7` | `g_p1/p2_input_current/pressed` | 16 | per-player edge detection — old + new state |

## Block E — Per-player state arrays (25-stride, both players)

Range: `0x5E98A0..0x5E994B`. Each player has ~25 dwords; both players covered
by saving the full ~344-byte block (p1 base + 25 × 4 = 100 B for p1, then p2
follows the same stride pattern through the next 25-int slot).

| Address | Symbol | Stride note |
|---------|--------|-------------|
| `0x5E98A0` | `g_p_pos_x_snap` | written end-of-frame from object pool |
| `0x5E98A4` | `g_p_pos_y_snap` | same |
| `0x5E98A8` | `g_p_facing_snap` | written end-of-frame from score comparison |
| `0x5E98AC` | `g_p_score_value` | round score |
| `0x5E98B0` | `g_p_round_lose_count` | rounds lost |
| `0x5E98B4` | `g_p_round_win_count` | rounds won |
| `0x5E98B8` | `g_p_meter_level` | super meter level |
| `0x5E98BC` | `g_p_meter_progress` | super meter charge |
| `0x5E98C0..E4` | `g_p_combo_var0..9` | combo state (10 dwords) |
| `0x5E98C8` | `g_p_combo_hits` | hit counter |
| `0x5E98CC` | `g_p_combo_damage` | combo damage accumulator |
| `0x5E98D0` | `g_p_combo_max` | max combo length |
| `0x5E9900` | `g_p_partner_object` | tag-team partner pointer |
| `0x5E9908` | `g_p_partner_input` | partner input |
| `0x5E9930` | `g_p_motion_table_idx` | motion-input lookup cursor |
| `0x5E9940` | `g_p_motion_idx2` | secondary motion cursor |

## Block F — Round state

Range: `0x5E9904..0x5E9A40`. ~316 bytes covering both players + global round
state. Contains:

| Address | Symbol |
|---------|--------|
| `0x5E9904` | `g_round_state_score_a` (P1 round score in current state) |
| `0x5E994C` | `g_round_p1_score` (P1 round-final score) |
| `0x5E9968` | `g_round_state_score_b` (P2 active) |
| `0x5E99B0` | `g_round_p2_score` (P2 final) |
| `0x5E99CC` | `g_round_state_score_c` (tertiary) |
| `0x5E9A30` | `g_round_state_var0` |
| `0x5E9A34` | `g_round_time_limit` |
| `0x5E9A38` | `g_round_state_var1` |
| `0x5E9A3C` | `g_round_count_max` |

## Block G — Sound rollback (engine-agnostic)

`SoundRollback::DesiredState sound_desired[MAX_CHANNELS]` — same Mike Z
desired-state record we use on FM2K. Already in the `SaveStateData` skeleton
under `#if defined(ENGINE_FM95)` since the sound layer is engine-agnostic.

## What's NOT saved (FM95 doesn't have these)

- Afterimage pool / motion-blur trails — FM2K uses 0x447930+; FM95 has no equivalent subsystem
- Object linked-list topology — FM95's iteration walks the array directly, no separate list
- Input tracking state at FM2K's 0x447EE0 — FM95 stores its edge state inline at `0x4255A8..B7`
- FM2K palette-flash arithmetic structures at `0x447D7D` / `0x4456B0` — FM95's analogues are the dword timer blocks at `0x509080+`, already covered in Block C
- FM2K's `g_round_end_flag` at 0x424718 — no direct FM95 equivalent (state lives in vs_round_function's per-object sub_state field)

## Estimated total per-slot size

| Block | Size |
|-------|------|
| A: object pool | 41 KB |
| B: frame/RNG/mode | 12 B |
| C: timer subsystems | 48 B |
| D: input rings + edge state | 3.1 KB |
| E: per-player state | 344 B |
| F: round state | 316 B |
| G: sound desired | (depends on MAX_CHANNELS) |

**~45 KB per save slot × 64 rollback slots = ~2.9 MB total state buffer.**

That's an order of magnitude lighter than FM2K's 850 KB-per-slot, because FM95
has no afterimage pool, no input-tracking subsystem, and a much smaller object
pool (256 × 164 vs 1024 × 382).

## Implementation plan

1. In `savestate.h`, replace the FM95 placeholder fields (object_pool,
   input_history, game_mode, etc.) with the full inventory above as named
   structs in the FM95 `SaveStateData` ifdef branch.
2. In `savestate.cpp`'s `#if defined(ENGINE_FM95)` block, replace the stub
   `SaveState_Save` body with `memcpy` of each block from its source
   address into the SaveStateData fields. Same pattern for `SaveState_Load`.
3. Compute per-region Fletcher32 CRCs into `saved_region_crcs` so the
   existing desync-diagnostic flow surfaces FM95-specific divergence.
4. Test offline first — confirm save/load roundtrips are bit-equal.
5. Then `FM95_TRAMPOLINE=1 + FM2K_STRESS_MODE=1` for the determinism test
   (Task 16). GekkoStressSession forces rollback every N frames and
   compares hashes — first real rollback determinism gate on FM95.
