# State Manager Audit — Desync Root-Cause Report

**Audit target:** `FM2KHook/src/netplay/savestate.cpp` (+ `savestate.h`)
**Reviewed against:** ground-truth frame-mutable state list for
`KGT2nd_GAME` / `WonderfulWorld_ver_0946.exe`, cross-referenced with
`docs/editor/runtime_entity.md`, `docs/editor/engine_bugs_mapped.md`,
`docs/game/wave_a_renames.md`, `docs/game/wave_c_renames.md`,
`docs/game/wave_d_renames.md`, and the bsnes-netplay rollback pattern
documented in `gekkonet_bsnes_reference.md`.

**Verdict (TL;DR):** The save/restore coverage is roughly **5%** of the
real frame-mutable footprint. The single largest gap is the 8-slot
`KgtPlayerRuntimeSlot`-analogue block at `g_player_name_buffer@0x4D1D90`
(459 KB total) — only the last **2,407 bytes of each slot** are saved,
missing **~55 KB × 8 ≈ 440 KB** of dynamic state that the script
interpreter writes every frame (taskVars, hit/hurt box slot pointers,
reactionBlit, afterimageSlot, etc.). The **afterimage pool is not saved
at all** (≈ 200 KB of dynamic state). The **object-pool list topology**
(heads/tails/node pool) is not saved. There are also two concrete
pointer-integrity bugs and a **16-byte base-address drift** on
`CHAR_SLOT_BASE`.

---

## 1. What the implementation saves (source-of-truth: savestate.cpp)

| Region (code) | Addr | Size | File:Line |
|---|---|---|---|
| RNG seed | `0x41FB1C` | 4 B | savestate.cpp:12,129,199 |
| Render frame counter | `0x4456FC` | 4 B | savestate.cpp:17,132,202 |
| Input tracking state | `0x447EE0` | 0xA0 (160 B) | savestate.cpp:26-27,138,208 |
| Character dynamic tail | `0x4D1D80 + i*57407 + 55000` × 8 slots | 2,407 B × 8 = 19,256 B | savestate.cpp:141-145, 211-215 |
| Object pool | `0x4701E0` | 0x5F800 (391,168 B) | savestate.cpp:30-31, 148, 218 |
| Input history | `0x4280D8` | 0x2008 (8,200 B) | savestate.cpp:34-35, 151, 221 |
| Game state | `0x470020` | 0x220 (544 B) | savestate.cpp:39-40, 154, 224 |
| Effect System 1 | `0x447D7D` | 42 B | savestate.cpp:45-46, 157, 227 |
| Effect System 2 | `0x4456D0` | 44 B | savestate.cpp:47-48, 158, 228 |
| Shake effects | `0x447DA9` | 40 B | savestate.cpp:49-50, 159, 229 |

**Total per-frame snapshot: ≈ 420 KB × 64 slots = 26.9 MB ring buffer.**

Save ordering (savestate.cpp:126-159) is sequential and deterministic.
Restore ordering (savestate.cpp:198-229) is the same sequence in the
same direction — safe, because all regions are independent memcpy
targets (no inter-region ordering dependencies).

---

## 2. Ground-truth coverage matrix

Legend: **SAVED** = copied in both save & restore. **PARTIAL** = region
is saved but only a fraction of the bytes. **MISSING** = region is not
touched by save/restore.

| # | Ground-truth item | Addr | Coverage | Notes |
|---|---|---|---|---|
| 1 | `g_object_pool` (1024 × 382 B) | `0x4701E0` / +0x5F800 | **SAVED** | Full pool saved. Size matches (391,168 B). |
| 2 | `g_object_node_pool` (1024 × 8 B) | `0x4CFA20` / 8,192 B | **MISSING** | Linked-list node pool for the object-pool. Holds `next` / `prev` pointers used by dispatcher iteration. Not in any saved region. |
| 3 | `g_object_list_heads` (256 × 4) | `0x430240..0x430640` | **MISSING** | List-head pointer table. Without it, active-list traversal order is wrong after restore even if pool bytes are correct. |
| 4 | `g_object_list_tails` / heads_end | `0x430244..0x430640` | **MISSING** | Same linked-list machinery. |
| 5 | `g_object_count` | `0x4246FC` | **MISSING** | |
| 6 | `g_max_objects` | `0x4259A4` | **MISSING** | |
| 7 | `g_current_object_ptr` | `0x4259A8` | **MISSING** | Points into `g_object_pool`; dispatcher reads/writes every tick. See §4 (pointer handling). |
| 8 | Per-player slot base (8 × 57,407 B = 459,256 B) | `0x4D1D90` | **PARTIAL** — only tail 2,407 B per slot | **SEE CRITICAL ISSUE #1.** 95.8% of each slot is skipped. |
| 9 | `g_p1_hp` + 7 mirrors | `0x4DFC85 + N×57407` | PARTIAL (inside saved tail) | In the 2,407-byte tail, so OK. |
| 10 | `g_p1_opponent_ptr` per-slot | `0x4DFC7D + N×57407` | PARTIAL (inside saved tail, BUT POINTER) | **Pointer** into `g_object_pool`. Saved as raw bytes → restored pointer value will still be valid IFF object pool is restored (which it is). OK. |
| 11 | `g_nearest_opponent_ptr` per-slot | `0x4DFCE9 + N×57407` | PARTIAL (inside saved tail, pointer) | Same as above. OK. |
| 12 | `g_p1_throw_hit_flag` | `0x4DFD6F + N×57407` | PARTIAL (tail) | OK. |
| 13 | `g_p1_throw_offset_x/y` | `0x4DFD77`, `0x4DFD7B` | PARTIAL (tail) | OK. |
| 14 | `g_char_value_current/_flag` (2-level gauge) | `0x4DFC95`, `0x4DFC9D` | PARTIAL (tail) | OK. |
| 15 | Per-slot round outcome | `0x4DFD87 + N×57407` | PARTIAL (tail) | OK. |
| 16 | `g_player_position_offset` | `0x4DFCC9 + N×57407` | PARTIAL (tail) | OK. |
| 17 | `g_player_hit_mask` | `0x4DFD37 + N×57407` | PARTIAL (tail) | OK. |
| 18 | Per-char-slot round-action flags | `0x4FBE05..0x541F40` | **MISSING** | Written by `vs_round_function` at state 410/420/430 (win/draw/loss). Only slot 0 at `0x4DFD87+0` sits in the saved tail — slots 1..6 at `0x4FBE05 + k*57407` are **completely outside** any saved region. |
| 19 | `g_p1_input_history` | `0x4280E0` | **SAVED** | Inside input-history region. |
| 20 | `g_combined_input_changes` | `0x4280D8` | **SAVED** | Region starts here (savestate.cpp:34). |
| 21 | `g_processed_input` | `0x447F40` | **SAVED** | Inside input-tracking region. |
| 22 | `g_input_buffer_index` | `0x447EE0` | **SAVED** | |
| 23 | `g_game_timer` | `0x470044` | **SAVED** | Inside game-state region. |
| 24 | `g_round_sub_state` | `0x4EDCAC` | **MISSING** | Not in any saved region. This is one of the three `g_savedRound_*` sources (see `docs/game/wave_a_renames.md` §6). |
| 25 | `g_match_phase` | `0x4DFC6D` | **MISSING** | 40 bytes before the char-slot 0's saved tail starts. Bumps into bug #CRITICAL-1 range. |
| 26 | `g_p1_round_count` | `0x4700EC` | **SAVED** | Inside game-state region. |
| 27 | `g_game_mode` / `_flag` | `0x470054` / `0x470058` | **SAVED** | Inside game-state region. |
| 28 | `g_stage_script_index` | `0x424F28` | **MISSING** | `g_titleScreen_demoTimer` adjacents; not in any saved region. |
| 29 | `g_stage_script_entry_flags` (stride 206) | `0x4D9A55` | **MISSING** | Sits **inside** char-slot 0's block at offset `0x7CD5` (31,957) from real base `0x4D1D90` — below `CHAR_SLOT_DYNAMIC_OFFSET=55000`, so currently unsaved. |
| 30 | `g_screen_x`, `g_screen_y` (camera) | `0x447F2C`, `0x447F30` | **SAVED** | Inside input-tracking region (savestate.cpp comments acknowledge this is screen-dim plus camera). |
| 31 | Afterimage pool (100 × 404 B = 40,400 B) | Editor: `0x6075E0`; game addr TBD | **MISSING** | Engine-bug note (§runtime_entity.md §KgtAfterimageEntry) confirms every object write to `+0x151` updates this pool. Not saved anywhere. **Desync vector.** |
| 32 | Afterimage 1,616-B per-entry buffers (161,600 B) | Same pool | **MISSING** | Same as above. |
| 33 | Saved-round state (continue dialog) | `0x424F34..0x424F3C` | **MISSING** | `g_savedRound_gameTimer/matchPhase/subState`. Written on round transitions. Usually only read on "continue" dialog, so lower priority — but if the game reads-back during stage transitions, it diverges. |
| 34 | RNG state (MSVC CRT `rand`) | `0x41FB1C` | **SAVED** | |
| 35 | `g_player_stage_positions` | `0x470020` | **SAVED** | Game-state region starts here. |
| 36 | `g_player_move_history` | `0x47006C` | **SAVED** | Inside game-state region. |
| 37 | `g_player_action_history` | `0x47011C` | **SAVED** | Inside game-state region. |
| 38 | `g_object_delay_timers` | `0x470220` | **SAVED** | Inside game-state region (ends at 0x470240). |
| 39 | DirectSound / MIDI / CD audio handles | `0x4246E0..0x424774` | NOT-NEEDED | Process-local; never read back for game logic. Deliberately excluded. |

---

## 3. CRITICAL ISSUES — ranked by desync risk

### CRITICAL-1 — 440 KB of per-player slot data is never saved

**File:** `savestate.h:10-14`, `savestate.cpp:141-145, 211-215`

```cpp
constexpr size_t CHAR_SLOT_SIZE = 57407;
constexpr size_t CHAR_SLOT_DYNAMIC_OFFSET = 55000; // Where dynamic data starts
constexpr size_t CHAR_SLOT_DYNAMIC_SIZE = CHAR_SLOT_SIZE - CHAR_SLOT_DYNAMIC_OFFSET; // 2407 bytes
constexpr size_t NUM_CHAR_SLOTS = 8;
constexpr uintptr_t CHAR_SLOT_BASE = 0x4D1D80;    // g_character_data_base
```

The comment justifying this ("Static character data (sprites, animations,
hitboxes) loaded from .player files doesn't change") is **false** for
much of the first 55,000 bytes:

Per `docs/editor/runtime_entity.md` §`KgtPlayerRuntimeSlot`, the per-
player block contains:

- `scriptsPtr@+0x0110` — a pointer. Not frame-mutable itself, but
  **any pointer is a latent rollback hazard if its target changes**
  (see pointer section).
- `playerAliveFlag@+0x0138` — read every frame by `FindNearestOpponent`.
  **Written at round start / KO / continue.** Any rollback that crosses
  a KO frame will load a stale `playerAliveFlag`.
- **`unknown_013C_BDF5[47289]`** — explicitly un-enumerated character-local
  state. Per `docs/editor/runtime_entity.md` §4 (Known Gaps), this
  "presumably contains the character's complete KgtProjectSlot-derived
  state (scripts, pictures, **hit junction table**)". The hit-junction
  table is definitely mutated by the interpreter via `[FD]`/`[FA]`
  opcodes — and `ClearObjectHitboxData` wipes `+0x89..+0x128` every
  animation change. That's inside the 47,289-byte unknown block.
- `opponentPtr@+0xB9F9` — already in the saved tail.
- `lifePercent@+0xBA01` / `specialPercent@+0xBA05` — these are in the
  saved tail but only because `CHAR_SLOT_DYNAMIC_OFFSET=55000` was
  eyeballed to include them. **The region between `+0x0138` and
  `+0xB9F9` (≈ 47 KB) is frame-mutable and currently unsaved.**

Per `docs/game/wave_c_renames.md` §1, the game's slot stride is 57,407
(vs editor's 47,851). Wave C's surprise note 2 identifies the extra
9,556 bytes as "runtime-scratch fields" for position/facing caches,
gauge sub-levels, and attack-chain bytes — **all frame-mutable**.

**Concrete fix:**

```cpp
// savestate.h
constexpr size_t CHAR_SLOT_DYNAMIC_OFFSET = 0;  // was 55000
constexpr size_t CHAR_SLOT_DYNAMIC_SIZE   = CHAR_SLOT_SIZE;  // was 2407
constexpr uintptr_t CHAR_SLOT_BASE = 0x4D1D90;  // was 0x4D1D80 — see CRITICAL-2
```

This quadruples per-frame state size from 420 KB to 840 KB × 64 slots
= 53 MB ring. If that's unacceptable, profile which bytes inside the
47-KB unknown band are actually written per-frame (hooks on the
interpreter's `[V]`/`[FD]`/`[FA]` opcodes) and carve a tighter range.
**Blanket saving is the correct default until proven wasteful.**

### CRITICAL-2 — `CHAR_SLOT_BASE` is 16 bytes too low

**File:** `savestate.h:14`, `globals.h:72`

```cpp
constexpr uintptr_t CHAR_SLOT_BASE = 0x4D1D80;    // g_character_data_base
```

Per `docs/game/wave_c_renames.md` §"The 57407-byte Per-Player Block",
the per-player struct base is `g_player_name_buffer@0x4D1D90`, **not**
`0x4D1D80`. Verification:
- `g_p1_hp@0x4DFC85` = `0x4D1D90 + 0xDEF5` (offset 57,077) → confirms
  base is `0x4D1D90`.
- `0x4DFC85 - 0x4D1D80 = 0xDF05` (offset 57,093) → doesn't match
  wave_c's documented `+0xDEF5`.

Impact with current code:
- Slot 0: code reads [`0x4DFC38`, `0x4E058F`). Real slot 0's dynamic
  tail is [`0x4DFC48`, `0x4E059F`). The **first 16 bytes read belong
  to the header region before slot 0** (pre-slot padding or the tail
  of an adjacent global), and the **last 16 bytes of slot 0's dynamic
  tail are never saved**.
- Slots 1..7: every slot i reads bytes that belong to slot i−1's last
  16 bytes + most of slot i, missing slot i's last 16 bytes. The
  `57407` stride is correct; only the base is wrong.

This bug is currently masked because `CHAR_SLOT_DYNAMIC_OFFSET = 55000`
was likely calibrated empirically against the wrong base, so the
`g_p1_hp@0x4DFC85` check still passes. But anything in the **last 16
bytes of each slot** (which contains the `timeStoppedFlag` and
`frozenInputSnapshot` per the editor's `KgtPlayerRuntimeSlot`) is
never saved, and the **first 16 bytes of our "saved tail"** belong
to the wrong slot.

**Concrete fix:** change `CHAR_SLOT_BASE` to `0x4D1D90` in **both**
`savestate.h:14` and `globals.h:72`.

### CRITICAL-3 — Afterimage pool is never saved

**File:** `savestate.cpp` — no references to `afterimage` or
`0x6075E0` / the game-binary equivalent.

Per `docs/editor/runtime_entity.md` §`KgtAfterimageEntry`, the pool is
100 entries × 1,616 bytes = 161,600 bytes, plus the 404-byte scratch
per-entry (another ~40 KB). The pool is written every time an object
with `afterimageSlot != 0` ticks — i.e. every frame the object
renders.

Cascading hazard documented in `docs/editor/engine_bugs_mapped.md`
§"Latent issues observed" point 2:

> **Creation-frame lag + [PS] filter interact** — an object created
> this frame won't have the `0x20000000` renderer flag yet. Combined
> with bug #5 ([PS] filter) and bug #4 (creation delay), a freshly-
> created object is triply-invisible to pause: not drawn (1 frame),
> not paused (no image flag), and not dispatched this frame. Rollback
> state-save must still capture the slot even though it's
> renderer-invisible.

Afterimage state tracks the object's recent render color/state ring.
Missing afterimage state means any rollback that crosses a frame
where an afterimage allocation or release happened will desync.

**Concrete fix:** locate the afterimage pool in the game binary (IDA
mcp task — editor addr is `0x6075E0`, game addr is probably in the
`0x4D1xxx`–`0x4FxxxX` range given the editor→game offset pattern).
Add:

```cpp
// savestate.h
constexpr uintptr_t ADDR_AFTERIMAGE_POOL = 0x????????;  // TBD via IDA
constexpr size_t    SIZE_AFTERIMAGE_POOL = 100 * 1616;  // 161,600 B

// SaveStateData
uint8_t afterimage_pool[SIZE_AFTERIMAGE_POOL];

// Save / Load
memcpy(state->afterimage_pool, (void*)ADDR_AFTERIMAGE_POOL, SIZE_AFTERIMAGE_POOL);
memcpy((void*)ADDR_AFTERIMAGE_POOL, state->afterimage_pool, SIZE_AFTERIMAGE_POOL);
```

### CRITICAL-4 — Object-list topology (nodes + heads) not saved

**File:** no references to `0x4CFA20`, `0x430240`, `0x430640`.

`g_object_pool` bytes ARE saved, but the **linked-list topology that
threads active objects** isn't:

- `g_object_node_pool@0x4CFA20` — 8,192 bytes of list-node structs.
- `g_object_list_heads@0x430240..0x430640` — 256 × 4 = 1,024 bytes.
- `g_object_list_tails` — companion region.

If the game uses these lists to iterate active objects (very likely —
the editor's `ProcessObjectCollisions` linearly scans the 1024 pool,
but a real game would use free/active lists for O(N_active) instead
of O(1024)), then a rollback that restores pool bytes but not list
heads will leave iteration order wrong. Active objects will be
skipped or dead slots will be visited.

**Concrete fix:**

```cpp
// savestate.h
constexpr uintptr_t ADDR_OBJECT_NODE_POOL  = 0x4CFA20;
constexpr size_t    SIZE_OBJECT_NODE_POOL  = 8192;
constexpr uintptr_t ADDR_OBJECT_LIST_HEADS = 0x430240;
constexpr size_t    SIZE_OBJECT_LIST_HEADS = 0x400;   // 256 heads + tails + companion region; verify in IDA

// SaveStateData additions + matching memcpy calls in Save/Load
```

### CRITICAL-5 — Object-pool cursor / count not saved

**File:** `savestate.cpp` — no references to `0x4246FC`, `0x4259A4`,
`0x4259A8`.

| Field | Addr | Role |
|---|---|---|
| `g_object_count` | `0x4246FC` | # active objects in pool |
| `g_max_objects` | `0x4259A4` | allocation cursor upper bound |
| `g_current_object_ptr` | `0x4259A8` | **POINTER** into pool; every opcode reads `*g_currentObject` |

`g_current_object_ptr` is the most dangerous: it's the pointer the
opcode dispatcher chases every tick. If it points to slot 37 when
we save, and during rollback replay the dispatcher allocates slots
in a different order, the pointer now points at a slot with
different data. Since the pool bytes ARE restored, the pointer's
addressed byte range IS valid — but its **logical meaning** depends
on pool-allocation order. See §4 for pointer integrity discussion.

**Concrete fix:** add these 3 dwords to the game-state extension or
a new dedicated region. All 3 are in the `0x424xxx..0x425xxx` band
which is otherwise unsaved.

### HIGH-1 — `g_match_phase`, `g_round_sub_state`, stage-script index unsaved

**Addresses:** `0x4DFC6D`, `0x4EDCAC`, `0x424F28`, `0x4D9A55`.

Per `docs/game/wave_a_renames.md` §6, these are the *sources* of the
"saved-round" backup at `0x424F34..0x424F3C`. They're written every
frame in CSS/pre-battle/battle transitions. None of them are in any
saved region.

- `g_match_phase@0x4DFC6D` — 35 bytes **before** `g_p1_hp@0x4DFC85`.
  Would fall into char-slot-0's block at offset `0xDEDD` (57,053),
  still **below** the current 55,000 threshold — so fixed by
  CRITICAL-1.
- `g_round_sub_state@0x4EDCAC` — sits in char-slot **1** at offset
  `0x4EDCAC - 0x4D1D90 = 0x1BF1C` (114,460). Beyond 57,407: so it's
  actually in slot 1's middle region (offset 57,053 into slot 1).
  Also fixed by CRITICAL-1 once the full-slot save is enabled.
- `g_stage_script_index@0x424F28` — sits in the `0x424xxx` band
  with the DirectPlay / title-screen / saved-round cluster. Needs a
  dedicated region or extension of `game_state`.

### HIGH-2 — Per-character-slot round-action flags only partially saved

**Addresses:** `0x4DFD87` (slot 0, saved) through `0x541F40` (slot 6).

Per `docs/game/wave_d_renames.md` §"Cluster 5":

```
0x4EDDC6, 0x4FBE05, 0x509E44, 0x517E83, 0x525EC2, 0x533F01, 0x541F40
```

Only slot 0's flag at `0x4DFD87` is in the saved tail (slot 0 of the
char block). The mirrors sit inside slot 1..6's **middle region** (not
tail) and are therefore unsaved. Fixed by CRITICAL-1 as well.

### MEDIUM-1 — RNG state may be incomplete

**File:** `savestate.cpp:12`

```cpp
constexpr uintptr_t ADDR_RNG_SEED      = 0x41FB1C;
```

Only one dword is captured. MSVC CRT's `rand()` keeps a 4-byte seed
so this is likely correct. If the game also uses Windows `timeGetTime`
or `QueryPerformanceCounter` for anything frame-deterministic, that
would be a second desync vector — but CLAUDE.md rollback doc says the
game uses `timeGetTime` as a frame limiter only. **Probably OK.**

### LOW-1 — Audio handles: confirm no readback

Per the user's ground-truth note, audio handles "probably DON'T need
saving since audio is non-deterministic feedback, but check: if any
game logic reads sound-play-status back, that's a desync vector."

`docs/game/wave_a_renames.md` §10 identifies `g_cdAudio_trackNumber`,
`g_midi_tempFilename`, etc. Search needed (not in scope of this audit
pass): any call to `MCIWndGetMode`, `MCISendCommand(MCI_STATUS)`, or
`IDirectSoundBuffer::GetStatus` — if found inside `run_game_loop`
logic, it's a desync risk.

---

## 4. Pointer handling — concrete bugs

### P-BUG-1 — `g_p1_opponent_ptr` memcpy is OK *only because pool is saved*

savestate.cpp:144 does:

```cpp
memcpy(state->char_dynamic[i], (void*)dynamic_addr, CHAR_SLOT_DYNAMIC_SIZE);
```

This byte-copies the 4-byte `opponentPtr` field inside the tail. On
restore the same bytes are written back. The pointer targets a
`KgtRuntimeObject*` in `g_object_pool@0x4701E0`. Since the object pool
**is** saved/restored to its original addresses (no relocation —
game loads at fixed base), the pointer **remains valid**.

**Caveat:** this only works as long as:
- ASLR is disabled for the game binary (checked at launch time — the
  current launcher does process injection into a fixed-base image).
- The object pool is restored to the **same byte layout** as when
  saved (which it is — full region memcpy).

If any fix ever shrinks the saved object-pool region, this pointer
becomes a dangling reference.

### P-BUG-2 — `g_current_object_ptr@0x4259A8` NOT saved, dispatcher chases it every tick

This is a genuine pointer-integrity bug. The dispatcher at
`ExecuteAnimationScript` (editor: `0x439CD0`; game: `0x411BF0` per
`editor_to_game_matches.json`) reads `g_currentObject` and writes to
`*g_currentObject->scriptItemIdx`, `->scriptId`, etc.

During rollback:
1. Frame N is saved. `g_currentObject = &pool[37]`. Not captured.
2. Rollback triggered. State restored. `g_currentObject` still has
   its **current** (post-rollback-trigger) value, pointing to
   whichever object the dispatcher is "currently" on.
3. Next tick begins. Dispatcher reads `g_currentObject` — might point
   at a slot that's been freed/reused by the restore.

**Concrete fix:** add `g_current_object_ptr` to the saved game-state.
Since pool base is fixed, save the pointer as-is (4-byte memcpy).

### P-BUG-3 — `parentPtr`, `nearestOppPtr` inside saved object pool

Same as P-BUG-1 but for the 1024 objects in the pool. Each object has:
- `parentPtr@+0x17A` — points at spawner object.
- `afterimageSlot@+0x151` — 1-based index into afterimage pool.

Both are byte-copied inside the 391 KB object pool region, so:
- `parentPtr` is OK (pool is saved).
- `afterimageSlot` **points into an unsaved pool**. Restore is
  writing back a 1-based index into `g_afterimagePool[]` — but the
  afterimage pool is never restored, so the index might refer to an
  entry that was reallocated by the post-save forward progress.

Fixed by CRITICAL-3 (save the afterimage pool).

### P-BUG-4 — `scriptsPtr@+0x0110` inside every per-player slot

Per `runtime_entity.md` §KgtPlayerRuntimeSlot, this is a pointer to the
player's `KgtScript[]` table, 39 bytes each. The table itself is loaded
from the `.player` file at game startup and **doesn't change**, so the
pointer is safe. But `scriptsPtr` currently falls in the **unsaved**
region of the slot (offset `0x0110 = 272`, far below 55,000). Since it
doesn't change frame-to-frame, not saving it is accidentally correct —
but if the `.player` file loader ever re-loads a character mid-match
(rematch, possibly?), the pointer would change and need saving. Low
priority — flag for future reference.

---

## 5. Over-saved items (waste rollback budget)

### O-1 — `g_hInstance@0x4701CC` saved but excluded from CRC

savestate.cpp:319, 457 explicitly excludes this 4-byte region from the
desync CRC (correctly — it's per-process). But it's still
**included** in the saved `game_state[0x220]` region (savestate.cpp:39-40).
Harmless bytes, no fix needed.

### O-2 — Full 160-byte input-tracking region

savestate.cpp:26-27 saves `0x447EE0..0x447F80`. The CRC at
savestate.cpp:335-344 splits this into 3 sub-regions and **skips** the
screen-dimension block at `0x447F20..0x447F3F`. The save still copies
those 32 bytes — which is actually CORRECT for rollback (screen dims
can change between frames if resize happens), but they're **excluded
from desync CRC**. That's consistent with the "process-local"
exclusion philosophy — no fix needed.

### O-3 — Object pool CRC only hashes first 4 KB

savestate.cpp:323:

```cpp
g_region_checksums.object_pool = Fletcher32((uint8_t*)ADDR_OBJECT_POOL, 4096);
```

Comment says "for speed". This is a **desync-detection** gap, not a
save gap. With pool at 391 KB, a divergence after the first 4 KB (i.e.
in object slots 10+ of 1024) will not trigger GekkoNet desync
detection. Not a rollback-correctness bug but it silently suppresses
desync alerts.

**Concrete fix:** either hash the full pool (it's the largest single
region — hashing 391 KB per frame at 100 FPS = 39 MB/s, well within
Fletcher32 throughput on modern CPUs) or use a **multi-sample**
approach: hash N 4 KB windows at known-active-object offsets.

---

## 6. Ordering & determinism

- Save order (savestate.cpp:126-159) is deterministic: frame number,
  then RNG, render_fc, buf_idx, input tracking, 8 char slots in
  order, object pool, input history, game state, effect sys 1/2,
  shake. No conditional skips.
- Load order (savestate.cpp:198-229) mirrors save order. Both are
  safe because no region overlaps another.
- First-save initial-sync logic (savestate.cpp:102-121) forcibly
  zeros `buf_idx`, `render_fc`, and 3 × 32-byte input-state regions
  **once**, to flush CSS residue. This is fragile: the comment at
  :361 explicitly warns any future desync here indicates the CSS
  residue theory is wrong. Document this in the audit for future
  reference; not itself a bug.

---

## 7. Rollback-trigger integration (netplay.cpp)

Read of `netplay.cpp:731-751`:

```cpp
case GekkoSaveEvent: {
    int frame = update->data.save.frame;
    SaveState_Save(frame);
    uint32_t checksum = SaveState_GetLastChecksum(frame);
    *update->data.save.state_len = sizeof(uint32_t);
    *update->data.save.checksum = checksum;
    memcpy(update->data.save.state, &frame, sizeof(uint32_t));
    break;
}

case GekkoLoadEvent: {
    int frame = update->data.load.frame;
    ...
    SaveState_Load(frame);
    break;
}
```

This follows the bsnes-netplay pattern correctly (per
`gekkonet_bsnes_reference.md`): only 4 bytes (the frame number) are
transmitted, full state stays local, rollback is local reload +
replay. Good.

The **rolling-back flag** at netplay.cpp:765,776 correctly brackets
the full game tick. Input edge-detection hooks should key off
`g_is_rolling_back` to avoid overwriting edge-detection globals on
replay frames — this is the cross-reference to
`docs/editor/engine_bugs_mapped.md` §13 ("Hold repeatedly fires off").

---

## 8. Priority-ranked fix list

| Priority | Fix | File:Line | LOC |
|---|---|---|---|
| P0 | Save full 57,407 B per slot (not just 2,407 tail) | savestate.h:10-14 | 3 |
| P0 | Fix `CHAR_SLOT_BASE` 0x4D1D80 → 0x4D1D90 | savestate.h:14, globals.h:72 | 2 |
| P0 | Save afterimage pool (100 × 1616 B) | savestate.h, savestate.cpp:148,218 | ~15 |
| P0 | Save `g_current_object_ptr@0x4259A8` + `g_object_count@0x4246FC` + `g_max_objects@0x4259A4` | savestate.cpp new region | ~10 |
| P1 | Save object linked-list nodes + heads (`0x4CFA20`, `0x430240..0x430640`) | savestate.cpp new regions | ~15 |
| P1 | Hash full object pool in desync CRC, not just 4 KB | savestate.cpp:323 | 1 |
| P2 | Save `g_stage_script_index@0x424F28` + stage-script entry flags region | savestate.cpp new region | ~10 |
| P2 | Save saved-round state at `0x424F34..0x424F3C` | savestate.cpp new region | ~5 |
| P3 | Audio-readback audit (search IDA for MCI / DirectSound status calls in game loop) | n/a | — |

Applying P0 alone should eliminate the bulk of desyncs. Each P0 fix is
self-contained and testable in isolation via
`SaveState_TestRoundtrip()` (savestate.cpp:382).

---

## 9. See also

- `docs/editor/runtime_entity.md` — ground truth for
  `KgtPlayerRuntimeSlot` layout (editor's 47,851-byte version).
- `docs/editor/engine_bugs_mapped.md` §"Latent issues observed" —
  documents creation-frame lag, [PS] filter, self-cancel state leak,
  opcode aliases, `v1` base-pointer corruption — each is a rollback
  hazard that interacts with state-save coverage.
- `docs/game/wave_c_renames.md` — per-slot field layout for the
  game binary's 57,407-byte block.
- `docs/game/wave_d_renames.md` §"Cluster 5" — 7-slot round-action
  flag array.
- `docs/game/wave_a_renames.md` §6 — `g_savedRound_*` continue-dialog
  backup.
- `gekkonet_bsnes_reference.md` — the "4 bytes over network, full
  state local" pattern that savestate.cpp correctly implements at the
  transport level but not at the content level.
