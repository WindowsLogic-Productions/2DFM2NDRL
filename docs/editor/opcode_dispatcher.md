# ExecuteAnimationScript@0x439CD0 — Opcode Dispatcher Reference

The editor's runtime script interpreter for animation preview / test-play. Takes
one `KgtScriptItem*` per dispatch (via `esi`) and executes one frame of game
logic for the current entity. Called from the per-frame loop over the currently
playing script.

## Overview

- **Entry**: `0x439CD0` (IDA label `ExecuteAnimationScript`)
- **Size**: ~7 KB / ~2000 instructions
- **Signature (inferred)**: `char __usercall(unsigned int entity_arg@<esi>)` — the
  function is compiled with register-parameter calling convention. Only the
  first byte of every `KgtScriptItem` (`*esi` inside the inner loop) is used as
  the opcode.
- **Entity state pointer chain**: the top-level state pointer is the global
  `dword_7D5190` (renamed below `g_currentEntityState`). At entry the code
  picks a second base pointer `v1` (different per entity type) out of five
  candidate buffers, indexed by `*(dword_7D5190 + 342)` (entity slot id /
  player index):
  - `&unk_4551E0 + 47851 * slot` — player-owned entity state blocks (ranges 0,1)
  - `&unk_5F1BC0` — system-owned entity
  - `&unk_4B2960` — stage-owned entity
  - `&unk_602240` — demo/menu-owned entity
- **Jump table**: `jpt_43A17F` at `0x43B818`, 42 entries (cases `0x00..0x29`).
- **Inner dispatch site**: `0x43A17F`  (`jmp ds:jpt_43A17F[eax*4]`), where `eax`
  has already been set to `*v22` (the opcode byte of the current script item).

## Global per-frame prologue (0x439CE8..0x43A17F)

Before the inner switch, the prologue does:

1. Pick entity state base `v1` based on `*(state+346)` (entity kind: 0..4).
2. If this is the first ever tick (`state+338 == 0`) run the INIT PATH at
   `0x43B597` (see comment there) to seed default hit-reaction params based on
   `state+342` (player slot id 1..7), then return.
3. Process pending-script-switch slot (`state+56`).
4. Handle pause/timer (`state+64`: -1 stall, >0 countdown, 0 = execute).
5. If the entity is not a sub-object (`state+346 == 0`) tick movement/physics
   (`FindNearestOpponent` + `ProcessObjectPhysics`) **before** reading any
   script items this frame.
6. Execute script items in a `while(1)` loop, fetching
   `v22 = *(v1 + 69) + 16 * (state + 44)` (where `state+44` is the item index),
   then switch on `*v22`.

## Opcode Table

Offsets below are relative to the `KgtScriptItem` (i.e. `esi[N]` in the
disassembly, `*(v22 + N)` in the decompile).

Base state structure offsets (referenced repeatedly):

| Offset (dec) | Meaning (inferred) |
|---|---|
| `+0x2C` (44) | current script-item buffer index |
| `+0x30` (48) | current script id |
| `+0x3C` (60) | wait countdown (fixed-point) |
| `+0x40` (64) | pause/time-stop counter |
| `+0x44..0x50` | RGBA color-mod state |
| `+0x54` | blend type |
| `+0x64..0x78` | [DS] 6 trigger→target slots |
| `+0x7C..0x80` | [SF] loop counter + return address |
| `+0x81..0x84` | [SF] saved return pair (scriptId\|pos<<16) |
| `+0x85..0x88` | [SC] saved return pair |
| `+0x89..0xD8` | 20 × `KgtScriptItem*` — hurtbox slots |
| `+0xD9..0x128` | 20 × `KgtScriptItem*` — hitbox slots |
| `+0x129` | active [C] cancel scriptItem ptr |
| `+0x131..0x150` | 16 × int16 Task variables |
| `+0x151` | active [AI] after-image slot index (1-based, 0 = none) |
| `+0x156` | player/slot id (1..7, duplicates `state+342`) |
| `+0x15A` | entity kind (0 player, 1..4 subtypes) |
| `+0x15E` | stun/flag bits |
| `v1 + 47605..47841` | player-specific blob (47677 = reaction type, 47617 = life %, 47621 = special %, 47705..= [R] reaction, 47743+n*4 = object manage-slot ptrs, 47827..= throw state) |

### Complete opcode map

| Opcode | Codeblock | C++ struct | Case address | Branch target in disasm | Helpers called | Semantics |
|---|---|---|---|---|---|---|
| `0x00` | — (START/NOP) | `KgtStageStart` (only in stage scripts) | `0x43B818` entry 0 | `0x43B583` (LABEL_316) | — | No-op; just advance buf idx. In a player script this is the implicit header. |
| `0x01` | `[M]` Movement | `KgtMoveCmd` | `0x43B818` entry 1 | `0x43A3BC` | — | accelX=esi[1..2], moveX=esi[3..4], moveY=esi[5..6], accelY=esi[7..8], flags=esi[9]. flags &1 add-vs-replace, &2/&4/&8/&0x10 ignore moveX/moveY/accelX/accelY. Values multiplied by `dword_7D7C48` (X scale = horiz per-frame) / `dword_60222C` (Y scale). Sign flipped when `state+92 & 1` (facing). Writes `state+0x18/+0x1C/+0x20/+0x24` (moveX, moveY, accelX, accelY). Proof: `0x43A3BC..0x43A47E`. |
| `0x02` | `[DS]` Detect Skill (**MISSING FROM HK ENUM**) | none | `0x43B818` entry 2 | `0x43A483` | — | esi[1] selects sub-type 1..6 (Landing, Attack Hits, Defending, Hit the Wall, in offset, While throw do). Builds `target = (esi[4]<<16) \| esi[2..3]` (scriptId+pos packed). Sub-switch `jpt_43A4AA` at `0x43A4AA` writes the target into one of `state+0x64..+0x78` (one DS slot per trigger type). These are consumed by physics/collision callbacks: if the trigger fires, the object jumps to the stored target. Proof: `0x43A483..0x43A4DC`. |
| `0x03` | `[S]` Sound | `KgtPlaySoundCmd` | `0x43B818` entry 3 | `0x43AD6A` | `DispatchSoundPlayback` | esi[2..3] = soundIdx. Gated by `dword_62ED48` (sound enabled flag). Calls `DispatchSoundPlayback(slot->soundHeaders + 42*idx)`. Proof: `0x43AD6A..0x43AD95`. |
| `0x04` | `[O]` Object | `KgtObjectCmd` | `0x43B818` entry 4 | `0x43A5C6` | `CreateAnimationObject` | esi[1]=flags, esi[2..3]=targetScriptId, esi[4]=targetPos, esi[5..6]=targetScriptIdIfExists, esi[7]=targetPosIfExists, esi[8..9]=posX, esi[10..11]=posY, esi[12]=manageNo (0..9), esi[13]=layer. Skipped entirely on sub-object (`state+346 >= 2`) or with flags&4 uncond. Looks up existing object in `v1[manageNo*4 + 47743]`. If present → redirect to 'It's Out' target. Otherwise calls `CreateAnimationObject` and tags it with flags: `&0x40` = absolute (no inherit XY inversion), `&3` = layer mode (0=10, 1=127, 2=esi[13]), `&4` bypass manage, `&8` shadow (sets bit 31 in flags), `&0x20` child-attach (sets bit 29 and stores offsets at AnimObj+0x12D/0x12F). Proof: `0x43A5C6..0x43A81E`. |
| `0x05` | `[E]` End | — | `0x43B818` entry 5 | `0x43A186` | `DestroyGameObject` | If sub-object (`state+346 != 0`) → `DestroyGameObject` and return. Else falls to LABEL_61 end-of-script handling (which either returns via [SC]/[SF] flag or resets to idle). Proof: `0x43A186..0x43A18E`. Case 0x29 aliased to this — see below. |
| `0x06` | — | — | `0x43B818` entry 6 | `0x43B583` (LABEL_316) | — | No-op advance. Reserved/unused opcode slot. |
| `0x07` | `[RC]` Change Shape's Condition (Common Image) (**MISSING FROM HK ENUM**) | none | `0x43B818` entry 7 | `0x43A936` | `ClearObjectHitboxData` | Target = opponent (`v1+47609`). esi[1] flags, esi[2] = commonImageIdx, esi[4..5]=posX offset, esi[6..7]=posY offset. Flips opponent facing vs self via `dword_44D8CC[]` lookup (flags&1 toggles swap). Writes `opp+0x4` facing, `opp+0x8` x, `opp+0xC` y. Then sets `opp+0x3` (picture index) from `word_45A1AA + 47851*player + 6*commonImageIdx`, masked to 13 bits; the upper bits OR in `flags & 0xC` (Turn X / Turn Y mirrored). Sets opponent to blue-state (`+0x15E = 0xA`) and `+0x10 = -1`. Proof: `0x43A936..0x43AB45`. Skipped if self is sub-object, or opponent is missing, or esi[2]==0. |
| `0x08` | — (**undefined**) | — | `0x43B818` entry 8 | `0x43B56B` (default) | `nullsub_5` | Debug: "The Script of expectattion is not called" (color 0xAFAFFF). |
| `0x09` | `[SF]` Loop | `KgtLoopCmd` | `0x43B818` entry 9 | `0x43A4E1` | — | esi[1]=loopCount, esi[2..3]=targetScriptId, esi[4]=targetPos. Both must be nonzero. Saves return as `state+0x81 = ((state+0x2C - scriptStart(curScript)) << 16) \| curScriptId`. Writes `state+0x7C = loopCount`, `state+0x7D = ((esi[4]<<16) \| esi[2..3])` (target as packed dword). Sets `state+0x30 = targetScriptId`, `state+0x2C = scriptStart(target) + esi[4] - 1`. Only one return flag per entity — both [SC] and [SF] share the lifecycle (see LABEL_61 at `0x43A194`). Proof: `0x43A4E1..0x43A57B`. |
| `0x0A` | `[SG]` GoTo / shared-jump tail | `KgtJumpCmd` | `0x43B818` entry 10 | `0x43B39A` | — | esi[1..2]=jumpId, esi[3]=jumpPos. If jumpId==0 → no-op. Else `state+0x30 = jumpId`, `state+0x2C = scriptStart(jumpId) + jumpPos - 1`. Many conditional opcodes fall through into this tail (e.g. [V] at LABEL_293). Proof: `0x43B39A..0x43B3D9`. |
| `0x0B` | `[SC]` Call | `KgtJumpCmd` | `0x43B818` entry 11 | `0x43A580` | — | esi[1..2]=jumpId, esi[3]=jumpPos. Saves return to `state+0x85` (separate slot from [SF] at +0x81). Sets `state+0x30=jumpId` and falls through to the [SG] tail. Proof: `0x43A580..0x43A5C1`. |
| `0x0C` | `[I]` Image / Wait | `KgtShowPic` | `0x43B818` entry 12 | `0x43A376` | — | esi[1..2] = keepTime (frames to wait). `state+0x10 = -1` (invalidate sprite-ready flag). If keepTime==0 → `state+0x3C = -1` (infinite wait). Else `state+0x3C += keepTime * dword_602230` (frames-per-unit scale). The `idxAndFlip`, `offsetX/Y`, `fixDir` fields are consumed separately by the renderer — here only the wait timer is advanced. Proof: `0x43A376..0x43A3B7`. |
| `0x0D` | — (**undefined**) | — | `0x43B818` entry 13 | `0x43B56B` | `nullsub_5` | Debug error. |
| `0x0E` | `[EB]` Background Effect (**MISSING FROM HK ENUM**) | none | `0x43B818` entry 14 | `0x43AD9A` | — | esi[1..5] = R/G/B/A + blend type packed (type=esi[1], R=esi[2], G=esi[3], B=esi[4], A=esi[5]). esi[6..7] = duration. esi[8] = target bitmask: &1 self/player, &2 opponent, &4 system-wide (`dword_602381..0x6023A9`), &8 BG-stage (`dword_601B54..0x601B7C`). Each target block stores: color components, current RGBA(+0x18..0x24), duration (+0x28), remaining (+0x14). Then Shake-BG params at esi[9..11] (X mode/dur/shake) → `dword_6023AD..0x6023BD` and esi[12..14] (Y mode/dur/shake) → `dword_6023C1..0x6023D1`. Proof: `0x43AD9A..0x43B01B`. |
| `0x0F`, `0x10` | — | — | `0x43B818` entries 15, 16 | `0x43B583` (LABEL_316) | — | No-op / reserved. |
| `0x11` | `[GL]` Life Gauge Check (**MISSING FROM HK ENUM**) | none | `0x43B818` entry 17 | `0x43AC8C` | `nullsub_5`, `GetIdleAnimationIndex` | esi[5] & 1 selects whenLittle (true: fail if life > threshold) vs whenAlot (fail if life < threshold). esi[6..7] = threshold (0..1000 per user docs). esi[2..3] = jumpId (target if failed), esi[4] = jumpPos. Reads life from `v1+0xBA01` (=47617). If condition met falls through. If failed, calls `nullsub_5(&unk_44D8D4, 0xAFAFFF)` — this is "死んだね" debug path — then jumps to target. If target script index becomes 0 (end-of-life), invokes `GetIdleAnimationIndex()` and jumps to the idle animation at position 0. Proof: `0x43AC8C..0x43AD65`. |
| `0x12`, `0x13` | — (**undefined**) | — | `0x43B818` entries 18, 19 | `0x43B56B` | `nullsub_5` | Debug error. |
| `0x14` | `[RP]` Change Skill (Partner: Script Mod) (**MISSING FROM HK ENUM**) | none | `0x43B818` entry 20 | `0x43AB4A` | `ClearObjectHitboxData` | Same shape as [RC] but reads from the **hit-junction** table (`word_459E8A + 47851*player + 4*junctionIdx`) rather than the common-image table. esi[1]=flags (bit 0 Turn X), esi[2]=junctionIdx, esi[4..5]=posX, esi[6..7]=posY. Sets opponent to blue-state (`+0x15E = 0xA`, `+0x40 = 0`, `+0x38 = junctionStartAddr`) — this slams them into a hitstun using the specified hit-junction (reaction). Proof: `0x43AB4A..0x43AC87`. |
| `0x15` | — | — | `0x43B818` entry 21 | `0x43B583` (LABEL_316) | — | No-op / reserved. |
| `0x16` | `[DB]` Basic Divergence (**MISSING FROM HK ENUM**) | none | `0x43B818` entry 22 | `0x43A869` | — | esi[1] & 1 = invert flag (0=formed, 1=failed), &2 = 'its not' (skip test, always use inverse of invert flag). esi[7] selects sub-type 1..8 (Guarding, Standing, Crouching, Forward tapped, Back tapped, Up tapped, Down tapped, Guarding-combo). Reads state from `dword_5E9300[player*1024 + dword_6023DC]`. esi[2..3]=jumpId, esi[4]=jumpPos. Proof: `0x43A869..0x43A931`. Matches user-doc table exactly. |
| `0x17` | `[C]` Cancel Condition (**MISSING FROM HK ENUM**) | none | `0x43B818` entry 23 | `0x43A85E` | — | Single store: `state+0x129 = esi` (keep pointer to this item). The cancel-check logic (Fail/Hit/Uncod, Level-range vs Skill-reference) is consulted elsewhere when a cancel-capable input arrives. Most-recent [C] wins. Proof: `0x43A85E..0x43A864`. |
| `0x18` | `[FD]` Defense Frame / Hurtbox (**MISSING FROM HK ENUM**) | none | `0x43B818` entry 24 | `0x43A823` | — | esi[9] = M.Number (0..19 — hurtbox identifier). Stores `esi` pointer into `state[0x89 + M.Number*4]`. Degenerate boxes (width esi[5]==0 OR height esi[7]==0) write 0 instead (clear slot). Proof: `0x43A823..0x43A859`. |
| `0x19` | `[FA]` Attack Frame / Hitbox (**MISSING FROM HK ENUM**) | none | `0x43B818` entry 25 | `0x43A831` | — | esi[9] = M.Number (0..19 — hitbox identifier). Stores `esi` pointer into `state[0xD9 + M.Number*4]`. Same zero-on-degenerate behavior as [FD]. Proof: `0x43A831..0x43A859` (shared with case 24 after the LEA). |
| `0x1A` | `[PS]` Player Stop / Time Stop (**MISSING FROM HK ENUM**) | none | `0x43B818` entry 26 | `0x43B020` | — | esi[1] = yourTime (0..255 frames), esi[2] = hisTime. Skips if self is sub-object. Writes `state+0x40 = yourTime`, stores current input-bank snapshot to `dword_460CC7 + 47851*player`, sets `dword_460CC3 + 47851*player = 1`. Then iterates **all 1024 active game objects** (`&unk_775928`, stride 0x17E = 382 bytes) and for each that matches this player (offset +302) and has the "paused-by-stop" bit (0x20000000 at offset 0), stamps its pause-counter (+0x18) with yourTime. Then does the same for the opponent with hisTime. Proof: `0x43B020..0x43B1A2`. |
| `0x1B`, `0x1C`, `0x1D` | — | — | `0x43B818` entries 27, 28, 29 | `0x43B583` (LABEL_316) | — | No-op / reserved. |
| `0x1E` | `[R]` Reaction (**MISSING FROM HK ENUM**) | none | `0x43B818` entry 30 | `0x43B1A7` | — | 6-byte blit: `*esi` (4 bytes) → `v1+0xBA59` (=47705), `esi[4..5]` (2 bytes) → `v1+0xBA5D` (=47709). Represents the 6 hit-junction selectors for the current move: {type_byte, hitStand, hitCrouch, hitAir, guardStand, guardCrouch} plus one trailing byte — this is a compact "active reaction-set" ref consumed by the hit-resolution code when this entity's hitbox connects. Proof: `0x43B1A7..0x43B1BD`. |
| `0x1F` | `[V]` Variables | `KgtVariableCmd` | `0x43B818` entry 31 | `0x43B1C2` | — | Complex. esi[4] = variable selector byte: top 2 bits (`>> 6`) select storage class: `00` Task (`state+0x131 + idx*2`), `01` Char (`v1+0xBA5F + idx*2`), `10` System (`dword_601B34 + idx*2`), `11` Data constants (read-only — sub-switch `jpt_43B267` cases 0..7: own X coord, own Y coord, dword_607420 (map X), dword_607424 (map Y), parent's X, parent's Y, round time `dword_7758F0/100`, round number `dword_7758E4`). Low 6 bits = variable index (0..15 for A..P). esi[5] operation flags: `&3` op(0 none, 1 assign, 2 add), `&0xC` compare cond (0 none, 4 equals, 8 greater, 0xC less), `&0x80` compare operand is another var (read via esi[6] selector) vs immediate (esi[7..8] signed). esi[9..10] = compareValue. On compare-fail falls through; on compare-pass (or no-compare) falls into LABEL_293 / [SG] tail using `esi[1..2]=jumpId`, `esi[3]=jumpPos`. Values are clamped to signed int16 range [-30000, 30000] per `0x43B318..0x43B33B`. Proof: `0x43B1C2..0x43B3D9`. |
| `0x20` | `[R]` Random (user-doc name) | `KgtRandomCmd` | `0x43B818` entry 32 | `0x43B3DE` | `_rand` | esi[1..2] = totalNumbers (0..65535), esi[3..4] = moreThanVal, esi[6..7] = targetScriptId, esi[8] = targetPos. Rolls `rand() % (total+1)`. If result > moreThanVal AND target nonzero, jump to target. Note: the HK enum also calls this `RANDOM` — same thing. Proof: `0x43B3DE..0x43B440`. |
| `0x21`, `0x22` | — (**undefined**) | — | `0x43B818` entries 33, 34 | `0x43B56B` | `nullsub_5` | Debug error. |
| `0x23` | `[COLOR]` Color Modification | `KgtColorSetCmd` | `0x43B818` entry 35 | `0x43B445` | — | esi[1] = colorBlendType (0..4: NORMAL, 50+50% alpha, add, subtract, alpha-blend). esi[2..4] signed int8 R/G/B (-32..+32 per user doc). esi[5] signed int8 alpha — only applied when blendType==4; else alpha forced 0. Writes `state+0x54` (blendType), `+0x44/+0x48/+0x4C/+0x50` (R/G/B/A). Proof: `0x43B445..0x43B491`. |
| `0x24` | — (**undefined**) | — | `0x43B818` entry 36 | `0x43B56B` | `nullsub_5` | Debug error. |
| `0x25` | `[AI]` After Image | `KgtAfterimageCmd` | `0x43B818` entry 37 | `0x43B496` | `nullsub_5`, `memset` | Manages entries in the global after-image pool `dword_6075E0..` (100 slots × 0x650 bytes). If self already owns a slot (`state+0x151 != 0`): esi[3]==0 or esi[4]==0 means "turn it off" — clears slot and zeros `state+0x151`. Otherwise allocates the first free slot (sets `[slot] = 1`, stores 1-based index into `state+0x151`). On full pool, prints "AfterI Scope Over" in 0xFFFF44. On successful allocation: `dword_6075E4 = 0` (state), `dword_6075E8 = esi` (scriptItem ptr), `dword_6075EC = 0`, zeroes 0x640 bytes of color buffer at `unk_6075F0 + 1616*slot`. Fields of `KgtAfterimageCmd` consumed by the render/tick code: `afterimageMaxCount` (esi[3]), `afterimageGap` (esi[4]), `colorBlendType` (esi[5]), `afterimageColorType` (esi[6]), R/G/B/A (esi[7..10]). Proof: `0x43B496..0x43B566`. |
| `0x26`, `0x27`, `0x28` | — (**undefined**) | — | `0x43B818` entries 38, 39, 40 | `0x43B56B` | `nullsub_5` | Debug error. |
| `0x29` | `[E]` End (alias) | — | `0x43B818` entry 41 | `0x43A186` | `DestroyGameObject` | Shares code with `case 0x05`. This alias exists because the editor's END codeblock can emit either value depending on history; the runtime treats them identically. |

## Opcodes **NOT** in the HK dev's `CommonScriptItemTypes` enum

The enum in `2dfm/2dfmScriptItem.hpp` lists: START=0, MOVE=1, SOUND=3, OBJECT=4,
END=5, LOOP=9, JUMP=10, CALL=11, PIC=12, VARIABLE=31, RANDOM=32, COLOR=35,
AFTERIMAGE=37.

The editor dispatcher handles **all** of those, but also 11 additional opcodes
that the HK port appears to have missed (at least — they're not in the enum,
though some may be handled elsewhere in the runtime):

| Opcode | Codeblock letter | Semantics | Notes for HK port |
|---|---|---|---|
| `0x02` | `[DS]` | Detect Skill divergence | Needs `KgtDetectSkillCmd { type; uint16 targetScriptId; uint8 targetPos; uint8 subType; }` — 1..6. |
| `0x07` | `[RC]` | Change Common-Image on opponent | Needs hit-junction address table and partner facing logic. |
| `0x0E` | `[EB]` | Background effect (palette flash + shake) | 16-byte item fully used; target-mask lets one codeblock affect multiple entities. |
| `0x11` | `[GL]` | Life-gauge threshold check + idle-fallback | Reads life from player state, not entity state. |
| `0x14` | `[RP]` | Change opponent skill (partner hit junction) | Identical shape to RC but different table. |
| `0x16` | `[DB]` | Basic Divergence (button-state conditional) | 8 sub-conditions; reads global input buffer. |
| `0x17` | `[C]` | Cancel condition hookup | Just stores pointer; consumption happens at input-handling layer. |
| `0x18` | `[FD]` | Register hurtbox slot | 20 slots per entity. |
| `0x19` | `[FA]` | Register hitbox slot | 20 slots per entity. |
| `0x1A` | `[PS]` | Time-stop / player pause | Iterates global object pool to pause dependents. |
| `0x1E` | `[R]` | Reaction reference | 6-byte selector → triggers `KgtReactionItem` lookup on hit. |

## Opcodes observed but **unreachable** in valid scripts

`0x06`, `0x0F`, `0x10`, `0x15`, `0x1B`, `0x1C`, `0x1D` all jump to LABEL_316
(no-op advance). These are likely reserved for future codeblocks or are stubs
for removed features.

`0x08`, `0x0D`, `0x12`, `0x13`, `0x21`, `0x22`, `0x24`, `0x26`, `0x27`, `0x28`
all hit the default case and print a debug message. These are "invalid opcode"
slots.

## Codeblock-letter-to-opcode cheat sheet

| User-doc letter | Hex | Decimal | HK enum name |
|---|---|---|---|
| (implicit header) | `0x00` | 0 | `START` |
| `[M]` | `0x01` | 1 | `MOVE` |
| `[DS]` | `0x02` | 2 | *(missing)* |
| `[S]` | `0x03` | 3 | `SOUND` |
| `[O]` | `0x04` | 4 | `OBJECT` |
| `[E]` | `0x05` | 5 | `END` |
| `[RC]` | `0x07` | 7 | *(missing)* |
| `[SF]` | `0x09` | 9 | `LOOP` |
| `[SG]` | `0x0A` | 10 | `JUMP` |
| `[SC]` | `0x0B` | 11 | `CALL` |
| `[I]` | `0x0C` | 12 | `PIC` |
| `[EB]` | `0x0E` | 14 | *(missing)* |
| `[GL]` | `0x11` | 17 | *(missing)* |
| `[RP]` | `0x14` | 20 | *(missing)* |
| `[DB]` | `0x16` | 22 | *(missing)* |
| `[C]` | `0x17` | 23 | *(missing)* |
| `[FD]` | `0x18` | 24 | *(missing)* |
| `[FA]` | `0x19` | 25 | *(missing)* |
| `[PS]` | `0x1A` | 26 | *(missing)* |
| `[R]` (Reaction) | `0x1E` | 30 | *(missing)* |
| `[V]` | `0x1F` | 31 | `VARIABLE` |
| `(R)` (Random) | `0x20` | 32 | `RANDOM` |
| `[COLOR]` | `0x23` | 35 | `COLOR` |
| `[AI]` | `0x25` | 37 | `AFTERIMAGE` |
| `[E]` (alias) | `0x29` | 41 | `END` |

## Codeblocks from user-doc that were **NOT** seen in this dispatcher

These exist in `2DFM Codeblocks.md` but do not appear to have cases in
`ExecuteAnimationScript`. They may be handled in a different runtime function
(e.g. command-input matching, gauge-modification, or partner-event dispatch
paths):

- `[FA]` vs `[R]` confusion — user doc's `[R]` is **Reaction** (opcode 0x1E);
  but `(R)` (parenthesized) is **Random** (opcode 0x20). Both mapped above.
- `[GS]` Special Gauge Check — analogous to `[GL]` but reads v1+47621
  (special %). **Not found in this dispatcher** — either coalesced with `[GL]`
  via a flag, or handled in a separate opcode not in the 0x00..0x29 range, or
  lives in a different dispatcher. **TODO: UNKNOWN.**
- `[GC]` Change Gauge Value — gauge delta. **Not found. TODO: UNKNOWN.**
- `[COM]` Command Divergence — input-matching jump. Likely handled in the
  input subsystem, not the animation dispatcher. **TODO: UNKNOWN.**

## Helpers called (verified)

All of these are pre-existing names; none are opcode-exclusive so no renames
were performed:

| Function | Address | Used from opcode |
|---|---|---|
| `ChangeObjectAnimation` | `0x4398B0` | prologue only |
| `ClearObjectHitboxData` | `0x439080` | prologue, `[RC]`, `[RP]` |
| `ClearObjectFlag297` | `0x439070` | prologue, `[RC]` |
| `ClearObjectStateSlots` | `0x439020` | prologue, end-of-script |
| `DestroyGameObject` | `0x438F90` | prologue, `[E]` |
| `FindNearestOpponent` | `0x439110` | prologue |
| `ProcessObjectPhysics` | `0x439950` | prologue |
| `ResetObjectToIdle` | `0x439BC0` | prologue (LABEL_61) |
| `CreateAnimationObject` | `0x436960` | `[O]` |
| `GetIdleAnimationIndex` | `0x439830` | `[GL]` |
| `SetObjectAnimation` | `0x439840` | init path |
| `DispatchSoundPlayback` | `0x404860` | `[S]` |
| `ZeroMemoryBlock` | `0x4034B0` | init path |
| `nullsub_5` | `0x439010` | `[GL]`, `[AI]`, default |
| `_rand` | `0x43CFD9` | `(R)` |

No helpers are unique enough to justify a per-opcode rename.

## Verification notes

- Jump table parsed directly from bytes at `0x43B818` (42 × dword, 168 bytes);
  every listed case address was confirmed by its address appearing in the
  table **and** by the IDA-generated `jumptable 0043A17F case N` comment at
  that address.
- Each opcode's semantics were verified by reading the disassembly at the
  target label (cited in the "Branch target" column) and matching the
  field-offset reads on `esi` (= `KgtScriptItem*`) against the struct layouts
  in `2dfm/2dfmScriptItem.hpp` and the user-facing field tables in
  `docs/editor/2DFM Codeblocks.md`.
- **All opcode dispatch sites in IDA have been tagged with `set_comments`**
  covering the case address, the codeblock letter, the struct name (where
  one exists), and the per-byte field layout. See each case's `0x43Axxx` or
  `0x43Bxxx` address in IDA for the inline reference.

## Summary / Findings

The HK dev's `CommonScriptItemTypes` enum is **substantially incomplete**: it
names 13 opcodes, but the dispatcher implements 25 opcodes (14 of which are
gameplay-essential — hitboxes, hurtboxes, cancel, DS, DB, GL, EB, RC, RP, PS,
R, plus the END alias 0x29). Anyone porting this engine using only the HK
headers will miss all hitbox/hurtbox/reaction logic. A C++ port should add at
minimum:

```
DETECT_SKILL   = 2,   // [DS]
COMMON_IMAGE   = 7,   // [RC]
BG_EFFECT      = 14,  // [EB]
LIFE_CHECK     = 17,  // [GL]
PARTNER_SKILL  = 20,  // [RP]
BASIC_BRANCH   = 22,  // [DB]
CANCEL_COND    = 23,  // [C]
HURTBOX        = 24,  // [FD]
HITBOX         = 25,  // [FA]
PLAYER_STOP    = 26,  // [PS]
REACTION_REF   = 30,  // [R]
END_ALIAS      = 41,  // [E] second form
```

plus struct definitions for each (`KgtDetectSkillCmd`, `KgtCommonImageCmd`,
`KgtBgEffectCmd`, etc. — the editor's 16-byte `KgtScriptItem` payloads are
fully documented per-opcode in the table above).

Three codeblocks from the user doc (`[GS]`, `[GC]`, `[COM]`) are **not** in
this dispatcher's switch. They likely live in the input/gauge subsystems; that
is out of scope for this agent's reverse.
