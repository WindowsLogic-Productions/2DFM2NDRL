# FM2K Character Select Screen (CSS) state machine

Decompile pass against `WonderfulWorld_ver_0946.exe`, 2026-05-08. Used to design
the CSS auto-lock-and-confirm hook (task #24) that lets offline replay playback
skip CSS without driving it via fake input streams.

## Top-level dispatcher

`game_state_manager` @ **0x406FC0** owns the CSS state machine. Called from the
main game loop while `g_game_mode` (0x470054) is in CSS phase.

It dispatches on a **substate** stored at `*(g_object_data_ptr + 338)`
(`g_object_data_ptr` is at 0x4CFA00; the +338 byte is the per-CSS-instance
substate field, NOT a static address).

| substate | meaning |
|----------|---------|
| 0 | Initialize — clears action_state[], cursor_pos, round_timer_counter; sets `g_game_mode = 0x7D0` (2000); transitions to substate 1 |
| 1 | Active selection loop — reads inputs, moves cursors, handles confirm presses |
| 4 | Exit — calls `ResetObjectsAndCalculateSpeed()` then `create_game_object(12, 127, 0, 0)` to spawn the type-12 battle-init object |

Branching by `g_game_mode_flag` (0x470058):
- 0 = 1P
- 1 = VS 1v1
- 2 = Team VS

## Critical globals

| address | name | width | meaning |
|---------|------|-------|---------|
| 0x470054 | `g_game_mode` | u32 | Phase scalar — 1000=title, 2000=CSS, 3000=battle |
| 0x470058 | `g_game_mode_flag` | u32 | 0=1P / 1=VS / 2=Team |
| 0x470020 | `g_p1_selected_char_idx[0]` | i32 | P1 selected char (grid index, -1 = unselected) |
| 0x470024 | `g_p1_selected_char_idx[1]` | i32 | P2 selected char (same array, +4) |
| 0x424E50 | `g_p1_cursor_pos` | i32×2 | {x, y} struct on the char grid |
| 0x424E58 | `g_p2_cursor_pos` | i32×2 | {x, y} struct on the char grid |
| 0x47019C | `g_p1_action_state[0]` | u32 | P1 confirmed flag (0=selecting, 1=locked) |
| 0x4701A0 | `g_p2_action_state` | u32 | P2 confirmed flag |
| **0x424F00** | `g_round_timer_counter` | u32 | Frames since both confirmed; > 100 triggers battle |
| 0x447F40 | `g_processed_input[]` | u32×2 | Per-player processed input — bits 1/2/4/8 = LRUD |
| 0x447F60 | `g_input_changes[]` | u32×2 | Per-player input edges — bits 0x3F0 = attack/confirm |
| 0x4280D8 | `g_combined_input_changes` | u32 | bit 0x400 (Start) = exit CSS |
| 0x4452B8 | `g_stage_width` | u16 | Char grid width (for `cursor.x + cursor.y * stage_width = char_idx`) |
| 0x4452BA | `g_stage_height` | u16 | Char grid height |
| 0x4452CC | `g_char_availability_flags` | u8[] | Per-slot availability (bit 1 = 1P, bit 2 = VS) |
| 0x435474 | `g_char_slot_data` | byte[256][N] | Per-slot data; `[256*idx]==0` means slot unused |
| 0x424F24 | `g_css_active_player` | u32 | Player side currently selecting (1P mode) |
| 0x4CFA00 | `g_object_data_ptr` | ptr | Used as base for CSS substate (`+338`) and cursor obj ptrs (`+358`/`+362`) |

**Globals.h fix applied 2026-05-08**: `ADDR_ROUND_TIMER_COUNTER` was at the
incorrect 0x47008E (zero xrefs in IDA — dead memory). Real address is 0x424F00,
verified by 7 xrefs from `game_state_manager`.

## Selection flow (VS 1v1, mode 1)

For each player slot 0..1, while `!g_p1_action_state[player]`:

1. **D-pad** (`g_processed_input[player] & 0xF`) → `WrapPositionToStageBounds(cursor_pos, dx, dy)`
2. Compute `new_grid_pos = cursor_pos.x + cursor_pos.y * g_stage_width`
3. If cursor moved AND previous selection was `-1`:
   - Validate: `new_grid_pos < 50` AND `g_char_slot_data[256*new_grid_pos] != 0` AND `(g_char_availability_flags[new_grid_pos] & 2) != 0`
   - Set `g_p1_selected_char_idx[player] = new_grid_pos`
   - **Call `player_data_file_loader(player, new_grid_pos)`** @ 0x4039F0 — loads the .player file (sprites, anims, audio)
   - Create portrait object via `create_game_object(4, 80, ...)`
4. **Attack button** (`g_input_changes[player] & 0x3F0`) on a valid slot:
   - `g_p1_action_state[player] = 1` (lock)
   - `AssignPlayerColor(player, input_changes)` — color picked from which attack button was pressed
   - Replace cursor sprite with confirm sprite via `CreateProjectileObject(g_css_confirm_sprite_id, 101, 0, 0)`

After both players locked, `g_round_timer_counter++` each frame. When `> 100`:
- `ResetObjectsAndCalculateSpeed()`
- `create_game_object(14, 127, 0, 0)` — spawns the battle-init object that drives `game_mode 2000 → 3000`

## Auto-CSS replay strategy

**Goal**: replay file's MATCH_START header carries `pb_p1_char` / `pb_p2_char`
(post-C6 fix). On entering CSS, force the local game's selection state to
those chars + flip both action_state flags, and the natural CSS→battle
transition fires after the standard 100-frame timer.

**Approach (post-call hook on `game_state_manager`):**

Each frame in mode 2000:
1. Compute target cursor positions from `pb_*_char` and `g_stage_width`
2. Write cursor positions (locks them against any battle-input bits leaking in from pb_queue)
3. If `!g_p1_selected_char_idx[player]` (still -1): the natural state-1 code on next frame will detect cursor-move and call `player_data_file_loader` automatically
4. Once selected_char_idx is set: inject a confirm bit into `g_input_changes[player]` so action_state flips on next frame
5. Once both action_states == 1: stop poking; let `g_round_timer_counter` tick to 101

**Worst-case timeline** (in CSS frames):
- Frame 0: substate flips 0→1, cursors at (0,0)
- Frame 1: write cursor pos to char grid, game detects move, file load fires
- Frame 2: inject confirm bits, both action_states lock
- Frame 3..103: round_timer_counter ticks
- Frame 104: type-14 battle-init object spawns, game_mode 2000→3000

So replay startup adds ~104 frames (1.04s at 100fps) for CSS skip. Audio is
muted during catchup (per existing trampoline policy), so no jarring CSS
preview clips.

**Why post-call vs replace**: the natural state-1 logic does a lot of the work
(file loading, portrait creation, color assignment, cursor sprite setup). We
just want to override input/state writes to constrain the result. A pure
override (skip game_state_manager entirely + jump to battle) would require
duplicating .player file loads + battle-init setup, which is fragile.

## Stage select

VS / Team modes don't have a separate stage-select screen — stage is implicit
from the chars/map config. `ADDR_SELECTED_STAGE = 0x43010C` is set elsewhere
(via random-stage hook at `STAGE_SELECT = 0x408756` per `docs/dev/random_stage_design.md`,
or by the natural battle-init path).

For replay determinism, the C6 MATCH_START header now carries the recorded
`stage_id`; the auto-CSS hook can re-write `*(uint32_t*)0x43010C = pb_stage_id`
to pin it before `create_game_object(14, ...)` fires. Verifying whether stage
ID is read after the hook fires (or earlier, requiring a different write site)
is task-#24's responsibility.

## Functions of interest

| addr | name | role |
|------|------|------|
| 0x406FC0 | `game_state_manager` | CSS dispatcher (the hook target) |
| 0x407D70 | `ProcessCharacterSelectHandler` | Inner handler called from state-1 — was where C6 chars came from |
| 0x4039F0 | `player_data_file_loader` | Loads `<char>.player` file (sprites/anims/audio) |
| 0x406570 | `create_game_object` | Spawns engine objects; type 14 = battle-init |
| 0x406450 | `ResetObjectsAndCalculateSpeed` | Per-state-transition object pool reset |
| 0x406890 | `CreateProjectileObject` | Creates cursor / confirm sprites |
| 0x406F20 | `AssignPlayerColor` | Maps confirm-button index → palette index |
| 0x406EE0 | `GetButtonIndexFromMask` | Bit-scan over `0x3F0` attack mask |
| 0x406E70 | `WrapPositionToStageBounds` | Cursor wrap (cursors wrap at grid edges) |
| 0x4084F0 | `ProcessMenuSelectionInput` | Title-menu input (separate from CSS) |
| 0x4080A0 | `title_screen_manager` | Title screen state machine |
| 0x4086A0 | `vs_round_function` | In-battle round-state machine (already hooked for ROUND_START/END events) |
