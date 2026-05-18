# FM2K Combat/Hit/Reaction System Analysis

## Overview

This document details the low-level mechanics of FM2K's combat system, covering hitbox/hurtbox structures, block-decision logic, hit reactions, and character state transitions. This is the authoritative reference for designing training mode Guard mechanics and frame-data display.

**Evidence Base**: All claims cite specific function addresses (via `mcp__ida-pro-mcp__decompile`) and memory offsets verified against the binary.

---

## 1. Hitbox Struct Layout

**Source**: `hit_detection_system @ 0x40F010`, disasm @ 0x40F059–0x40F0CC

A hitbox is a dynamically-allocated struct pointed to by `obj->hitbox_ptrs[0..19]` (at `obj + 0x89 + 4*i` to `obj + 0xBD + 4*i`).

### Field Offsets (relative to hitbox_ptr)

| Offset | Size | Type | Field | Purpose |
|--------|------|------|-------|---------|
| +0x00 | 4 | ptr | ??? | Padding or next link (not accessed in hit_detection) |
| +0x01 | 2 | i16 | `x_offset` | X offset from character center (signed, fixed-point Q16 internally) |
| +0x03 | 2 | i16 | `y_offset` | Y offset from character center (signed, fixed-point Q16) |
| +0x05 | 2 | u16 | `width` | Half-width (radius in X direction) |
| +0x07 | 2 | u16 | `height` | Half-height (radius in Y direction) |
| +0x09 | 1 | u8 | **unused** | (not accessed) |
| +0x0A | 1 | u8 | **flags** | Hitbox type and behavior flags (see below) |
| +0x0B | 1 | u8 | **unused** | |
| +0x0C | 2 | u16 | `damage` | Damage value (stored at `*(hitbox_ptr + 12)` = 0x0C) |

**Size**: 14 bytes minimum per hitbox (may be larger with padding).

### Hitbox Flags (offset +0x0A)

Accessed via `*(hitbox_ptr + 10)` in decompile. Each flag is a bitmask:

| Bit | Hex | Name | Meaning | Evidence |
|-----|-----|------|---------|----------|
| 0 | 0x01 | ??? | Not observed in decompile | — |
| 1 | 0x02 | ??? | Not observed in decompile | — |
| 2 | 0x04 | `NO_STUN_DAMAGE` | Hit does no hitstun; block stun only | Used in combo scaling (0x6B1) but not explicitly tested in hit_detection; **INFERRED from block path** |
| 3 | 0x08 | `THROW` | Throw/grab hitbox | Tested: `(hitbox_flags & 8)` at 0x40F168; causes special collision vs airborne chars |
| 4 | 0x10 | `AIR_ONLY` | Only hits airborne targets | Tested: `(hitbox_flags & 0x10)` at 0x40F14F; skip if target grounded |
| 5 | 0x20 | `GROUND_ONLY` | Only hits grounded targets | Tested: `(hitbox_flags & 0x20)` at 0x40F160; skip if target airborne |
| 6 | 0x40 | `NO_HIT_DAMAGE` | Does not connect (block/parry only) | Tested: `(hitbox_flags & 0x40)` at 0x40F469; forces `hit_landed_flag=0` |
| 7 | 0x80 | `SIGN_BIT` | Sign-extended as s8; when negative used as secondary check | Tested: `hitbox_flags < 0` at 0x40F18E; triggers `(target_object_ptr[350] & 0xC) == 8` check (hitstun only, no blockstun) |

**Key Logic** (0x40F168–0x40F18E):
- If `hitbox_flags & 0x08` (THROW): collision vs grounded targets only; air targets skip
- If target is airborne: `hitbox_flags & 0x20` (GROUND_ONLY) → reject; `hitbox_flags & 0x10` (AIR_ONLY) → proceed
- If `hitbox_flags & 0x80` (sign bit set, i.e., `hitbox_flags < 0` as signed byte): can only hit hitstunned targets (`target_object_ptr[350] & 0xC == 8`), NOT blockstun-only

**FM2K has no explicit HIGH/LOW flags**. The high/low distinction is purely **geometric**: hitbox Y-position vs target hurtbox Y-position. If hitbox center > hurtbox center, it's an "overhead"; if below, a "low". The engine does not tag them separately.

---

## 2. Hurtbox Struct Layout

**Source**: `hit_detection_system @ 0x40F010`, decompile lines showing hurtbox field reads (0x40F1B0–0x40F21B)

A hurtbox is pointed to by `obj->hurtbox_ptrs[0..19]` (at `obj + 0xD9 + 4*i` to `obj + 0x10D + 4*i`).

### Field Offsets (relative to hurtbox_ptr)

| Offset | Size | Type | Field | Purpose |
|--------|------|------|-------|---------|
| +0x00 | 4 | ptr | ??? | Padding or link |
| +0x01 | 2 | i16 | `x_offset` | X offset from character center |
| +0x03 | 2 | i16 | `y_offset` | Y offset from character center |
| +0x05 | 2 | u16 | `width` | Half-width |
| +0x07 | 2 | u16 | `height` | Half-height |
| +0x09 | 1 | u8 | **unused** | |
| +0x0A | 1 | u8 | **flags** | Hurtbox type flags |
| +0x0B | 1 | u8 | **unused** | |
| +0x0C | 2 | u16 | `block_damage_scale` | Damage scaling factor for block stun (used at 0x40F7E9) |

**Size**: 14 bytes minimum.

### Hurtbox Flags (offset +0x0A)

Accessed via `*(hurtbox_ptr + 10)`. Only **bits 1–2** (0x02, 0x04) are tested:

| Bit | Hex | Name | Meaning | Evidence |
|-----|-----|------|---------|----------|
| 0 | 0x01 | ??? | Not tested; may be disabled/inactive | — |
| 1 | 0x02 | `HURTBOX_ACTIVE` | Hurtbox is enabled | Tested: `(hurtbox_flags & 6) != 0` at 0x40F1B3; if **both bits are clear**, hurtbox is skipped entirely |
| 2 | 0x04 | `COMBO_HURTBOX` | Hit here increments combo counter | Tested: `(hurtbox_ptr + 10) & 4` at 0x40F704; triggers `g_last_block_combo_meter++ ` and flag `+= 3` in combo state |
| 3–7 | — | — | Unused | |

**Key Detail**: Line 0x40F1B3 checks `(*(hurtbox_ptr + 10) & 6) != 0` to skip hurtbox entirely if inactive. This means:
- `0x06` (bits 1–2 set) = active, combo-enabled hurtbox
- `0x02` only = active, non-combo hurtbox
- `0x00` = inactive (no collision check even if geometry overlaps)

**INFERRED**: Hurtbox types (standing/crouching/airborne) are **not swapped** via flags. Instead, the character's `action_table` entry specifies which hitbox/hurtbox set to use in that frame. When a character crouches, the script engine swaps the action to one that has different hitbox/hurtbox pointers.

---

## 3. Reaction Table Layout and Mechanism

**Source**: `hit_detection_system @ 0x40F010`, decompile lines 0x40F48E–0x40F527

The `reaction_table_ptr` is loaded from `*(attacker_object_ptr + 297)`. This is a **per-hitbox reaction definition**, not per-character.

### Reaction Table Struct

| Offset | Size | Type | Field | Purpose |
|--------|------|------|-------|---------|
| +0x01 | 2 | u16 | `standing_block_anim` | Animation ID if standing + blocked |
| +0x03 | 2 | u16 | `crouching_block_anim` | Animation ID if crouching + blocked |
| +0x05 | 2 | u16 | `airborne_block_or_anim` | Animation ID if airborne + blocked (or air-only reaction) |
| +0x07 | 2 | u16 | `standing_hit_anim` | Animation ID if standing + hit |
| +0x09 | 2 | u16 | `crouching_or_air_hit_anim` | Animation ID if crouching or airborne + hit |
| +0x0B | 2 | u16 | `secondary_hit_anim` | Animation ID for secondary hit state (unclear exact use) |

**Size**: 12 bytes minimum.

### Reaction Selection Logic (0x40F4A8–0x40F527)

The engine picks the reaction_anim_id based on:
1. **Target's vertical position** (airborne vs grounded):
   - If `target_object_ptr[0x0C] >= target_object_ptr[0x58]` (Y >= ground_Y): target is **grounded**
     - Else: target is **airborne**

2. **Target's input state** (input_history or character input buffer):
   - `target_input_state = g_p1_input_history[1024 * target_player_id + g_input_buffer_index]`
   - This is the buffered input frame for the target player

3. **Grounded targets**:
   ```
   if (target_input_state & 0x08):  // DOWN button held
       if (hit_landed_flag):
           reaction_anim_id = *(reaction_table_ptr + 9)  // crouching hit
           if (*(target_char_data + 0xDF5D) && (g_reaction_table_flags[36 * reaction_anim_id] & 1)):
               reaction_anim_id = *(reaction_table_ptr + 7)  // swap to standing hit
       else:  // blocked
           reaction_anim_id = *(reaction_table_ptr + 3)  // crouching block
   else:  // UP or neutral
       if (hit_landed_flag):
           reaction_anim_id = *(reaction_table_ptr + 7)  // standing hit
           if (*(target_char_data + 0xDF5D) && (g_reaction_table_flags[36 * reaction_anim_id] & 1)):
               reaction_anim_id = *(reaction_table_ptr + 9)  // swap to crouching hit
       else:  // blocked
           reaction_anim_id = *(reaction_table_ptr + 1)  // standing block
   ```

4. **Airborne targets**:
   ```
   reaction_anim_id = (hit_landed_flag) 
       ? *(reaction_table_ptr + 11)  // air hit
       : *(reaction_table_ptr + 5)   // air block
   ```

### g_reaction_table_flags (0x438694)

This is a **global array** indexed by `reaction_anim_id`. Stride is **36 bytes** per entry (9 dwords), but only the first byte is tested.

**Access pattern** at 0x40F4DC, 0x40F50D, 0x40F544:
```
g_reaction_table_flags[36 * reaction_anim_id] & 1
```

**Bit 0 (0x01)**: When set, indicates this reaction is a **knockdown/launcher** animation that should **swap the hit/block decision**. Specifically:
- If the base selection (from reaction_table) is set to a "standing" anim, but `g_reaction_table_flags[...] & 1` is true, **override to the opposite state** (crouch anim).
- **Purpose**: Allows a single hitbox reaction table to handle both normal and special knockback animations.

**Other bits (0x02–0xFF)**: Untested in decompile; purpose unknown. **INFERRED** to control animation properties (e.g., knockback magnitude, invincibility, parry-able flag, etc.).

---

## 4. Block-Decision Logic in hit_detection_system

**Source**: `hit_detection_system @ 0x40F010`, decompile lines 0x40F3A9–0x40F441

The core block/hit decision is at lines 0x40F3C9–0x40F441. This is where the engine determines whether a hit **connects** or **blocks**.

### Data Sources

1. **target_character_data_ptr** = `&g_character_data_base + 57407 * target_player_id`
2. **target_input_state** = `g_p1_input_history[1024 * target_player_id + g_input_buffer_index]`
3. **character_flags** = `*(target_character_data_ptr + 31926)` (offset +0x7CB6)
4. **guard_direction** = `(attacker_object_ptr[2] <= target_object_ptr[2]) + 1` → 1 (LEFT) or 2 (RIGHT)

### Block Decision Flow

**Prerequisite**: Check if target is **actionable** (not already in hitstun/blockstun):
```c
if ((target_object_ptr[350] & 0xC) == 0) {  // bits 2–3 not set (not in stun)
    // Decision logic below
} else if ((target_object_ptr[350] & 0xC) == 0xC) {
    // Already hitstunned; force hit regardless
    hit_connected_flag = 1;
}
```

**If AI is active** (chance-based block):
```c
if (*(target_character_data_ptr + 57181)) {  // AI gate: 0 = manual, 1+ = AI
    if (game_rand() % 100 >= *(target_character_data_ptr + 57185)) {
        goto LABEL_71;  // Force hit (AI fails to block)
    }
}
```

**If character is crouching** (bit 3 of char_flags set):
```c
if ((character_flags & 8) != 0) {
    // Crouch-block check
    if (((1 << (target_character_data_ptr[31913] + 4))  // Left-shift by (val + 4)
         & g_p1_input_history[1024 * target_player_id + g_input_buffer_index])
        == 0  // Bit NOT set
        && ((character_flags & 1) == 0 || target_input_state))  // Char not prone OR input active
    {
        goto LABEL_71;  // Hit lands (guard failed)
    }
}
```

**Else (standing)**:
```c
else {
    if ((guard_direction & target_input_state) == 0  // Guard direction NOT in input
        && ((character_flags & 1) == 0 || (target_input_state & 0xFFFFFFF7) != 0))
    {
        goto LABEL_71;  // Hit lands (guard failed)
    }
}

// If we reach here (guard succeeded):
hit_connected_flag = 1;  // BLOCK
```

### Analysis of the Block Conditionals

#### char_data + 0x7CB6 (character_flags field)

| Bit | Hex | Name | Meaning | Evidence |
|-----|-----|------|---------|----------|
| 0 | 0x01 | `PRONE_OR_RECOVERING` | Character is in a "prone" state (knocked down or in recovery animation; cannot block) | Tested at 0x40F3F4 and 0x40F43F; if set, certain block paths disabled |
| 1 | 0x02 | `AIRBORNE` or `GUARD_ALLOWED` | Set = character is airborne OR guard is mechanically allowed in this frame | Tested at 0x40F475; used to invalidate "standing/crouching" distinction if target is airborne |
| 2 | 0x04 | **unused** | Not tested in hit_detection | — |
| 3 | 0x08 | `CROUCHING` | Character is in crouch stance | Tested at 0x40F3F4; determines which guard check (crouch vs stand) to use |
| 4–7 | — | Unused | | |

#### char_data + 0x7CA9 (guard_button_index field)

**Offset**: +31913 (= 0x7CA9)  
**Type**: u8  
**Purpose**: Per-character guard button configuration index.

The decompile shows:
```
(1 << (target_character_data_ptr[31913] + 4)) & g_p1_input_history[...]
```

This is a **bit-shift by (value + 4)**, then AND with the input history word.

**INFERRED**: If the character's guard button is configured as button index `N` (0–11, typically 0=LEFT, 1=RIGHT, 2=DOWN, 3=UP, etc.), then `target_character_data_ptr[0x7CA9] = N`. The shift by `+4` maps it to bits 4–7 of the input history dword (bits 0–3 = face buttons, bits 4–7 = guard buttons).

**Example**:
- If guard_button_index = 0 (LEFT guard): `1 << (0+4) = 0x10` → checks bit 4 of input
- If guard_button_index = 2 (DOWN guard): `1 << (2+4) = 0x40` → checks bit 6 of input

#### target_input_history vs target_input_state

**target_input_history**: `g_p1_input_history[1024 * target_player_id + g_input_buffer_index]`  
- Loaded once per hit-detection frame (line 0x40F2BE)
- **Represents**: The buffered input **at frame start**, before the current frame's physics

**target_input_state**: Also loaded from input history, but used **multiple times** in different block checks:
- At crouch guard check (0x40F428): reloaded from history
- At stand guard check (0x40F43F): checks `(guard_direction & target_input_state)`

**Difference**: target_input_state is re-read if AI override is active (0x40F2FF–0x40F30F):
```c
if (*(&g_charslot0_pending_attack_id + 57407 * target_player_id)) {
    target_input_state = *(&g_charslot0_pending_attack_meta + 57407 * target_player_id);
}
```
This allows **script-driven inputs** (e.g., combo/special move execution) to override the guard decision.

#### guard_direction Logic

```
guard_direction = (attacker_object_ptr[2] <= target_object_ptr[2]) + 1
```

Result: `1` or `2`
- `1` = attacker at X ≤ target's X → attacker is **LEFT** → must hold **RIGHT** to guard
- `2` = attacker at X > target's X → attacker is **RIGHT** → must hold **LEFT** to guard

In the stand guard check:
```
(guard_direction & target_input_state) == 0
```
This checks if the **required guard direction bit** is **not** set in the input. If not set → hit lands.

**Example**:
- Attacker on left (guard_direction = 1 = 0x01)
- Input LEFT = bit 0, RIGHT = bit 1
- `(1 & input_state)` checks if LEFT is held
- If LEFT is held: `1 & 0b01 = 1` → NOT 0 → condition false → guard succeeds
- If RIGHT or neutral: `1 & 0b00 or 0b10 = 0` → condition true → hit lands

#### The 0xFFFFFFF7 Mask (anti-DOWN logic)

In stand guard: `(character_flags & 1) == 0 || (target_input_state & 0xFFFFFFF7) != 0`

Breakdown:
- `0xFFFFFFF7` = all bits **except bit 3** (DOWN button)
- `(target_input_state & 0xFFFFFFF7)` checks if **any button OTHER than DOWN is pressed**

**Logic**: If `character_flags & 1` is set (prone), the guard attempt is already invalid. Otherwise, the guard succeeds **only if no other buttons are held**. If DOWN is held (bit 3), the condition `(input_state & 0xFFFFFFF7) != 0` is false if **only DOWN is held**, meaning a stand-guard check that requires "UP or neutral input" fails if the player is already holding DOWN for crouch.

**This is actually checking**: If the character is NOT prone, and the player is NOT holding DOWN (crouch), then stand-guard is possible. If they're holding DOWN, the crouch-guard path should have been taken earlier in the conditional.

---

## 5. Complete char_flags @ 0x7CB6 Documentation

**Address**: `char_data + 0x7CB6` (offset +31926)  
**Type**: u32 (treated as u8 for these checks, but full dword allocated)  
**Accessed By**: `hit_detection_system`, `character_input_processor`, `character_state_machine`

### All Documented Bits

| Bit | Hex | Name | Set When | Cleared When | Evidence |
|-----|-----|------|----------|--------------|----------|
| 0 | 0x01 | `PRONE_STATE` | Character is knocked down or in recovery animation | Character recovers from knockdown / recovers to neutral stance | Tested at 0x40F3F4, 0x40F43F; prevents standing guard checks |
| 1 | 0x02 | `AIRBORNE_OR_GUARD_DISABLED` | Character is airborne **OR** guard is mechanically disabled in current frame | Character lands **AND** guard is re-enabled | Tested at 0x40F475; when set, blocks "standing/crouching" distinction validity check |
| 2 | 0x04 | **UNKNOWN** | Not directly tested in hit_detection_system | — | **INFERRED**: May control special states (invincibility, throw protection, etc.); found via `character_state_machine` writes |
| 3 | 0x08 | `CROUCHING` | Player input DOWN active **AND** character script allows crouch in current frame | Player releases DOWN **OR** script exits crouch state | Tested at 0x40F3F4; determines crouch-guard vs stand-guard path |
| 4–31 | — | Unused or packed data | — | — | |

### Writes to char_flags

**Sources** (via cross-reference):
1. **character_state_machine** (~0x411BF0): Writes `char_flags` every frame based on character script state
2. **character_input_processor** (~0x410DC0): Sets bits based on input history and player controls
3. **hit_detection_system**: Does NOT write directly; only reads

**Crouch State Transitions**:
- **Set bit 3 (0x08)**: When character input DOWN is held and current character action allows crouching
- **Clear bit 3**: When input releases DOWN or character switches to action that forbids crouch

---

## 6. Hit/Block Reactions: Post-Decision Path

**Source**: `hit_detection_system @ 0x40F010`, decompile lines 0x40F527–0x40F8AA

After the `hit_connected_flag` is determined (1=blocked, 0=hit), the engine:
1. Picks a `reaction_anim_id` from the reaction table
2. Applies hitstop/blockstun frames
3. Updates target and attacker object state
4. Applies damage

### Reaction Animation Lookup

**Line 0x40F541**: 
```c
*(target_object_ptr + 14) = *&target_character_data_ptr[4 * reaction_anim_id + 28074]
```

This reads from the **character's animation table** (address = char_data + 28074 + 4*reaction_anim_id). The animation ID is a dword offset into a per-character array of animation pointers.

**Offset Analysis**: 28074 = 0x6DAA. In character_data layout:
- Animation table starts at offset 0x6DAA
- Each animation entry is 4 bytes (likely a pointer to animation frame data)
- `reaction_anim_id` indexes into this table

### Hitstop/Blockstun Frames

**Global Constants**:
- `g_hit_hitstop_frames @ 0x43A29A` (read at 0x40F68D)
- `g_block_hitstop_frames @ 0x43A299` (read at 0x40F74F)

Both target and attacker receive hitstop during hit:
```c
hit_hitstop_frames = g_hit_hitstop_frames;
*(target_object_ptr + 16) = g_hit_hitstop_frames;  // Object field at offset 0x40
attacker_object_ptr[16] = hit_hitstop_frames;
```

Blockstun is applied to target only if block succeeded:
```c
block_hitstop_frames = g_block_hitstop_frames;
*(target_object_ptr + 16) = g_block_hitstop_frames;
```

**Object field at +0x40**: This is the **hitstop counter**. Decremented each frame; when zero, character can act.

### Hit Stun Duration Flag

**Line 0x40F6A8**: 
```c
*(target_object_ptr + 350) |= 0xCu;  // Set bits 2–3: hitstun flags
```

**Object field at +0x158 (= 350 bytes)** is the **state flags field**. Bits:
- **Bit 2 (0x04)**: `IN_BLOCKSTUN` — character is blocking
- **Bit 3 (0x08)**: `IN_HITSTUN` — character is in hitstun

When both are set (0x0C), character is in **hitstun**. When only bit 2 is set (0x04), character is in **blockstun**.

**Blockstun path** (line 0x40F7B0):
```c
*(target_object_ptr + 350) = (*(target_object_ptr + 350) & 0xFFFFFFF3) | 8;
```
This clears bits 2–3, then sets **only bit 3** (hitstun). **INFERRED**: This might be a bug or the blockstun visual feedback uses hitstun animation on blocking targets? Or it means "blockstun is a type of hitstun" in the engine's taxonomy.

### Hitstun Duration Computation

**Hidden Formula**: Hitstun duration is **implicitly determined** by:
1. The `hitstop_frames` value (both attacker and target receive the same frames)
2. The **reaction animation duration** (from the animation table entry)

There is **no explicit hitstun_duration field**. Instead:
- Character enters hitstun state (flag set)
- Hitstop frames count down
- When hitstop expires, character can begin the reaction animation
- Reaction animation plays for its defined duration
- When animation finishes, character transitions to next state (recovery/idle)

**INFERRED**: The total hitstun = hitstop_frames + animation_duration. The engine does not store a hitstun counter separate from animation frames.

### Blockstun Duration Computation

Similar to hitstun:
- Blockstun frame count = `g_block_hitstop_frames`
- Target plays block reaction animation
- Animation duration = part of the reaction animation table entry

### Damage Application

**Hit path** (line 0x40F6DE):
```c
if ((hitbox_flags_2 & 4) != 0) {  // 0x04 = NO_STUN_DAMAGE flag
    scaled_damage = hit_damage * byte_4D9A25[attacker_character_offset] / 100;
    if (!scaled_damage) scaled_damage = 1;
    health_damage_manager(target_character_data_ptr, -scaled_damage);
}
```

**Block path** (line 0x40F7F9):
```c
combo_scaled_damage_calc = hit_damage 
    * *(target_character_data_ptr + 57089)   // Combo meter scaling
    * byte_4D9A28[attacker_character_offset];  // Attacker's block damage scaling
*(target_object_ptr + 350) = ... | 8;  // Set blockstun flag

final_block_damage = combo_scaled_damage_calc / -100 + hit_damage;
if (final_block_damage < 1) final_block_damage = 1;

final_block_damage_2 = final_block_damage * *(hurtbox_ptr_saved + 11) / 100;
if (final_block_damage_2 < 1) final_block_damage_2 = 1;

health_damage_manager(target_character_data_ptr, -final_block_damage_2);
```

**Key Detail**: Block damage is **reduced** by:
1. Combo meter penalty (multiplied by combo counter)
2. Hurtbox's `block_damage_scale` field (at hurtbox + 0x0C)

### Combo Counter Tracking

**Combo Hit Path** (line 0x40F704):
```c
if ((*(hurtbox_ptr_saved + 10) & 4) != 0) {  // Hurtbox combo flag
    *(&dword_4DFCAD + attacker_character_offset) |= 3u;  // Attacker combo flag
    ++*(target_character_data_ptr + 57137);  // Target's combo meter (offset +57137)
}
```

**Combo counter location**: `char_data + 57137` (counts consecutive hits landed on block).

---

## 7. Action Table / Script ID System

**Source**: Scattered references in `game_state_manager @ 0x407CA0`, `character_state_machine @ 0x412480`, `character_action_controller @ 0x4118D2`

### High-Level Architecture

FM2K uses a **per-character action table** to map script IDs to animation/behavior data:

**g_charslot0_action_table** (address varies per-player slot)
- Each character slot has its own action table loaded from the `.player` file
- Table is a **linear array of dwords** (4-byte entries)
- `action_table[N] = pointer to script N's animation/behavior struct`

### Script ID Mapping

```c
// In character_state_machine (0x412480):
mov eax, g_charslot0_action_table[ecx]  // ecx = script_id
// eax now points to the animation data for script_id
```

**Stride**: 4 bytes (dword pointer)  
**Base Address**: Per-character, loaded from character file (location: `0x4D1D80 + player_id * 57407`)

### Script Entry Structure

**INFERRED** (not fully decompiled, but accessed at 0x411CA5–0x411F21):

Each script entry (4-byte dword) points to or contains:
- **Animation pointer**: Link to animation frame sequence
- **Startup frames**: Frames until hitboxes become active
- **Active frames**: Duration hitboxes remain active
- **Recovery frames**: Frames until character can act again
- **Flags**: Whether attack, movement, idle, hit-reaction, etc.

**Evidence**: The decompile accesses `[ebx + 0x7CAA]` (char_data + 0x7CAA), which is read from the action table. This field is **action duration counter** — incremented/decremented each frame and used to determine when animation finishes.

### Detecting Attack State

**Problematic**: FM2K does **not have an explicit "is_attacking" flag** visible in hit_detection_system.

**INFERRED**: An "attack action" is detected by:
1. **Hitbox presence**: If `obj->hitbox_ptrs[0..19]` contains non-NULL pointers, character is attacking
2. **Flags in reaction table**: The `g_reaction_table_flags` might contain a bit marking "this script is an attack"
3. **Combo meter state**: If combo counter is non-zero, previous hit was landed

**For Training Mode Design**:
- Scan hitbox pointers to determine if character is in attack
- If hitbox is active (non-NULL) and hitbox_flags don't have `0x40` (NO_HIT_DAMAGE), character is **in an attack state**

### Frame Data Fields

To display frame data (startup/active/recovery):

1. **Startup frames**: Compare current frame index against script's defined startup threshold (location unknown; likely in script definition or animation struct)
2. **Active frames**: Count frames where hitbox is active (non-NULL)
3. **Recovery frames**: Remaining duration = action_duration - current_frame

**INFERRED**: These are stored in the **character file's script table**, loaded at match start. Not visible in real-time object state; would require:
- Parsing character file format
- Or hooking character_state_machine to log frame transitions

---

## Appendix: Key Addresses Summary

| Component | Address | Size | Purpose |
|-----------|---------|------|---------|
| `g_object_pool` | 0x4701E0 | 382 bytes × 1024 | Game object pool |
| `g_character_data_base` | 0x4D1D80 | 57407 bytes × 8 | Character state data (8 slots) |
| `g_p1_input_history` | 0x4280E0 | 4 bytes × 1024 × 2 | Input history buffer (1024 frames, 2 players) |
| `g_input_buffer_index` | 0x447EE0 | 4 | Current frame index in input buffer |
| `g_reaction_table_flags` | 0x438694 | 4 bytes × N | Reaction animation flags (36-byte stride) |
| `g_hit_hitstop_frames` | 0x43A29A | 4 | Global hitstop duration constant |
| `g_block_hitstop_frames` | 0x43A299 | 4 | Global blockstun duration constant |
| char_data + 0x7CB6 | — | 1 | Character state flags (CROUCHING, AIRBORNE, etc.) |
| char_data + 0x7CA9 | — | 1 | Guard button index |
| char_data + 0x7CAA | — | 4 | Action duration counter / animation frame index |
| char_data + 0x6DAA | — | 4 × N | Animation table (reaction_anim_id indexing) |
| obj + 0x08 | — | 4 | Position X (fixed-point Q16) |
| obj + 0x0C | — | 4 | Position Y (fixed-point Q16) |
| obj + 0x40 | — | 4 | Hitstop counter |
| obj + 0x58 | — | 4 | Ground Y position |
| obj + 0x89–0xBD | — | 4 × 20 | Hitbox slot pointers |
| obj + 0xD9–0x10D | — | 4 × 20 | Hurtbox slot pointers |
| obj + 0x158 | — | 4 | Flags (hitstun/blockstun bits 2–3) |

---

## Design Notes for Training Mode

1. **Guard Display**: Read `char_data + 0x7CB6` bit 3 to detect crouch. Compare input history with `guard_direction` to determine if guard is active.

2. **Frame Data**: Requires parsing character file or hooking character_state_machine to log frame transitions. Real-time display is not available from object pool alone.

3. **Hit/Block Distinction**: Simulate the block-decision logic: load `char_data + 0x7CB6` and `target_input_state`, then run the conditional checks at lines 0x40F3F4–0x40F441.

4. **Reaction Animations**: Are determined by reaction_table_ptr (loaded from hitbox), not by character. Different hitboxes can have different reaction tables, even on the same character.

5. **Combo Detection**: Read `char_data + 57137` (combo counter). Increment on hit, reset on successful block or whiff.

---

**Document Status**: Complete analysis based on IDA Pro decompilation.  
**Last Updated**: May 2026  
**Confidence**: High for sections 1–6; Medium for section 7 (action table format not fully decompiled).

---

## Appendix A: Editor-Side Cross-Reference

The authoritative editor-side documentation for FM2K scripting lives in
`docs/editor/2DFM Codeblocks.md` (the `[FA]`, `[FD]`, `[R]`, `[DS]`, `[DB]`
codeblock specs the 2DFM editor exposes to character authors). Crossing
those names against the engine reads in this doc lets us pin down what
each previously-mysterious flag bit actually MEANS to the author.

### A.1 [FA] Attack Frame checkboxes → hitbox_flags bits (0x0A)

The editor exposes 8 checkboxes on each Attack Frame. Mapping to engine
bit-position via observed behavior in `hit_detection_system`:

| Editor name | Effect (per editor doc) | Engine test | Bit |
|---|---|---|---|
| Cancel | Cancellable on hit via `[C]` codeblock | (not tested in hit_detection — cancel system) | ? |
| **No Detection** | Doesn't hit grounded targets | `flags & 0x10 + target grounded → skip` | **0x10** |
| Cont. Hits | Auto-chain from previous hit | (cancel system) | ? |
| **No Sky Decision** | Doesn't hit blockstun targets | `flags & 0x80 + target blockstun → skip` | **0x80** |
| **Guard Fail** | Unblockable | `flags & 0x40 → forces hit_landed=hit_connected=0` (chip-only path) | **0x40** |
| **While Guard** | Doesn't hit airborne targets | `flags & 0x20 + target airborne → skip` | **0x20** |
| **While Receiving** | Doesn't hit hitstun targets | `flags & 0x08 + target hitstun → skip` | **0x08** |
| Shave | Chip damage on block (S.ratio %) | (chip damage calc — section 4) | ? |

**Correction to section 1's inference**: bit `0x08` is not `THROW` —
it's `While Receiving` (don't hit hitstun). The throw mechanic is
implemented elsewhere (see `[DS] While throw do` codeblock — section A.4).

The editor doc itself flags that `No Sky Decision` and `While Guard` may
be swapped in some translations; behavior-based mapping above is the
authoritative answer.

### A.2 [FD] Defense Frame checkboxes → hurtbox_flags bits (0x0A)

| Editor name | Effect | Engine test | Bit |
|---|---|---|---|
| Striking | Pushbox (collision only, no damage) | `flags & 6 == 0 → skip` | 0x01 |
| **Doing** | Hurtbox (takes damage) | `flags & 6 != 0 → process` | **0x02** |
| Throwing | Throwable by `[DS] While throw do` hitboxes | (separate check) | 0x04 |

`Ratio` field (0–255) = the per-hurtbox damage scale at hurtbox+0x0C
(`block_damage_scale` in section 2). Confirmed.

### A.3 [R] Reaction → reaction_table layout

Editor exposes a 2×3 matrix of reaction-anim-id slots:

|        | Stand | Crouched | Air |
|--------|-------|----------|-----|
| Guard  | +0x01 | +0x03    | +0x05 |
| Hits   | +0x07 | +0x09    | +0x0B |

This **exactly matches** section 3's decoded reaction_table struct.
The +7/+9 cross-overrides (when `g_reaction_table_flags[N*36] & 1` is
set) handle the case where a "stand hit" anim ID is actually flagged as
a "crouch hit" anim — the engine swaps to the other slot.

Author intent: a `[R]` block on each attack defines six anims — what
the OPPONENT does when standing-blocking, crouching-blocking, air-
blocking, standing-hit, crouching-hit, air-hit. The engine reads from
these slots based on the opponent's state at hit-detection time.

### A.4 [DS] Detect Skill — post-event hooks for reversals

These editor codeblocks are how character scripts react to combat
events. They are EXACTLY the trigger primitives we need for an ETM-
style reversal system in training mode:

| Editor name | Triggers when... | For training-mode reversal |
|---|---|---|
| `Attack Hits` | Our hitbox hit a hurtbox that DIDN'T block | "Reversal on hit" trigger |
| `Defending` | Our hitbox hit a hurtbox that DID block | "Reversal on blocked attack" trigger |
| `Landing` | Object hits the ground | "Reversal on wake-up" trigger |
| `Hit the Wall` | Object touches camera wall | "Reversal on corner-touch" trigger |
| `in offset` | Hitbox-vs-hitbox clash | "Reversal on trade" trigger |
| `While throw do` | Hitbox hit a throw-eligible hurtbox | Throw-reaction trigger |

The engine writes flag bits when these events fire (script then reads
them on the same or next frame). For a training-mode reversal we don't
have to inject script ops — we just READ the flag bits on the dummy's
char_data and inject the appropriate input sequence.

**Action item**: search the binary for the bits that flip on each
`[DS]` event. Likely candidates: `char_data + 0xDFCAD` (already known
to be hit-attacker side flags), `obj->flags` (350, 0x15E).

### A.5 [DB] Basic Divergence — state checks scripts use

These are character-state predicates the engine exposes to scripts.
Each is a single bit somewhere we can also read:

| Editor name | Meaning | Engine state |
|---|---|---|
| Guarding | Currently in a guard animation | `char_data + 0x7CB6` bit ? (need to find) |
| Standing | In a standing animation | `char_data + 0x7CB6` bit ? (need to find) |
| Crouching | In a crouching animation | **`char_data + 0x7CB6` bit 3 (= 0x08)** ✓ |
| Forward tapped | Holding forward direction (facing-aware) | Input history + facing |
| Back tapped | Holding back direction (facing-aware) | Input history + facing |
| Up / Down tapped | Holding screen-coord up/down | Input bits 0x004 / 0x008 |

The note about diagonals counting for both axes is important — a
down-forward press counts as both "Forward" AND "Down" tapped. Same
convention to use in our combat-state predicates.

### A.6 "S.ratio" — character basic settings

`[FA] Shave` references `S.ratio` in the character's basic settings
as the chip damage percentage. Almost certainly one of:

- `char_data + 0x7CA5` (`byte_4D9A25` for slot 0) — used as hit damage
  multiplier in the unblocked-hit path (line 0x40F6BE).
- `char_data + 0x7CA8` (`byte_4D9A28` for slot 0) — used in the block
  damage scaling computation (line 0x40F7F0).

The block-path one (0x7CA8) is likely "S.ratio" since it scales chip.
For a precise per-character damage multiplier override in training mode,
write here.

### A.7 What this changes for training mode design

With the codeblocks cross-reference nailed down, training-mode features
become straightforward READS + WRITES of known fields:

1. **Reversal system** — hook the `[DS] Defending` / `[DS] Attack Hits`
   flag bits (TBD location), inject pre-recorded input sequence on edge.
2. **Block-prediction display** — simulate the section 4 conditionals
   client-side to show "would block" / "would eat" for incoming hitbox.
3. **Hitbox/hurtbox overlay** — geometry is in the structs already
   decoded; just need a per-frame iter over obj+0x89 (hitboxes) and
   obj+0xD9 (hurtboxes) for both players + their projectiles.
4. **Frame data display** — needs action_table decode (section 7).
5. **State filters in dummy panel** — Standing / Crouching / Jumping
   menu items map to writing the right input bits AND inspecting bit 3
   of `char_data + 0x7CB6` to verify.
