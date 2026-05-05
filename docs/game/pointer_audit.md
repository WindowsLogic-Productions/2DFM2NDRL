# FM2K Rollback-Desync Pointer Audit — `WonderfulWorld_ver_0946.exe`

Scope: every pointer-typed field stored inside the game's runtime state that the rollback save/restore path must understand. Classifications follow the prompt's A–F scheme:

- **A** — self-referential into the object pool / node pool / list heads (rollback state internal). Safe IF the pool is saved & restored IN PLACE (same virtual addresses).
- **B** — node-pool / list-head internal. Same safety criterion as A.
- **C** — into per-player **compiled script data** (character files loaded at match-start; per-player sprite/reaction/box/opcode streams). Safe IFF the script region is NOT freed/reallocated between save and restore.
- **D** — heap (`GlobalAlloc` / `malloc`) buffer that lives outside rollback state. Save-separate required.
- **E** — OS-owned handle (DirectDraw surface, Win32 HANDLE, DSound buffer). NEVER save.
- **F** — stack pointer (bug if it ever happens).

All addresses are WonderfulWorld v0.946 RVAs. Structure strides in the game are **57407 bytes** per `KgtPlayerRuntimeSlot` (vs. editor's 47851) and **382 bytes** per `KgtRuntimeObject`.

---

## 1. Executive summary

The live rollback state is dominated by **four pointer families** that must be treated together:

1. **Object-pool internal pointers** (type A) — the object linked-list, parent-of-spawn pointer, opponent pointers. Safe provided the rollback save region captures the pool(s) byte-for-byte at the same virtual address.
2. **Node-pool linked list** (type B) — 1024 × 8-byte nodes forming 128 priority chains. Must be saved ATOMICALLY with the list heads and list tails; a partial save creates dangling `next_ptr`.
3. **Script-stream pointers** (type C) — 20×2=40 box-slot pointers per object, reaction-table pointers, afterimage command pointers, `g_hitbox_sprite_data_ptrs[]` — all point into per-player compiled script memory. Safe IFF the script region lifetime covers every rollback window. A mid-match character reload (e.g., demo transition, KGT project switch) will invalidate **all 40,960** box pointers across the full pool.
4. **Afterimage pool** (type D-ish) — 100 × 1616-byte entries at `0x447F80..0x46F6C0`. Each entry owns a `command_ptr` (C) and a 1600-byte position/color buffer (referenced by index from objects via `obj+337`). Must be state-saved in full; the command_ptr dangles if scripts were reloaded.

**Highest-risk rollback hazards** (in order of blast radius):

- Failing to save **all 128 `g_object_list_heads` + 128 `g_object_list_tails` + entire `g_object_node_pool`** atomically. Any partial save corrupts iteration in `update_game_state` / `finalize_game_objects` → crash.
- Failing to save `g_current_object_ptr` (points **into** the node pool) and `g_object_data_ptr` (points **into** the object pool). These are live iterators written mid-frame; a rollback snapshot taken during the update loop MUST capture them.
- Failing to save `g_sprite_data_command_ptrs[]` in the afterimage pool. Each 1616-byte slot has a 4-byte pointer into script memory that must be re-valid after restore.
- The **20 hit-box + 20 hurt-box pointers per object** (40 × 1024 = 40,960 pointers) are all set to the current script opcode pointer `edi` in opcodes 0x18/0x19. If the rollback path reloads a character file mid-match (or if you keep the object pool but throw away the script data on a mode transition), you have 40,960 dangling pointers.

---

## 2. Pointer field classification table

Offsets are byte offsets from the struct base (`KgtRuntimeObject = 382 bytes`, `KgtPlayerRuntimeSlot = 57407 bytes`, afterimage entry = 1616 bytes). Writer / reader columns list function RVAs or opcode numbers.

### 2.1 `KgtRuntimeObject` (382 bytes) — pool at `0x4701E0`, stride 382, 1024 slots

| Offset | Field name (proposed) | Type | Writer | Reader | Target | Class | Save requirement |
|---|---|---|---|---|---|---|---|
| +137 (0x89) | `hit_box_ptrs[20]` | `KgtScriptItem*[20]` | opcode 0x18 at `0x412cfa` (`mov [esi+eax*4+0x89], edi`) | `ProcessHitboxCollisions` at `0x40eb88`; `sprite_rendering_engine` debug draw | Per-player compiled script stream | **C** | Script stream must not be reallocated. All 20 entries live/dead controlled by `ClearObjectHitboxData` (`0x40e550`) = `memory_clear(obj+137, 80)`. |
| +217 (0xD9) | `hurt_box_ptrs[20]` | `KgtScriptItem*[20]` | opcode 0x19 at `0x412d63` (`mov [esi+ecx*4+0xD9], edi`) | `physics_collision_handler` attacker @ `0x40fcc9`; `ProcessHitboxCollisions` defender; `hit_detection_system` hurtbox @ `0x40f1a2`; sprite debug draw | Per-player compiled script stream | **C** | As above; also cleared by `ClearObjectHitboxData`. |
| +297 (0x129) | `reaction_table_ptr` | `KgtReactionItem*` | opcode 0x17 at `0x412d95` (`mov [esi+0x129], edi`) | `hit_detection_system` @ `0x40f48e` | Per-player compiled script stream (reaction table block) | **C** | As above. If null → hit_detection_system logs "Reaction Error 2". |
| +378 (0x17A) | `parent_object_ptr` | `KgtRuntimeObject*` | `create_game_object` @ `0x4065c3` (`*(v5+378)=v8`) | `physics_collision_handler` @ `0x40f99b` when flag 0x20000000 set | Another slot in `g_object_pool` | **A** | Save object pool in place. Parent survival not guaranteed (parent may die mid-match and its slot get recycled), so game code guards with `*(+92)&1` flag; rollback just needs identical slot addresses. |
| +334 (0x14E) | `attacker_slot_id` | `byte` | `hit_detection_system` (from attacker's +342) | various | Integer 0..7 (slot index) | not a pointer | save byte |
| +337 (0x151) | `afterimage_slot_idx` | `int` | opcode 0x25 / `character_state_machine` @ `0x413b22` | `sprite_rendering_engine` @ `0x40cd3a` (`v9 = 1616 * obj[+337]`) | INDEX into afterimage pool (not a pointer) | not a pointer | save dword; afterimage pool saved separately |
| +342 (0x156) | `player_slot_id` | `byte` | `create_game_object` callers / script | all per-player data look-ups (`57407 * obj[+342]`) | Integer 0..7 (player-slot index) | not a pointer | save |
| +346 (0x15A) | `object_role` | `byte` | creators | everywhere | Integer (0=character,1=effect,2=projectile,3=spawn,4=special,5=inactive) | not a pointer | save |

**Offsets the audit did NOT find pointers at (verified non-pointer dwords / integers / bitfields):** +0 (state), +4..+88 (phys/vel/screen), +48 (attack state id), +56 (anim idx), +72/76 (color), +84 (palette idx), +92 (facing flags), +124 (bufferred anim), +152 (script node index), +156 (player slot alias), +158-160 (flags), +350 (state flags), +376 (spare?).

### 2.2 `KgtPlayerRuntimeSlot` (stride 57407) — array base at `g_character_data_base @ 0x4D1D80`

Only pointer fields are enumerated here; the slot also contains dozens of scalar fields (HP, input mirror, char config blocks) that are non-pointers.

| Offset | Global alias | Type | Writer | Reader | Target | Class | Save requirement |
|---|---|---|---|---|---|---|---|
| +272 (0x110) | (char file header pointer) | `void*` | `player_data_file_loader` @ `0x403a17` | `character_state_machine`, `SetCharacterAttackState` @ `0x410d37` (`[ebx+110h]`), opcode tables | Per-player compiled character file (`KgtPlayerFileBlocks`) buffer | **C / D** | Buffer lifetime spans match; saved once at match-start. Rollback must NOT free/realloc. |
| +31914 (0x7CAA) | `char_value_max` alias base | `dword` | char loader | state machine | non-pointer integer block | — | — (listed for context) |
| +57081 (0xDEF9) | `g_last_hit_target_ptr[slot]` (alias `0x4DFC79`) | `KgtRuntimeObject*` | `hit_detection_system` @ `0x40f2da`, `0x40f2e0` | `physics_collision_handler` @ `0x40fa33`; `finalize_game_objects` | Object in `g_object_pool` | **A** | Pool saved in place. Cleared by `finalize_game_objects` on frame end in some paths. |
| +57085 (0xDEFD) | `g_p1_opponent_ptr[slot]` (alias `0x4DFC7D`) | `KgtRuntimeObject*` | `hit_detection_system` @ `0x40f8a5`, `0x40f8b9` | `physics_collision_handler` @ `0x40fae6`, `0x40fbc6`, `0x40fc4a` (stage clamps) | Object in `g_object_pool` | **A** | Pool saved in place. |
| +57193 (0xDF69) | `g_nearest_opponent_ptr[slot]` (alias `0x4DFCE9`) | `KgtRuntimeObject*` | `character_facing_controller` @ `0x40e672` (recomputed every frame); cleared @ `0x40e5ed` at frame start | `ai_behavior_processor` @ `0x4107f2` | Object in `g_object_pool` | **A** | Technically derivable (recomputed each frame), but AI / facing logic reads it mid-frame so rollback MUST snapshot it. Pool saved in place. |
| +57279 (0xDFBF) | `spawn_slot_ptrs[10]` | `KgtRuntimeObject*[10]` | `spawn/create` opcode in `character_state_machine`; memset by `DeactivateCharacterProjectiles` @ `0x40656c` | `ClearObjectReferences` @ `0x40e4a0` | Objects in `g_object_pool` | **A** | 40 bytes (10 × 4). Pool saved in place. Cleanup scans these and nulls any matching dying object. |

Per-player scratch pointer arrays at `g_hitbox_sprite_data_ptrs @ 0x4D1E94` and `g_character_sprite_data` etc. are **aliases into** this same slot region — each is a byte/dword offset into `g_character_data_base + 57407*slot`. They point to per-player compiled sprite/box metadata blocks inside the character file (type **C**).

### 2.3 Node pool and list heads — `0x4CFA20` and `0x430240`

| Field | Location | Type | Writer | Reader | Target | Class | Save requirement |
|---|---|---|---|---|---|---|---|
| Node `[i].obj_ptr` | `g_object_node_pool + 8*i + 0` | `KgtRuntimeObject*` | `update_game_state` @ `0x404d2a` | every per-object-update path | Object in `g_object_pool` | **A** | Saved with node pool. |
| Node `[i].next_ptr` | `g_object_node_pool + 8*i + 4` | `_node*` | `update_game_state` @ `0x404d27` (sets `prev->next`), `0x404d32` (zeros terminator) | linked-list walk in `update_game_state` @ `0x404d79` and `render_game` | Another entry in `g_object_node_pool` | **B** | Saved with node pool. |
| `g_object_list_heads[k]` | `0x430240 + 8*k` (k=0..127) | `_node*` | `update_game_state` @ `0x404d1f` | iterator @ `0x404d4c` | First node in priority chain k | **B** | **Must be saved atomically with tails + node pool.** |
| `g_object_list_tails[k]` | `0x430244 + 8*k` (k=0..127) | `_node*` | `update_game_state` @ `0x404d2c` | chain-append in-place | Last node in chain k | **B** | Same as heads. |
| `g_current_object_ptr` | `0x4259A8` | `_node*` | `update_game_state` @ `0x404cfb`, `0x404d47`, `0x404d5a` | `render_game` @ `0x404e01` | Current node during iteration | **B** | Live iterator, MUST save during rollback window. |
| `g_object_data_ptr` | `0x4CFA00` | `KgtRuntimeObject*` | `update_game_state` @ `0x404d41`, `0x404d5f` | EVERY per-object handler (state machine, physics, hit detection, ai) | Current object | **A** | Live iterator, MUST save. Almost every game function reads it. |
| `g_max_objects` | `0x4259A4` | `int` (constant during match) | `update_game_state` @ `0x404d05` | physics / hit code | integer = 1024 | — | constant |

### 2.4 Afterimage pool — `0x447F80 .. 0x46F6C0` (100 entries × 1616 bytes)

The first 16 bytes of each 1616-byte slot hold the header (active flag, frame counter, command pointer, state flags), followed by 1600 bytes of position/color payload. The four named field-globals are **offset aliases within each slot**; each "global" is actually slot 0 of the array for that field. Stride = 1616 bytes = 404 DWORDs.

| Field (within slot) | Offset | Type | Writer | Reader | Target | Class | Save requirement |
|---|---|---|---|---|---|---|---|
| `active_flag` | +0 | `dword` | `ClearObjectReferences` @ `0x40e4fb`; opcode 0x25 @ `0x413ad4`, `0x413b18` | state machine checks | bool-ish integer | — | save |
| `frame_counter` | +4 | `dword` | opcode 0x25 @ `0x413b59` | update loop | integer | — | save |
| `command_ptr` | +8 | `KgtScriptItem*` | opcode 0x25 @ `0x413b5f` (`mov g_sprite_data_command_ptrs[edx], edi`) | character_state_machine (re-iterate script opcodes for the afterimage) | Per-player compiled script stream | **C** | As with box pointers; script memory must be stable. |
| `state_flags` | +12 | `dword` | opcode 0x25 @ `0x413b6b` | state machine | flags | — | save |
| `position_data[400]` | +16..+1616 | 400 × 4-byte entries, each with pos/color/anim-frame-ptr fields | `character_state_machine` @ `0x4122e8` (pos x), `0x4122fc` (pos y), `0x412311` (facing+bank), `0x412314` (anim-frame ptr) | `sprite_rendering_engine` @ `0x40cd3a..0x40d4e1` | The per-entry `+0x1C` stores a **pointer to a sprite animation frame** inside compiled script data — **another Class C pointer** | **C** | 1600 × 4 = yes, nested script-stream pointer per entry. |

**Observed object→afterimage linkage:** `KgtRuntimeObject+337` stores the afterimage slot index (0..99 or 0=none). That's an INDEX, not a pointer. The slot's `command_ptr` is the sole pointer field; the payload's per-entry `+0x1C` DWORDs (referenced as `ebp` in the writer) are additional script-stream pointers.

### 2.5 Global state pointers (non-struct fields)

| Global | Address | Type | Classification | Notes |
|---|---|---|---|---|
| `g_current_object_ptr` | `0x4259A8` | `_node*` | **B** | Live iterator into node pool. |
| `g_object_data_ptr` | `0x4CFA00` | `KgtRuntimeObject*` | **A** | Live iterator into object pool; read by virtually every per-object function. |
| `g_p1_opponent_ptr` | `0x4DFC7D` (slot 0) | `KgtRuntimeObject*` | **A** | Per-player slot field aliased as a top-level global; stride 57407. |
| `g_nearest_opponent_ptr` | `0x4DFCE9` (slot 0) | `KgtRuntimeObject*` | **A** | Per-player; recomputed each frame but must be saved. |
| `g_last_hit_target_ptr` | `0x4DFC79` (slot 0) | `KgtRuntimeObject*` | **A** | Per-player. |
| `g_character_data_array` | `0x4DFC75` | alias = slot 0 base | — | Aliases into `KgtPlayerRuntimeSlot`; not a distinct pointer field. |
| `g_hitbox_sprite_data_ptrs` | `0x4D1E94` (slot 0) | `KgtPictureHeader*` | **C** | Per-player; points into compiled character data. Stable for match duration. |
| Effect params (`g_effect_id_1/2`, `g_shake_effect_*`) | `0x447D7D+` and `0x4456D0+` | non-pointer dwords/bytes | — | Global effect state blocks; no pointer fields. |

No **type D** (heap) nor **type E** (OS-owned) pointers were found inside the object pool, the node pool, the afterimage pool, or the `KgtPlayerRuntimeSlot`. DirectDraw/DSound handles live in separate globals that are NOT part of rollback state (correctly excluded).

---

## 3. Focused section — box pointer array at `+137` (hit) and `+217` (hurt)

### 3.1 Where they're written

The editor's `[FA]` opcode is opcode `0x18` in the game; `[FB]` is `0x19`. Writers (confirmed via disasm of `character_state_machine`):

```
; opcode 0x18 — HIT BOX install
412cfa  xor     eax, eax
412cfc  cmp     word ptr [edi+5], 0     ; halfW != 0
412d01  mov     al, [edi+9]             ; box slot index (0..19)
412d04  lea     eax, [esi+eax*4+89h]    ; dst = obj + 137 + slot*4  (hit box array)
412d0b  mov     [eax], edi              ; store script_opcode_ptr

; opcode 0x19 — HURT BOX install
412d63  xor     ecx, ecx
412d65  cmp     word ptr [edi+5], 0
412d6a  mov     cl, [edi+9]             ; slot index
412d6d  lea     eax, [esi+ecx*4+0D9h]   ; dst = obj + 217 + slot*4  (hurt box array)
412d74  mov     [eax], edi              ; store script_opcode_ptr
```

The stored value is `edi`, which is the current opcode stream pointer inside the character_state_machine interpreter. It advances through a **per-player compiled script buffer**.

### 3.2 Where those pointers point

`edi` at the time of write is iterating through the per-player compiled script. The character-file loader (`player_data_file_loader @ 0x403a17`) loads the compiled script blocks at match-start and wires a pointer to the action table at `player_slot+0x110 (+272)`. The script region is NOT a heap allocation that is freed across frames; it is freed only when the whole character is unloaded (match end / mode transition).

The fields the consumers read:

```c
struct KgtScriptItem_box {   // 12+ bytes at the stored edi
    uint8_t  opcode;          // [edi+0]
    int16_t  cx_off;          // [edi+1]
    int16_t  cy_off;          // [edi+3]
    int16_t  halfW;           // [edi+5]
    int16_t  halfH;           // [edi+7]
    uint8_t  slot_idx;        // [edi+9]
    uint8_t  type_flags;      // [edi+10]  ← & 1 tested for "active"
    uint8_t  damage_scale;    // [edi+11]
    uint16_t damage;          // [edi+12]
};
```

Consumers:
- `physics_collision_handler` @ `0x40fcdf` reads `[box+1..+10]` per slot.
- `ProcessHitboxCollisions` @ `0x40eb98` reads same fields for both sides (its attacker array is also at `obj+137` internally — it's the HIT box array).
- `hit_detection_system` @ `0x40f1aa` reads defender's hurt boxes at `obj+217` and attacker's hit box via `[attacker+34+i]`.
- `sprite_rendering_engine` @ `0x40d881/0x40d925` calls `RenderHitboxDebug` / `RenderHitboxOutline` on each pointer for debug draw.

### 3.3 Rollback-integrity verdict

- **Classification: C** — pointer into per-player compiled script data.
- **Risk**: if the rollback save/restore cycle includes any code path that re-parses character files or reallocates the script region, every object's box arrays become dangling. This is 40 pointers × up to 1024 objects = **40,960 potential crashes** on restore.
- **Mitigation**: (i) treat the script region as immutable for the duration of a match, (ii) never call `player_data_file_loader` in a rollback-reachable path, (iii) either save the entire object pool byte-for-byte (preserving the pointer values as-is) OR clear the box arrays on restore using `ClearObjectHitboxData(obj)` and let the next frame re-run the opcode stream to repopulate them.
- **Cleaner option (recommended)**: treat the box pointer arrays as **scratch state**. On restore, zero `obj+137..obj+297` (clears both 20-entry arrays + reaction pointer via `ClearObjectHitboxData` + manual clear of `obj+297`). The next advance of the script machine will re-install them. Downside: one frame of missing hitboxes on the restored frame. Upside: complete immunity to script-region re-addressing.

---

## 4. Focused section — node pool linked-list chain integrity

`g_object_node_pool @ 0x4CFA20` is **exactly 8192 bytes** (1024 × 8 — one node per potential object). Each node:

```c
struct ObjectNode {
    KgtRuntimeObject* obj_ptr;   // +0 — type A
    ObjectNode*       next_ptr;  // +4 — type B
};
```

Every frame, `update_game_state` @ `0x404cfb` **rebuilds the entire linked list from scratch**:

```c
current_node_ptr = &g_object_node_pool;
for (i = 0; i < 1024; i++) {
    if (g_object_pool[i].state != 0) {
        list_idx = 2 * g_object_pool[i].layer;     // layer in obj[4..7]
        if (g_object_list_tails[list_idx])
            g_object_list_tails[list_idx]->next = current_node_ptr;
        else
            g_object_list_heads[list_idx] = current_node_ptr;
        current_node_ptr->obj_ptr = &g_object_pool[i];
        g_object_list_tails[list_idx] = current_node_ptr;
        current_node_ptr->next = 0;
        current_node_ptr += 1;  // 8 bytes
    }
}
g_object_data_ptr = &g_object_pool[1024];   // end marker
g_current_object_ptr = current_node_ptr;
```

### 4.1 Integrity requirements

- **Heads + tails + full node pool MUST be saved atomically.** The list is chained by pointers; a save that captured heads but not tails (or the pool but not the heads) would restore to a state where `tails[k]->next == 0` is still true but `heads[k]` points at the wrong node, causing iteration to diverge.
- `g_object_list_heads` is 128 × 8 bytes (heads interleaved with tails — heads at `0x430240, 0x430248, ...`, tails at `0x430244, 0x43024C, ...`). Total region `0x430240..0x430640` = 1024 bytes.
- `g_object_list_heads_end = 0x430640`, `g_object_list_end = 0x430644` are compile-time constants (not stored data) and need no save.
- **Rollback shortcut**: because the list is fully reconstructed every frame from the object pool, you CAN skip saving the node pool / heads / tails IF your save point is **between** frames (after `update_game_state` completes). If the save can occur mid-frame (during hitbox checks, AI, rendering), you MUST capture them.

### 4.2 Iterator pointers

Two globals are **live iterators** into these pools, read by almost every per-object handler:

- `g_current_object_ptr @ 0x4259A8` — advances through the node pool.
- `g_object_data_ptr @ 0x4CFA00` — points to the object the current handler is operating on; `[g_object_data_ptr+156]` = player slot.

Both must be in the rollback save region; restoring an object pool without these would leave the handlers writing to a stale slot.

### 4.3 Classification

All node and list-head fields are **type A/B** (pool-internal). Safe if the pools are saved at identical virtual addresses — which they are, since these are `.bss` globals with fixed RVAs, not heap allocations.

---

## 5. Focused section — afterimage pool ownership

### 5.1 Layout

Pool: `0x447F80 .. 0x46F6C0` = 100 entries × 1616 bytes = 161600 bytes. Each entry is one `KgtAfterimageEntry`:

```c
struct KgtAfterimageEntry {
    uint32_t       active_flag;       // +0
    uint32_t       frame_counter;     // +4
    KgtScriptItem* command_ptr;       // +8   ← type C
    uint32_t       state_flags;       // +12
    // +16..+1616 = 400 entries, each 4 bytes, containing pos/color/anim-frame-ptr data
    struct {
        int32_t x;                    // set by character_state_machine @ 0x4122e8
        // ... (shared dword per-field offsets: +4 =y, +0x18 =flags, +0x1C =anim_frame_ptr)
    } entries[400];
};
```

The "per-field" global names (`g_sprite_data_active_flags`, `g_sprite_data_frame_counters`, `g_sprite_data_command_ptrs`, `g_sprite_data_state_flags`, `g_sprite_data_buffer`) are aliases for slot-0's individual fields, with stride 1616 bytes between slots (404 DWORDs).

**Additional pool-0 header**: four DWORDs at `0x447930..0x44793F` (`g_sprite_position_data`, `g_sprite_animation_frame`, `g_sprite_data_array`, `g_sprite_frame_counter`) form a dedicated slot-0 preamble that lives BEFORE the main pool and is used as `offset + 1616*slot` for individual-field lookups. These must be included in the afterimage save region.

### 5.2 Pointer contents

- **`command_ptr` at entry+8** — type **C**, script stream pointer (written by opcode 0x25 `[FC AfterImage]` at `0x413b5f`).
- **Per-entry anim-frame pointer at entry's payload +0x1C of each sub-entry** — also type **C** (written at `0x412314` — `mov [eax+1Ch], ebp` where ebp = `g_sprite_animation_frame[ecx] + frame*16` = pointer into per-player sprite data).

### 5.3 Object→afterimage linkage

The object has an **integer index** at `+337` (`*(obj + 337)`), NOT a pointer. `sprite_rendering_engine @ 0x40cd3a` multiplies by 1616 to compute the slot. This means the object→afterimage link is automatically stable across rollback, PROVIDED the afterimage pool is saved at the same base address (it is — fixed `.bss`).

### 5.4 Ownership cleanup

`ClearObjectReferences @ 0x40e4a0` zeroes the afterimage slot's `active_flag` (`g_sprite_position_data[404 * obj[+337]] = 0`) when the owning object dies. This is the only "free" path; no `GlobalFree`/`free` is involved.

### 5.5 Rollback verdict

- **Save the entire afterimage pool** (`0x447F80..0x46F6C0`, 161600 bytes) and the slot-0 preamble (`0x447930..0x44793F`, 16 bytes).
- The **command_ptr and per-entry anim-frame pointers are type C** — safe as long as per-player compiled sprite/script data isn't reallocated.
- NO heap allocations or OS handles live in this pool.

---

## 6. Non-issues / confirmed safe

- **No `GlobalAlloc`/heap pointers** found inside `KgtRuntimeObject`, `KgtPlayerRuntimeSlot`, `g_object_node_pool`, `g_object_list_heads`, or the afterimage pool.
- **No Win32 HANDLE / DirectDraw / DirectSound pointers** in rollback state. DirectDraw surface pointers (hMem, ppvBits, lpDDSPrimary) and DirectSound buffer pointers live in separate globals that are touched only by render/audio code outside the rollback save region.
- **No stack pointers** stored in state (no obvious `F`-class bugs).

---

## 7. Recommended rollback save regions

The minimum-correctness rollback snapshot for pointer-integrity is:

1. **Object pool**: `0x4701E0 .. 0x4FE560` (1024 × 382 = 391168 bytes). Covers all 40+1 pointer fields per object (hit/hurt arrays, reaction, parent).
2. **Node pool**: `0x4CFA20 .. 0x4D1A20` (1024 × 8 = 8192 bytes).
3. **List heads/tails**: `0x430240 .. 0x430640` (1024 bytes, 128 head+tail pairs).
4. **Live iterators**: `g_current_object_ptr` (4B @ `0x4259A8`), `g_object_data_ptr` (4B @ `0x4CFA00`), `g_max_objects` (optional, constant).
5. **All 8 player slots**: `0x4D1D80 .. 0x5514C0` (`57407 * 8 = 459256` bytes). Covers `last_hit_target_ptr`, `opponent_ptr`, `nearest_opponent_ptr`, `spawn_slot_ptrs[10]`, and every scalar like HP/input-mirror.
6. **Afterimage pool**: `0x447930 .. 0x46F6C0` (preamble 16B + 100×1616 = 161616 bytes).

Total ≈ **1,021,472 bytes** if all saved naively. This is consistent with the ~850–1000 KB figures in existing FM2K rollback docs.

The **script region** (per-player compiled character data pointed at by `player_slot+0x110`, typical size hundreds of KB per character) must be **kept stable for the match**, not state-saved.

---

## 8. Items needing live verification (ask before asserting)

The following require runtime inspection that this static audit could not confirm:

1. Does `player_data_file_loader` ever run in a rollback-reachable path (e.g., during a demo→vs transition)? If yes, every box pointer in the pool goes dangling.
2. Is `mem_clear(obj+137, 80)` at `ClearObjectHitboxData` called frequently enough that box pointers are effectively scratch? If so, rollback could safely skip the box-pointer region and let the next frame repopulate.
3. What does offset `+0x160..+0x175` (22 bytes) in the object contain between the flag region and the parent pointer at +378? Static analysis didn't identify writes — likely padding or scalar scratch. Confirm with a memory diff.
4. Do any of the per-slot `g_hitbox_sprite_data_ptrs[slot]` entries ever get updated mid-match (e.g., during costume swap)? If they do, rollback needs to treat them as state.

---

## 9. IDA comment placement summary

Comments have been applied to the following addresses (26 tagged):

- `0x404cf1`, `0x404cfb`, `0x404d27`, `0x404d2a` — node pool linked-list build
- `0x4065c3` — `create_game_object` parent-pointer write
- `0x40656c` — `DeactivateCharacterProjectiles` spawn-slot memset
- `0x40e4a0` — `ClearObjectReferences`
- `0x40e550` — `ClearObjectHitboxData`
- `0x40e5ed`, `0x40e672` — nearest_opponent_ptr clear/write
- `0x40eb8e` — hit box array read in `ProcessHitboxCollisions`
- `0x40cd3a` — afterimage slot index read in `sprite_rendering_engine`
- `0x40f2da`, `0x40f2e0`, `0x40f8b9` — last_hit/opponent_ptr writes in `hit_detection_system`
- `0x40fa33`, `0x40fae6`, `0x40fcc9` — pointer reads in `physics_collision_handler`
- `0x410d37` — character data ptr read in `SetCharacterAttackState`
- `0x412cfa`, `0x412d0b`, `0x412d63`, `0x412d74`, `0x412d95` — box/reaction opcode writers
- `0x412f8d` — `g_hitbox_sprite_data_ptrs` load
- `0x413b5f` — afterimage `command_ptr` write
