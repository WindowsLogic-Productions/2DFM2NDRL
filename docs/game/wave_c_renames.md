# Wave C Rename Report — game `.data` 0x4A0000..0x4F0000

Session cleanup of the per-player runtime state block and palette/character-data
region in `WonderfulWorld_ver_0946.exe`. **38 auto-named globals enumerated, 38
renamed, 0 left** (task hint of 43 was slightly high — some had already been
renamed in prior passes, e.g. `g_p1_opponent_ptr@0x4DFC7D` and
`g_charSlot{1..6}_round_action_flag@0x4FBE05..0x541F40`).

## Summary

| | Before | After |
|---|---|---|
| Auto-named globals (`dword_`, `word_`, `byte_`, `unk_`) in range | 38 | 0 |
| User-named globals in range | ~92 | 130 (+38) |

No `__dword_<addr>` fallbacks; every global in the Wave C range now has a
semantic name.

## The 57407-byte Per-Player Block

`byte_4D1D90` (5 xrefs, 256-byte name buffer) is the **base address of the
per-player runtime struct**, stride 57407 bytes, 8 slots (0..7). Old auto-named
p2 fields inside this block were consistently 57407 bytes after their p1
counterparts — labeled them `g_p2_*` for traceability. The game's stride is
longer than the editor's `KgtPlayerRuntimeSlot` (47851) but the field semantics
line up with `runtime_entity.md`.

Per-player struct fields named this wave (offsets relative to `g_player_name_buffer`):

| Game addr | Offset | Type | Name | Editor analogue |
|---|---|---|---|---|
| `0x4D1D90` | `+0`      | `char[256]` | `g_player_name_buffer`       | (netplay) |
| `0x4D3FB8` | `+0x2228` | `int`       | `g_net_connection_state`     | (netplay: 0/1/2=joined/here/left) |
| `0x4DFC7D` | `+0xDEED` | `ptr`       | `g_p1_opponent_ptr` *(pre)*  | `KgtPlayerRuntimeSlot.opponentPtr` @ +0xB9F9 |
| `0x4DFC85` | `+0xDEF5` | `int`       | `g_p1_hp` *(pre)*            | `lifePercent`-adjacent |
| `0x4DFC89` | `+0xDEF9` | `int`       | `g_hp_percent_x1000`         | `lifePercent` @ +0xBA01 (0..1000) |
| `0x4DFC91` | `+0xDF01` | `int`       | `g_p1_max_hp` *(pre)*        | (max HP) |
| `0x4DFC95` | `+0xDF05` | `int`       | `g_char_value_current` *(pre)* | `specialPercent` (integer gauge level) |
| `0x4DFC99` | `+0xDF09` | `int`       | `g_char_value_current_max`   | (max gauge level count) |
| `0x4DFC9D` | `+0xDF0D` | `int`       | `g_char_value_flag` *(pre)*  | (progress within current level) |
| `0x4DFCA1` | `+0xDF11` | `int`       | `g_char_value_flag_max`      | (max for flag; clamp/rollover threshold) |
| `0x4DFCAD` | `+0xDF1D` | `int`       | `g_combo_hit_flags` *(pre)*  | |
| `0x4DFCB1` | `+0xDF21` | `int`       | `g_combo_state_extra`        | (per-frame cleared next to combo flags) |
| `0x4DFCC1` | `+0xDF31` | `int32`     | `g_player_pos_x`             | `KgtRuntimeObject.posX` cached here |
| `0x4DFCC5` | `+0xDF35` | `int32`     | `g_player_pos_y`             | `KgtRuntimeObject.posY` cached here |
| `0x4DFCCD` | `+0xDF3D` | `int`       | `g_facing_prev_frame`        | (previous-tick value of `g_char_active_flag`) |
| `0x4DFCDD` | `+0xDF4D` | `int`       | `g_player_enabled_flag`      | (0/1; gates debug HP seed) |
| `0x4DFCE1` | `+0xDF51` | `int`       | `g_debug_hp_percent_seed`    | (set to 100 via -c flag) |
| `0x4DFCE5` | `+0xDF55` | `int`       | `g_debug_hp_value_seed`      | (set to `g_config_value2`) |
| `0x4DFCE9` | `+0xDF59` | `ptr`       | `g_nearest_opponent_ptr`     | `nearestOppPtr` @ +0xBA41 |
| `0x4DFD09` | `+0xDF79` | `int`       | `g_net_sync_frame_counter`   | (per-slot input sync counter) |
| `0x4DFD12` | `+0xDF82` | `byte`      | `g_player_attack_flags`      | Related to `reactionType` @ +0xBA3D |
| `0x4DFD13` | `+0xDF83` | `byte`      | `g_player_attack_range_low`  | (attack-chain gate low) |
| `0x4DFD14` | `+0xDF84` | `word`      | `g_player_attack_chain_id`   | (chain-into anim id) |
| `0x4DFD16` | `+0xDF86` | `byte`      | `g_player_attack_range_high` | (attack-chain gate high) |
| `0x4DFD6F` | `+0xDFDF` | `byte`      | `g_p1_throw_hit_flag` *(pre)*| `throwHitFlag` @ +0xBAD3 |
| `0x4DFD77` | `+0xDFE7` | `int`       | `g_p1_throw_offset_x` *(pre)*| `throwOffsetX` @ +0xBADB |
| `0x4DFD7B` | `+0xDFEB` | `int`       | `g_p1_throw_offset_y` *(pre)*| `throwOffsetY` @ +0xBADF |
| `0x4DFD87` | `+0xDFF7` | `int`       | `g_charSlot0_round_action_flag` | (per-player round result: 1=win, 2=draw, 3=loss) |
| `0x4DFD8F` | `+0xDFFF` | `int`       | `g_player_alive_flag`        | `playerAliveFlag` @ +0x0138 |

The p2 mirrors (stride +57407, i.e. `+0xE03F`) were renamed `g_p2_*`:
`g_p2_hp_percent_x1000`, `g_p2_pos_x`, `g_p2_pos_y`, `g_p2_enabled_flag`,
`g_p2_debug_hp_percent_seed`, `g_p2_debug_hp_value_seed`, `g_p2_win_point_counter`,
`g_p2_char_value_current`, `g_p2_char_selected_action`,
`g_charSlot_p2_round_action_flag`, `g_p2_character_sprite_data`,
`g_p2_char_anim_data_hi`.

## Cluster Summaries

### 1. `g_palette_lut@0x4D1A20` (20 xrefs — HIGHEST in range)
256-word (512-byte) sprite blitter color LUT. Used by every branch of
`graphics_blitter@0x40ACA0` (normal/50%/additive/subtractive/alpha-blend)
and all three `BlitSpriteWithBlendMode` callsites in `sprite_rendering_engine`.
Source is a compiler-emitted color-channel-expand buffer: the first do/while
loop at `0x40b59d` packs `(r,g,b)` from each palette entry `v67` into RGB555
with color-shift adjustments, then subsequent blit branches sample
`g_palette_lut[*src_pixel]` for every output pixel.

### 2. Special-gauge two-level state (0x4DFC95..0x4DFCA1)
Per-player, four fields: `current`, `current_max`, `flag`, `flag_max`.
`UpdateAnimationFrame@0x40E6F0` implements a two-part fractional gauge:
`flag` overflows → `current++`, `flag` clamped to `[0, flag_max]`, `current`
clamped to `[0, current_max]`. Rendered by `sprite_rendering_engine` case 0x14
(self) / 0x15 (opponent) as a gauge bar: `v79 = v6 * g_char_value_flag /
g_char_value_flag_max`.

### 3. Position/camera state (0x4DFCC1..0x4DFCC5, 0x4EDD00..0x4EDD04)
`g_player_pos_x / g_player_pos_y` cache the renderable position of each
player's `KgtRuntimeObject` at 16.16 fixed. `camera_manager@0x40AC20` reads
the p1+p2 mirrors to recenter the camera; `ai_behavior_processor@0x4105B8`
uses `abs(self.x - opp.x)` for range checks.

### 4. Per-player round action flag (0x4DFD87, 0x4EDDC6, 0x4FBE05..0x541F40)
8-slot array of `int` at stride 57407, one per character slot. Values 1/2/3
mean **round win / draw / round loss** — written by `vs_round_function` at
state transitions 410/420/430 (single-player round outcome) and 520/530
(team-mode round outcome). The state-430 handler writes 3 to all 8 slots
(everyone loses). Already-named slots 2..7 used `g_charSlot{1..6}_*`
convention; added slot0/p2 consistent with that.

### 5. Attack chain + reaction state (0x4DFD12..0x4DFD16)
Five adjacent bytes/word read by `character_action_controller@0x411820` for
attack-chain resolution: `attack_flags & 7` selects branch (1 = "chain into
attack_chain_id", 2 = "test hit_confirm"), followed by `attack_range_low <=
hitframe <= attack_range_high` window test for cancellable frames.

### 6. Reaction timing (0x4DFD6F, 0x4DFD77, 0x4DFD7B) — pre-named
Already renamed in a prior wave to `g_p1_throw_hit_flag`, `g_p1_throw_offset_x/y`.
These are the throw/grab target-positioning state (`physics_collision_handler@0x40F910`
warps grabbed opponent to `self.pos ± throw_offset` when `throw_hit_flag & 0x10`).

### 7. Netplay per-slot state (0x4D3FB8, 0x4DFD09)
DirectPlay/netplay per-player slots:
- `g_net_connection_state` (values 1=left, 2=here/joined; set by
  `check_game_continue`, read by `camera_manager` to exclude disconnected
  players from centering).
- `g_net_sync_frame_counter` (incremented every frame a slot is active, used by
  `directplay_send_frame_inputs@0x4025A0` to advance playback after a join
  packet arrived).

### 8. Character data & stage config (0x4CF9E0, 0x4D931E, 0x4D9328, 0x4D9A23, 0x4D9A4E, 0x4D9A51)
- `g_player_loaded_char_slot[8]` (0x4CF9E0) — 8-element int array: which
  character slot id is loaded into each of the 8 player slots. `-1` = unloaded.
  `ClearCharacterSlot@0x4039B0` sets it to -1; `player_data_file_loader@0x403A00`
  reads/writes.
- `g_player_standup_anim_id` (0x4D931E) — per-player stand-up / idle animation
  id used by `character_action_controller` when round state==2 (round
  ended, standing).
- `g_character_win_anim_index` (0x4D9328) — animation id for a character's
  "winner" animation; used by `ui_state_manager` in demo/story mode 3+.
- `g_char_ai_attack_range` (0x4D9A23) — AI-config attack-range threshold in
  pixels; `ai_behavior_processor` compares `abs(self.x - opp.x)/0x10000` against
  this for cancel-into-attack decisions.
- `g_stage_script_mode_flags` (0x4D9A4E) — per-stage-script byte at stride 206:
  bit 0x01 gates `create_game_object(13)` end-of-match, bit 0x02 preserves
  match phase across stage reload, bit 0x04 enables demo-mode camera clamp.
- `g_stage_p1_spawn_x` (0x4D9A51) — per-stage-script word (stride 206/2=103)
  giving p1 spawn X for stage-mode matches.

## Surprises

1. **Two-level gauge system.** The special gauge is stored as *two* integers
   (`current_max`, `flag_max`) rather than one max — matches the "gauge level +
   sub-level progress" pattern. The editor's `KgtPlayerRuntimeSlot` only has
   `specialPercent` at +0xBA05; the game's runtime has the extra sublevel
   accumulator. Relevant for rollback state size.
2. **Player stride mismatch editor vs. game.** Editor is 47851, game is 57407 —
   delta 9556 bytes. The extra bytes in the game live roughly in the
   +0xDEED..+0xDFF7 band where I found `g_hp_percent_x1000`, the gauge
   `*_max` fields, position/facing caches, and the attack-chain bytes. The
   editor doesn't need these runtime-scratch fields (it's just a preview).
3. **Pre-existing renames are numbered inconsistently.** `g_charSlot1..6` at
   0x4FBE05..0x541F40 actually represent slots 2..7 (0-indexed), so I kept the
   existing convention and added `g_charSlot0_round_action_flag` for slot 0 —
   the numeric label is off-by-one from slot indices but internally consistent
   with the prior wave's names.
4. **Palette LUT is a COMPUTED table.** `g_palette_lut[256]` is rebuilt every
   frame in `graphics_blitter`'s inner do/while (lines 0x40b59d..0x40b680) from
   the sprite's 16-bit color table plus the caller's `color_r/g/b` additive
   offsets — then blit branches sample it. It's a scratch LUT, not character
   data, which is why it sits at 0x4D1A20 before the 0x4D1C20 char-data region.
5. **Opponent pointers come in pairs.** `g_p1_opponent_ptr@0x4DFC7D` (held
   during grabs/throws) vs. `g_nearest_opponent_ptr@0x4DFCE9` (rewritten every
   frame by `character_facing_controller`). Editor only has
   `nearestOppPtr@+0xBA41`; game adds a "locked-on" variant for throw/grab
   persistence.
6. **Netplay sync uses a simple counter.** `g_net_sync_frame_counter` increments
   once per frame per active slot from `directplay_send_frame_inputs`; spec
   hint was 7 xrefs (matches find count). The protocol is trivially a linear
   frame counter — relevant for anyone attempting to piggyback the existing
   DirectPlay sync vs. injecting GekkoNet alongside.

## Files affected

- **IDB renames:** 38 globals in `.data` range `0x4A0000..0x4F0000`
- **Docs updated:** this file (`/mnt/c/dev/wanwan/docs/game/wave_c_renames.md`)

## Follow-ups

- Confirm `g_player_attack_chain_id` by correlating with the editor's
  attack-chain opcode (probably `[BR]` or `[SC]` family) — the semantics of
  the 5-byte attack group at +0xDF82..+0xDF86 deserve a dedicated
  hit-junction-in-game doc.
- `g_character_win_anim_index@0x4D9328` was left under-investigated (1 xref,
  low signal) — could be win-pose animation id *or* a shared "announcer
  voice" trigger; confirm by watching post-round rendering.
- The `g_p2_*` mirror names are placeholders; in a proper struct pass, these
  should disappear behind a `KgtPlayerRuntimeSlot*`-typed array so the
  compiler-emitted per-slot constants are reconstructed as `slots[1].field`.
- `g_palette_lut` should ideally be typed `uint16_t[256]` and decorated as a
  scratch buffer — blit sites would then read `g_palette_lut[src]` directly
  instead of the cast-based `word_4D1A20[v57]` pattern.
