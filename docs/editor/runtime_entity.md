# Runtime Game Object — Entity Struct + Helpers

Reverse-engineers the runtime "object" pointed to by `g_currentObject` (old label
`dword_7D5190`) in `KGT2nd_EDITOR.exe`. This object is the target of every
codeblock the dispatcher at `ExecuteAnimationScript@0x439CD0` writes back to,
plus the input/output of the 11 physics / collision / animation-state helpers
listed below. Complementary to `docs/editor/opcode_dispatcher.md`.

Source evidence: disassembly of the 12 helpers listed below, each decompile
verified against the writes and reads enumerated in the opcode dispatcher doc.
Proofs cited as `0x4xxxxx` addresses in the field tables.

## `KgtRuntimeObject` — 382 bytes (`0x17E`)

Stride matches `sizeof(g_objectPool[i])` (1024 entries, start at
`unk_775900` = `g_objectPool`). The pool is linearly scanned by
`CreateAnimationObject@0x436960` (first slot with `status == 0` wins) and by
`[PS] 0x1A`, `FindNearestOpponent@0x439110`, `ProcessObjectCollisions@0x439250`.

```c
struct __attribute__((packed)) KgtRuntimeObject {      // size = 382
    // --- header / status (0x00..0x07) ---
    int  status;                // +0x000  0=free, 1=destroyed (set by DestroyGameObject), 2=active
    u8   unknown_04[4];         // +0x004  reserved; byte +4 looks like a kind-byte written by caller

    // --- kinematics (0x08..0x27) ---
    int  posX;                  // +0x008  X world position (16.16 fixed)       (ProcessObjectCollisions: [edi-10h])
    int  posY;                  // +0x00C  Y world position (16.16 fixed)       (ProcessObjectCollisions: [edi-0Ch])
    int  groundY;               // +0x010  ground / pushback target Y            (ProcessObjectCollisions: [edi+10h])
    int  physicsFlags;          // +0x014  bit 0 = facing (right=0 / left=1),
                                //         bit 1 = jumping (ProcessObjectPhysics: +20)
    int  moveX;                 // +0x018  horiz velocity (integrated into posX)
    int  moveY;                 // +0x01C  vert velocity (integrated into posY)
    int  accelX;                // +0x020  horiz accel (added to moveX each tick)
    int  accelY;                // +0x024  vert accel

    // --- per-frame flags / script ptrs (0x28..0x53) ---
    int  flagsA;                // +0x028  misc flags; bit 0x20000000 = "pausable-by-[PS]" (dispatcher)
    int  scriptItemIdx;         // +0x02C  current script-item buffer index (aka state+44)
    int  scriptId;              // +0x030  current script id (+48)
    u8   unknown_34[8];         // +0x034
    int  waitCounter;           // +0x03C  [I]-driven wait countdown (-1 = infinite)
    int  pauseTimer;            // +0x040  -1 stall, >0 countdown, 0 = run; also [PS] stores per-other-object

    // --- color modulation (0x44..0x57) --- written by [COLOR] 0x23 ---
    int  colorR;                // +0x044  signed int32, clamped via COLOR op to [-32,+32]
    int  colorG;                // +0x048
    int  colorB;                // +0x04C
    int  colorA;                // +0x050  only used when blendType==4 (alpha-blend)
    int  blendType;             // +0x054  0 NORMAL, 1 50% alpha, 2 add, 3 sub, 4 alpha-blend

    // --- physics continued (0x58..0x63) ---
    int  jumpTargetY;           // +0x058  peak / stand Y used by ProcessObjectPhysics to resolve landing
    int  flagsMisc;             // +0x05C  byte +0x5C bit 0 = mirror/facing for hit-offset math
                                //         (ProcessObjectCollisions: [edi+44h] & 1; also test in [M] sign-flip)
    u8   unknown_60[4];         // +0x060

    // --- [DS] Detect-Skill trigger→target slots (0x64..0x7B) ---
    int  dsTriggers[6];         // +0x064  6 slots: Landing, Attack-Hits, Defending,
                                //         HitTheWall, InOffset, WhileThrowDo
                                //         ProcessObjectPhysics consumes state+0x64 as "queued cancel target"

    // --- [SF] loop return + [SC] call return (0x7C..0x88) ---
    int  sfLoopCounter;         // +0x07C  [SF] current loop count
    u8   sfPad_80;              // +0x080  unaligned boundary (packed)
    u8   sfReturnPair[4];       // +0x081  ((pos<<16)|scriptId) saved by [SF]
    u8   scReturnPair[4];       // +0x085  ((pos<<16)|scriptId) saved by [SC]

    // --- hit/hurt boxes (0x89..0x128) --- 20 pointers each ---
    int  hurtboxSlots[20];      // +0x089  [FD] stores KgtScriptItem* here; 80 bytes.
                                //         ClearObjectHitboxData zeros `a1+137..a1+217`.
    int  hitboxSlots[20];       // +0x0D9  [FA] stores KgtScriptItem* here; 80 bytes.
                                //         ClearObjectHitboxData zeros `a1+217..a1+297`.

    // --- state flags / vars (0x129..0x150) ---
    u8   cancelPtrByte;         // +0x129  byte from [C] cancel-cond; cleared by ClearObjectFlag297
    u8   unknown_12A[3];        // +0x12A
    u16  childOffX;             // +0x12D  int16; written by [O] flags&0x20 (child-attach); used in
                                //         ProcessObjectCollisions when flagsA&0x20000000: parent-relative X
    u16  childOffY;             // +0x12F  parent-relative Y offset
    u16  taskVars[16];          // +0x131  16 × int16 Task variables (A..P), written/read by [V]

    // --- afterimage / identity (0x151..0x17D) ---
    u8   afterimageSlot;        // +0x151  1-based slot in g_afterimagePool[] (0 = none)
    u8   unknown_152[4];        // +0x152
    u16  playerSlotId;          // +0x156  player id 1..7; indexes g_playerSlots[] (×47851)
    u8   unknown_158[2];        // +0x158
    int  entityKind;            // +0x15A  0 = player-owned, 1..4 = sub-object kinds
                                //         (DestroyGameObject clears manage slot iff kind==1)
    int  flagsStun;             // +0x15E  stun / state bits: bits 0..1 from blockstun-exit path,
                                //         &4 = new-anim just-applied, &8 = throw bit
    u8   unknown_162[24];       // +0x162  parent-link metadata (unknown fine structure)
    int  parentPtr;             // +0x17A  parent KgtRuntimeObject* (spawner). Set by CreateAnimationObject
                                //         from g_currentObject at creation.
};
```

### Offsets cross-reference

| Offset (hex / dec) | Field | Proof |
|---|---|---|
| `+0x00 / 0`  | `status` | `CreateAnimationObject` picks first `while(*v5)` zero; `DestroyGameObject` sets `*v0 = 1` |
| `+0x08 / 8`  | `posX` | `ProcessObjectCollisions` `[edi-10h]` (edi = pool+0x18); `FindNearestOpponent` `v0[2]` |
| `+0x0C / 12` | `posY` | `ProcessObjectCollisions` `[edi-0Ch]`; `ProcessObjectPhysics` `+12` |
| `+0x10 / 16` | `groundY` | `ProcessObjectCollisions` `[edi+10h]` (compared to posY for ceiling/floor) |
| `+0x14 / 20` | `physicsFlags` | `ProcessObjectPhysics` `+20`: `&1`=facing, `&2`=jumping; `FindNearestOpponent` `v0[5]` |
| `+0x18..0x24` | `moveX..accelY` | `ProcessObjectCollisions` integration: `posX += moveX; moveX += accelX; moveY += accelY; posY += moveY` (4392b2..4392aa) |
| `+0x28 / 40` | `flagsA` | `[PS]` iteration tests `dword[+0]&0x20000000` with edi = pool+0x28 → offset 0x28 |
| `+0x2C / 44` | `scriptItemIdx` | Dispatcher reads `v21+0x2C` and writes on opcode advance |
| `+0x30 / 48` | `scriptId` | `SetObjectAnimation` writes `+48 = scriptId` |
| `+0x3C / 60` | `waitCounter` | `[I]` writes `+0x3C`; engine-bug comment at `ProcessObjectPhysics` `0x439AE5` |
| `+0x40 / 64` | `pauseTimer` | `[PS]` writes `+0x40`; physics checks `>0 countdown` |
| `+0x44..0x50` | `color*` | `[COLOR]` writes; see `0x43B445..0x43B491` |
| `+0x54 / 84` | `blendType` | `[COLOR]` writes |
| `+0x58 / 88` | `jumpTargetY` | `ProcessObjectPhysics` pick of 55705600 / 60293120; also `ProcessObjectCollisions` `[edi+40h]` |
| `+0x5C / 92` | `flagsMisc` | `ProcessObjectCollisions` `[edi+44h] & 1` (edi = pool+0x18 → offset 0x5C). Facing for hit-offset add/sub. |
| `+0x64..0x78` | `dsTriggers[6]` | `[DS]` sub-switch `jpt_43A4AA` writes one per trigger type. `ProcessObjectPhysics` consumes `+0x64` as blockstun-exit target. |
| `+0x7C` | `sfLoopCounter` | `[SF]` writes `+0x7C` with loopCount |
| `+0x81,+0x85` | `sfReturnPair,scReturnPair` | `[SF]` / `[SC]` save (scriptId)\|(pos<<16). Byte-aligned (packed) |
| `+0x89..0x128` | `hurtboxSlots / hitboxSlots` | `[FD]`/`[FA]`; `ClearObjectHitboxData` zeros both 0x50 ranges |
| `+0x129` | `cancelPtrByte` | `[C]` stores esi; `ClearObjectFlag297` sets to 0 |
| `+0x12D,+0x12F` | `childOffX, childOffY` | `[O] flags&0x20` writes; `ProcessObjectCollisions` reads movsx word at `[edi+115h]`/`[edi+117h]` (edi = +0x18) |
| `+0x131..0x150` | `taskVars[16]` | `[V]` Task-class storage at `state+0x131 + idx*2` |
| `+0x151` | `afterimageSlot` | `[AI]` stores 1-based idx; `DestroyGameObject` reads `*(v0+337)` |
| `+0x156` | `playerSlotId` | `FindNearestOpponent` reads `*v2` (v2 = pool+0x156); [PS] `[eax+12Eh]` (eax = pool+0x28) |
| `+0x15A` | `entityKind` | `DestroyGameObject`: `*(v0+346)==1` → clear manage slot; dispatcher prologue picks `v1` by `state+346` |
| `+0x15E` | `flagsStun` | `ProcessObjectPhysics` sets bits 1,2,4; `[RC]`/`[RP]` set to 0xA (blue-state) |
| `+0x17A` | `parentPtr` | `CreateAnimationObject`: `*(v5+378) = dword_7D5190` |

## `KgtPlayerRuntimeSlot` — 47851 bytes (`0xBAEB`)

One per player, stride 47851 from `g_playerSlots` (old `unk_4551E0`). Indexed
by `KgtRuntimeObject.playerSlotId` (1..7). Only the observed fields are named;
the 47289-byte `unknown_013C_BDF5` block is the character-local state table
(animation ptrs, gauge values, etc) not yet enumerated.

```c
struct __attribute__((packed)) KgtPlayerRuntimeSlot {        // size = 47851
    u8   unknown_0000[272];      // +0x0000  (up to 0x110)
    void *scriptsPtr;            // +0x0110  ptr to this player's KgtScript[] table (39 B each)
                                 //          Used by SetObjectAnimation: *(v1+272) + 39*scriptId + 32
    u8   unknown_0114[36];
    int  playerAliveFlag;        // +0x0138  FindNearestOpponent checks dword_455318 at slot+312
                                 //          (zero => skip as candidate)
    u8   unknown_013C_BDF5[47289];

    int  unknownBD5;             // +0xB9F5  referenced in [RP] (throw-reaction state write path)
    int  opponentPtr;            // +0xB9F9  active opponent ptr (null when nobody targeted)
    u8   unknown_B9FD[4];
    int  lifePercent;            // +0xBA01  0..1000; tested by [GL] Life Gauge Check
    int  specialPercent;         // +0xBA05  dispatcher-doc noted (special-gauge %)
    u8   unknown_BA09[16];
    int  throwState1;            // +0xBA19  throw state selector; tested in ProcessObjectPhysics block-exit
    u8   unknown_BA1D[20];
    int  facingFlag;             // +0xBA31  FindNearestOpponent writes 0 or 1 based on self.posX vs opp.posX
    u8   unknown_BA35[8];
    int  reactionType;           // +0xBA3D  hit-junction reaction type
    int  nearestOppPtr;          // +0xBA41  written by FindNearestOpponent (pool ptr of closest match)
    u8   unknown_BA45[8];
    int  throwState2;            // +0xBA4D  tested in ProcessObjectPhysics at +47693 (>1 => skip idle fallback)
    u8   unknown_BA51[8];
    u8   reactionBlit[6];        // +0xBA59  6-byte [R] Reaction selector (type, hit S/C/A, guard S/C)
    u8   unknown_BA5F[32];
    int  objectManageSlots[10];  // +0xBA7F  [O] opcode's 10 manage-slots (1-per-manageNo);
                                 //          DestroyGameObject clears matching ptrs (kind==1 path)
    u8   unknown_BAA7[44];
    u8   throwHitFlag;           // +0xBAD3  byte_460CB3 in ProcessObjectCollisions (&0x10 gate)
    u8   unknown_BAD4[7];
    int  throwOffsetX;           // +0xBADB  applied to grabbed opponent.posX with sign by self.facing
    int  throwOffsetY;           // +0xBADF  applied to grabbed opponent.posY
    int  timeStoppedFlag;        // +0xBAE3  [PS] sets to 1 to freeze player input
    int  frozenInputSnapshot;    // +0xBAE7  input bits captured at time-stop entry
};
```

Evidence for all fields is in the `ProcessObjectCollisions@0x439250`,
`FindNearestOpponent@0x439110`, `ResetObjectToIdle@0x439BC0`, and the
`[PS]`/`[GL]`/`[RC]`/`[RP]`/`[R]` cases of `ExecuteAnimationScript`.

## `KgtAfterimageEntry` — 1616 bytes (`0x650`)

100 entries at `g_afterimagePool` (old `dword_6075E0`). `[AI]` opcode allocates
the first entry with `active == 0`:

```c
struct __attribute__((packed)) KgtAfterimageEntry {    // size = 1616
    int   active;            // +0x000  1 = owned, 0 = free
    int   state;             // +0x004  runtime state (reset to 0 on alloc)
    void *scriptItemPtr;     // +0x008  pointer to the KgtAfterimageCmd that spawned it
    int   reserved_0C;       // +0x00C  zeroed on alloc
    u8    colorBuffer[1600]; // +0x010  400-dword color/state ring buffer
                             //         (zeroed by rep stosd ecx=0x190)
};
```

The `dword_606F90` reference in `DestroyGameObject` is a "phantom slot -1":
`dword_606F90 + 1616*slot = g_afterimagePool[slot - 1]` (since the value stored
in `KgtRuntimeObject.afterimageSlot` is 1-based). DestroyGameObject uses this
to zero the `active` flag of the owning slot when the object despawns.

## Helpers — 12 functions

All decompilations verified in IDA; where the engine-bug audit already left a
top or inline comment, this doc references it by date rather than repeating.

### `CreateAnimationObject@0x436960`
`KgtRuntimeObject *__cdecl CreateAnimationObject(int flags, int scriptId, int posX, int posY)`

Linear-scans `g_objectPool[0..1023]` for the first entry with `status == 0`
(free). On full pool, displays `aObjectOver` ("Object Over") status message and
returns the final slot (last-valid write target). On success:

1. `memset(slot, 0, 0x17C)` — zeroes everything EXCEPT the last 2 fields.
2. `slot->unknown_17C` (unnamed byte at +0x17C = 380) is set to 0.
3. `slot->status = flags`.          // `*v5 = a1`
4. `slot->scriptItemIdx = scriptId`. // `v5[1] = a2` (actually +0x04)
5. `slot->posX = posX;  slot->posY = posY;`
6. `slot->parentPtr = g_currentObject`. (spawner)

(Pre-existing top-comment flags the "creation render lags 1 frame" engine bug —
the new object's first `[I]` wait is not processed until the next tick.)

Wait, re-read: `v5[1]=a2` with v5 a `_DWORD*` means offset +4. And with
`status` at +0 being 4 bytes (`int`), `+4` is `unknown_04`. So `a2` (scriptId
param) is actually being written into `+4`, not `+0x2C` as I first thought.
**This is actually the "opcode-flags" packed-byte argument** from the [O]
caller in the dispatcher (`flags = esi[1]`, held in `a2`). Verify by cross-
referencing dispatcher's O opcode: `CreateAnimationObject(newScriptId, flags, x, y)`.
Left the param names `flags, scriptId, posX, posY` aligned with the [O]-caller
view; the struct-field mapping is:

| Param | Stored to field | Offset |
|---|---|---|
| `a1 = scriptId_or_status` | `slot->status` | `+0x00` |
| `a2 = flags`              | `slot->unknown_04` | `+0x04` |
| `a3 = posX`               | `slot->posX`   | `+0x08` |
| `a4 = posY`               | `slot->posY`   | `+0x0C` |
| (implicit) g_currentObject| `slot->parentPtr` | `+0x17A` |

### `DestroyGameObject@0x438F90`
`void DestroyGameObject()`

Reads `g_currentObject`. If `entityKind == 1` (spawned child) iterates the
spawner's `KgtPlayerRuntimeSlot.objectManageSlots[10]`, zeroing any slot that
points at this object. Then reads `afterimageSlot` — if non-zero, zeroes the
`active` flag of `g_afterimagePool[afterimageSlot - 1]`. Finally sets
`g_currentObject->status = 1` (destroyed; next `CreateAnimationObject` sweep
will reuse the slot because `while(*v5)` is false for... wait — it's false only
when status is 0, so status=1 keeps it allocated. The render layer clears
destroyed objects next frame which is where the engine-bug's "destroyed sprite
persists one frame" comes from).

### `SetObjectAnimation@0x439840`
`int __cdecl SetObjectAnimation(int scriptId)`

Calls `ClearObjectHitboxData(g_currentObject)`. If `scriptId == 0` returns 1.
Otherwise writes `g_currentObject->scriptId = scriptId`, and sets
`g_currentObject->scriptItemIdx = player.scriptsPtr[scriptId].scriptIndex`
(the 32-byte-offset field of `KgtScript`), returning 0.

### `ChangeObjectAnimation@0x4398B0`
`int __cdecl ChangeObjectAnimation(int scriptId)`

Wraps `SetObjectAnimation`: if `scriptId == current scriptId` returns 0
**without clearing `cancelPtrByte`** (inline engine-bug comment already in
IDA — "C Cancel doesn't clear"). Otherwise:

- `SetObjectAnimation(scriptId)`
- `ClearObjectFlag297(g_currentObject)` (clears cancelPtrByte)
- `ClearObjectStateSlots()` (zeros dsTriggers)
- `g_currentObject->waitCounter = 0`

Then, if `entityKind == 0` (player, not sub-object) and the object is grounded
(`posY == jumpTargetY && moveY == 0`), zeroes all 4 of `moveX, moveY, accelX,
accelY`, and zeroes the byte at +133 (scReturnPair[0]) and the dword at +124
(sfLoopCounter). Returns 1.

### `ProcessObjectPhysics@0x439950`
`int ProcessObjectPhysics()`

One-frame physics integrator + animation-transition enforcer for
`g_currentObject`. Three paths based on `physicsFlags` bits and
`dsTriggers[0]`:

1. **Jumping** (`physicsFlags & 2`): picks `jumpTargetY` from one of two
   magic-number constants (55705600 / 60293120 ≈ 3400 / 3680 in 16.16
   fixed-point) depending on facing bit. Integrates `posY` toward `jumpTargetY`
   in 0x80000-per-frame steps (±8.0 fixed).
2. **Grounded landing** (`posY >= jumpTargetY && moveY > 0`):
   snaps `posY = jumpTargetY`, zeroes kinematics, clears low 2 bits of
   `flagsStun`, then either consumes a queued `dsTriggers[0]` (==
   `state+0x64` == `+100`) as a cancel target — writing to
   `scriptId`, `scriptItemIdx`, `flagsStun |= 4`, and
   **`waitCounter = 0`** (engine-bug: dispatcher sees wait=0 same frame) —
   or just zeroes `waitCounter`.
3. **Animation selection** based on `flagsStun & 0xC`:
   - `0` → `GetIdleAnimationIndex()` → `ChangeObjectAnimation(idle)`
     → clear `flagsStun & 3`, set `flagsStun |= 4`.
   - `8` → no-op.
   - else → if `throwState2 > 1` OR idle > current scriptId, idle-fallback
     via `ChangeObjectAnimation` and zero `throwState2`.

Inline engine-bug comments already in IDA at `0x439AE5` and `0x439B41`.

### `ProcessObjectCollisions@0x439250`
`int ProcessObjectCollisions()`

Walks `g_objectPool[0..1023]` from slot 0 (edi = `pool_base + 0x18`). For each
active entry (`status == 2`) with `pauseTimer == 0` it first runs the physics
integration (`posX += moveX; moveX += accelX; ...`), then:

- **Child-attach** (`flagsA & 0x20000000`): overwrites `posX` / `posY` from
  parent's `posX/posY + (childOffX/Y << 16)`. Sign-flip via parent's
  `flagsMisc & 1`.
- **Throw bind** (`player.throwHitFlag & 0x10` AND `player.opponentPtr`): warps
  the opponent's `posX/posY` to `self.posX/posY ± player.throwOffsetX/Y` with
  sign from `self.flagsMisc & 1`.
- **Floor/ceiling clamp**: constrains `posX` to `[0x400000, 0x4C00000]` or
  `[0x320000, 0x4CE0000]` (depending on `posY >= jumpTargetY`), pushing a held
  opponent with the overflow (1/4 damping when airborne).
- **Inner collision test**: iterates `g_objectPool[i+2 .. 1023]` for active
  entries with matching facing XOR'd bit 0, compares self-hitbox[k]
  (`+0xD9 + k*4` for k=0..19) vs other's slot array at `+0xDD + k*4` for
  19 slots. **Asymmetry noted**: the dispatcher doc places hitbox at `+0xD9`
  and hurtbox at `+0x89` but this collision loop reads the defender at
  `+0xDD` (=`+0xD9 + 4`, skipping slot 0). This may be a deliberate
  self-vs-self-first-slot exclusion or it may be a genuine off-by-one —
  follow-up TODO.
- **Separation**: on AABB overlap computes X-axis separation vector and pushes
  both objects (`self.posX += sep << 14`, `other.posX -= sep << 14`),
  distributed by `posY` and `throwState1 == 0` (no-damage-held case).

### `ClearObjectHitboxData@0x439080`
`void __cdecl ClearObjectHitboxData(KgtRuntimeObject *obj)`

`ZeroMemoryBlock(obj + 217, 0x50)` — zeros `hitboxSlots[20]`.
`ZeroMemoryBlock(obj + 137, 0x50)` — zeros `hurtboxSlots[20]`.

### `ClearObjectStateSlots@0x439020`
`void ClearObjectStateSlots()`

If `g_currentObject->entityKind <= 1` (player or level-1 child), zeroes
`dsTriggers[0..5]` (`+0x64..+0x78`). Sub-objects at kind >= 2 skip.

### `ClearObjectFlag297@0x439070`
`void __cdecl ClearObjectFlag297(KgtRuntimeObject *obj)`

Single byte store: `obj->cancelPtrByte = 0` (at +0x129).

### `FindNearestOpponent@0x439110`
`int FindNearestOpponent()`

Scans `g_objectPool[0..1023]` for another object with `status == 2`,
`entityKind == 0` (player), matching `physicsFlags` (side of the stage), and
`!player.opponentPtr`, `player.playerAliveFlag != 0`. Picks the one with
minimum `|self.posX - other.posX|`. On success:

- `self.player.nearestOppPtr = &other_object`
- `self.player.facingFlag = (self.posX >= other.posX ? 1 : 0)`
- Returns `23925 * playerId` or `4785 * playerId` (both overflow-style
  per-facing stride indices used downstream — probably hit-junction table
  biases).

### `GetIdleAnimationIndex@0x439830`
`int GetIdleAnimationIndex()`

Stub — unconditionally returns 0. Real implementation likely lives in the
compiled-game binary (`WonderfulWorld_ver_0946.exe`); in the editor there is
no "idle" because the animation preview always runs a user-specified script.

### `ResetObjectToIdle@0x439BC0`
`int ResetObjectToIdle()`

Called from the LABEL_61 end-of-script path in the dispatcher prologue when
the current script runs off its last item and neither `[SF]` nor `[SC]`
returns are pending.

1. Look up the per-player blocked-input bitmask `dword_5E9300[1024*player +
   g_playerStateSelector]` → `v1`.
2. `ClearObjectFlag297(g_currentObject)` (cancel ptr).
3. `ClearObjectStateSlots()` (ds triggers).
4. Clear low 4 bits of `flagsStun`.
5. If NOT jumping (`physicsFlags & 2 == 0`):
   - Reset posX to `0x02800000` (40.0), posY to `0x03980000` (57.xx) — default
     "spawn" coords.
   - Zero all kinematics.
   - If `posY != jumpTargetY || moveY != 0`, set `flagsStun |= 2` and call
     `SetObjectAnimation(word_7DD006[player*4])` — per-player stand-up anim
     id table at 0x7DD006 (parallel array, stride 4 bytes per player, 2 words
     per entry).
   - Else zero `accelX / moveX`; if `v1 & 8` set `flagsStun |= 1`.
   - `nullsub_6()`, then `ChangeObjectAnimation(g_currentSkillIdx)`.

## Global Pools — renamed

| Addr | Old label | New name | Type | Role |
|---|---|---|---|---|
| `0x7D5190` | `dword_7D5190` | `g_currentObject` | `KgtRuntimeObject*` | Pointer to the object whose dispatcher/physics is currently executing |
| `0x775900` | `unk_775900` | `g_objectPool` | `KgtRuntimeObject[1024]` | 1024-entry object pool (linear-scan alloc) |
| `0x4551E0` | `unk_4551E0` | `g_playerSlots` | `KgtPlayerRuntimeSlot[8]` | Per-player runtime state blob (47851 B each) |
| `0x6075E0` | `dword_6075E0` | `g_afterimagePool` | `KgtAfterimageEntry[100]` | Afterimage slot pool (100 × 1616 B) |
| `0x5E9300` | `dword_5E9300` | `g_playerStateBuffer` | unchanged | Per-player 4-byte × 1024-entry state ring (input/flags bank) |
| `0x6023DC` | `dword_6023DC` | `g_playerStateSelector` | int | Index into above used by [DB], ResetObjectToIdle |

### `g_objectPool` "convenience aliases" (not separate buffers)

The compiler emits these as micro-optimized base pointers (small signed
displacement) rather than loading `g_objectPool + imm32` everywhere:

| Label | `= g_objectPool + N` | Reason |
|---|---|---|
| `unk_775918` | +0x18 | ProcessObjectCollisions — `[edi-10h..+162h]` covers fields +0x08..+0x17A |
| `unk_775928` | +0x28 | [PS] opcode — centers on `flagsA` at +0x28 |
| `unk_775A56` | +0x156 | FindNearestOpponent — centers on `playerSlotId` |

## Applied Function Signatures

| Addr | Signature |
|---|---|
| `0x436960` | `KgtRuntimeObject *__cdecl CreateAnimationObject(int flags, int scriptId, int posX, int posY)` |
| `0x438F90` | `void DestroyGameObject()` |
| `0x439020` | `void ClearObjectStateSlots()` |
| `0x439070` | `void __cdecl ClearObjectFlag297(KgtRuntimeObject *obj)` |
| `0x439080` | `void __cdecl ClearObjectHitboxData(KgtRuntimeObject *obj)` |
| `0x439110` | `int FindNearestOpponent()` |
| `0x439250` | `int ProcessObjectCollisions()` |
| `0x439830` | `int GetIdleAnimationIndex()` |
| `0x439840` | `int __cdecl SetObjectAnimation(int scriptId)` |
| `0x4398B0` | `int __cdecl ChangeObjectAnimation(int scriptId)` |
| `0x439950` | `int ProcessObjectPhysics()` |
| `0x439BC0` | `int ResetObjectToIdle()` |

## Known Gaps / Follow-ups

1. `KgtRuntimeObject.unknown_162[24]` (+354..+377): parent-link metadata —
   only offset `+354` is known so far (it *is* a parent pointer in
   `ProcessObjectCollisions`; may overlap with `parentPtr` at `+0x17A`,
   requiring a second pass).
2. `KgtRuntimeObject.unknown_60[4]` (+96..+99): unknown dword inside the
   color-mod block.
3. `KgtPlayerRuntimeSlot.unknown_013C_BDF5[47289]`: massive un-enumerated
   per-character block — presumably contains the character's complete
   KgtProjectSlot-derived state (scripts, pictures, hit junction table).
4. `GetIdleAnimationIndex` is a stub in the editor — when porting, reimplement
   based on per-player's current state (standing / crouching / aerial).
5. ProcessObjectCollisions reads defender-box array at `+0xDD` (hitbox+4) when
   the dispatcher doc's `[FD]` says hurtbox is at `+0x89`. Either the loop
   intentionally skips slot 0, or one of the two conventions is off by 4.
   Should be resolved by watching actual hit detection in the running game.

## See Also

- `docs/editor/opcode_dispatcher.md` — the `ExecuteAnimationScript@0x439CD0`
  dispatcher that owns `g_currentObject` once per frame.
- `docs/editor/ida_progress.md` — global IDA state table; adds this session's
  3 new structs + 6 new globals + 12 typed helpers.
- `Hantei-chan/han4docs/` — hit-junction / reaction-system reference (for
  `reactionBlit`, `reactionType`).
