# 2DFM Engine Bugs — CSV-to-Binary Mapping

Cross-reference between the three bug CSVs in this folder and the code sites
reverse-engineered in `KGT2nd_EDITOR.exe`. Each bug is tagged in IDA with an
`ENGINE BUG` / `SK Codeblock` / `LilithPort` comment pointing back to this doc.

The editor binary hosts the same script interpreter the runtime game uses for
test-play, so the majority of Engine-Bugs CSV entries reproduce here. The three
**Input** bugs (Command-time vs hitstop, Hold behavior, negative edge) live in
the runtime's input/command matcher — see the **Runtime-only** section at the
bottom.

Baseline references:

- Jump-table + opcode semantics: `docs/editor/opcode_dispatcher.md`
- Function / struct naming conventions: `docs/editor/ida_progress.md`
- User-facing codeblock docs: `docs/editor/2DFM Codeblocks.md`

All state offsets below are offsets from the per-entity state struct pointer
held in `dword_7D5190` (aka `g_currentEntityState`); the `v1` helper used in
decompile comments is the per-entity-kind base derived from `state+346`.

Ratings:

- **Trivial** — single-instruction patch; byte-for-byte safe.
- **Localized** — multi-instruction rewrite but within one function.
- **Invasive** — spans multiple functions and/or shared state; risk of breaking
  existing character scripts that rely on the bug.
- **Design** — semantics are correct-for-the-format; "fix" changes script
  authoring contract.

---

## Engine Bugs CSV

### 1. Delay — "Delay issues slow down the pace of character actions, stalling the reading of code blocks for 1 or more frames."

Meta description: umbrella row for the three specific delay bugs below. No
single code site; see the tagged bugs #2, #3, #4. The additional note
"*Delay bugs stack with each other, increasing delay further*" is observable
because each bug stalls the dispatcher prologue in `ExecuteAnimationScript`
before the current frame's `while(1)` item-fetch loop runs — two delays on the
same frame each consume one full 10 ms tick without incrementing
`state+0x2C`.

Patchability: n/a (meta).

---

### 2. Delay — "If a move doesn't clear the C Cancel Codeblock, the move will get delayed by X frames, where X is the command buffer."

> *"Happens because a move repeatedly tries to cancel into itself."*
> *Workaround: Add C Fail or a new C Cancel Codeblock to the beginning of moves.*

**IDA addresses**

| Addr | Fn | Role |
|---|---|---|
| `0x4398BD` | `ChangeObjectAnimation` | Root: self-check early-return — skips `ClearObjectFlag297` |
| `0x43A85E..0x43A863` | `ExecuteAnimationScript` case 0x17 `[C]` | Stores scriptItem pointer to `state+0x129` unconditionally |

**What the code does**

`ExecuteAnimationScript` case `0x17` (opcode `[C]` Cancel Condition) does a
single store:

```
mov [ebp+129h], esi      ; state+0x129 = current [C] item ptr
```

No clearing, no condition check. "Most-recent [C] wins" — each frame's [C] item
simply overwrites the slot.

When the player inputs the cancel, the cancel-target evaluator calls
`ChangeObjectAnimation(newAnimId)`. The decompile:

```c
int __cdecl ChangeObjectAnimation(int a1) {
    if ( a1 == *(dword_7D5190 + 48) )   // state+48 = current script id
        return 0;                        // <-- EARLY RETURN, no cleanup
    SetObjectAnimation(a1);
    ClearObjectFlag297(dword_7D5190);    // clears +0x129 among others
    ClearObjectStateSlots();
    *(dword_7D5190 + 60) = 0;            // state+0x3C wait counter
    ...
}
```

If the cancel target script id **equals** the currently-playing script id
(self-cancel — the classic "chain light-light-light into itself" case), the
function bails before `ClearObjectFlag297` runs, so `state+0x129` still holds
the pointer to last frame's `[C]` item. Next frame the input-matcher sees a
matching cancel-capable input (the command buffer still contains the button
press for ~X frames), re-fires the same cancel, and the loop continues until
the command buffer finally drains.

Net effect: the move's first frame gets stalled by exactly the command-buffer
length because every frame the input-matcher fires, `ChangeObjectAnimation`
short-circuits, and `state+0x2C` is never advanced.

**Workaround explained**

Adding a new `[C]` at the start of the new move overwrites `state+0x129` with a
different filter (e.g. a cancel that can't match the same input), which
prevents the re-fire. `[C Fail]` does the same by substituting a "block all
cancels" predicate.

**Patchability**: **Localized**. Two options:

1. Patch `0x4398BD`: even on self-check match, call `ClearObjectFlag297` and
   reset `state+0x2C` / `state+0x3C`. Risk: changes semantics of intentional
   self-cancels in existing scripts.
2. Patch `0x43A85E`: clear `state+0x129` to 0 when a `[C]` item re-fires with
   the same pointer already stored. Lower risk but doesn't address the
   hitbox/hurtbox slot leak on other self-cancel paths.

---

### 3. Delay — "Attacks immediately out of blockstun have the first frame duplicated, resulting in +1 Startup in most situations."

> *Workaround: Adjust blockstun as needed.*
> *"Effects both attacker and defender."*

**IDA addresses**

| Addr | Fn | Role |
|---|---|---|
| `0x439AE5..0x439AE7` | `ProcessObjectPhysics` | Writes `state+0x3C = 0` on cancel-consume |
| `0x439B41..0x439B45` | `ProcessObjectPhysics` | `ChangeObjectAnimation` call from idle-fallback |
| `0x43A0F8` | `ExecuteAnimationScript` prologue | Reads `state+0x3C` AFTER physics, sees 0, consumes frame |

**What the code does**

When an entity is in blockstun, `ProcessObjectPhysics` (`0x439950`) is called
every frame from the `ExecuteAnimationScript` prologue *before* the
script-item loop. On the frame blockstun expires, the physics function:

1. Consumes a queued cancel/reaction target from `state+0x64` (the
   blockstun-exit DS slot).
2. Calls `ChangeObjectAnimation(newAnimId)` which sets `state+0x2C =
   scriptStart(newAnim)`.
3. Writes `state+0x3C = 0` (wait-counter reset).

Control returns to the prologue at `0x43A0F8`, which reads `state+0x3C`:

```
wait = state+0x3C;
if (wait) { --state+0x3C; return; }   // decrement-and-exit path
// else fall into item-fetch loop
```

Because `state+0x3C == 0`, the dispatcher *does* enter the item-fetch loop —
but the **very first** item of the new move is an `[I]` (image) whose
`keepTime` is then *added* to the already-zero wait counter. So this frame the
new move renders its first image, but the next frame the wait counter is still
positive — meaning frame 1 of the move is shown for 2 ticks instead of 1.

The `ChangeObjectAnimation` call at `0x439B41` (the idle-fallback path) exhibits
the same pattern — when blockstun exits without a queued cancel, the entity
falls back to idle via `ResetObjectToIdle` which also touches `state+0x3C`.

**Why it effects both**: blockstun-exit timing is symmetric — when defender
leaves blockstun at frame N, attacker's recovery animation (if stopped by a
block-connection reaction) exits its reaction script at the same frame, so
both sides gain the +1 startup.

**Patchability**: **Localized**. Patch `ProcessObjectPhysics` at `0x439AE5` to
set `state+0x3C = -1` instead of 0, then let the `[I]` of the new move
overwrite the sentinel. Care needed: several other prologue paths assume
`state+0x3C == 0` means "free to dispatch this tick"; changing the sentinel
would need corresponding updates at the 3–4 other reads of `+0x3C`. A safer
patch is adding a one-shot "fresh-anim" flag that tells the dispatcher to skip
one `[I]`'s `keepTime` addition.

---

### 4. Delay — "Objects visually are delayed by 1 frame when creating/destroying them."

> *"Logic still runs, just not the accompanying visual object."*
> *Workaround: Tweak frame data with this info in mind.*

**IDA address**

| Addr | Fn | Role |
|---|---|---|
| `0x436960` | `CreateAnimationObject` | Allocates slot in `unk_775900[1024]` but never runs the new object's first-frame script |

**What the code does**

`CreateAnimationObject` is a pure allocator:

```c
_DWORD *CreateAnimationObject(int a1, int a2, int a3, int a4) {
    _DWORD *v5 = &unk_775900;
    while (*v5) { v5 += 382; if (++v4 >= 1023) { error; return v5; } }
    memset(v5, 0, 0x17C);
    v5[0]   = a1;  // scriptId
    v5[1]   = a2;  // posX
    v5[2]   = a3;  // posY
    v5[3]   = a4;  // flags
    v5[378] = dword_7D5190;  // parent entity
    return v5;
}
```

No call to `ExecuteAnimationScript`, no sprite assignment, no `[I]` processing.
The new object just sits in the array with `state+0x10 == 0` and zero fields
until the **next** global-object-tick loop picks it up, which is one frame
later. The renderer therefore draws the creator's this-frame sprite without
the child, and next frame the child's logic (which includes the creator's
this-frame hitbox/VFX intent) appears — visually offset by 10 ms.

Destruction: the corresponding destroy path zeroes the array slot, but the
already-queued sprite draw for this frame has already been emitted to the
render list, so the sprite persists one more frame after logic removes it.

**Patchability**: **Invasive**. Fixing "created-this-frame objects run this
frame" means calling `ExecuteAnimationScript` on the new object inline from
`CreateAnimationObject`, which re-enters the dispatcher while already inside
the parent's dispatch — requires reentrancy-safe state (`dword_7D5190` is a
global). A scope-limited fix can swap `dword_7D5190` to the new object, run
one tick, then restore — but this changes the render ordering contract for
all existing scripts relying on the 1-frame delay.

---

### 5. System — "Physics can still progress while Pausing. Has something to do with the frame data of images, or using objects in Pause."

> *Workaround: "Had to copy work from Wonderful World. Don't know the root issue." "No Image used at all also prevents the bug."*

**IDA addresses**

| Addr | Fn | Role |
|---|---|---|
| `0x43B020..0x43B1A2` | `ExecuteAnimationScript` case 0x1A `[PS]` | Player Stop / time-freeze dispatch |
| `0x43B0BB` | same | Root: `test dword ptr [eax], 20000000h` filter (self-side loop) |
| `0x43B18A` | same | Same filter on opponent-side loop (mirror) |

**What the code does**

`[PS]` Player Stop iterates the global 1024-slot object pool at `unk_775928`
(stride `0x17E` = 382 bytes) twice — once for self's pool, once for
opponent's. Each loop looks like:

```
0x43B0A5  cmp  dword ptr [eax-28h], 2            ; is this slot live?
0x43B0AB  mov  ecx, [eax+12Eh]                   ; obj's player-id
0x43B0B1  cmp  ecx, state+0x156                  ; match self?
0x43B0BB  test dword ptr [eax], 20000000h        ; <-- BUG FILTER
0x43B0C1  jz   skip
0x43B0C3  mov  cl, [esi+1]                        ; yourTime
0x43B0C8  mov  [eax+18h], ecx                     ; stamp pause counter
```

The `0x20000000` flag at `[eax+0]` is set by the image renderer when an
object's current frame has a valid sprite. **An object that hasn't rendered
a sprite yet this game (or any — e.g. pure-logic helper objects) never gets
this flag**, so it's skipped by the pause loop.

Confirmed by the CSV workarounds:

- *"No Image used at all also prevents the bug"* — if the user's own moves
  authoritatively have no image, the engine paths are different (author goes
  through a codeblock chain that re-ticks the logic under a different filter).
  Not a rigorous fix but observationally consistent.
- *"Copy from Wonderful World"* — WW presumably patches the filter or adds a
  secondary flag that's set by creation rather than rendering.

**Patchability**: **Trivial** — NOP the two `test dword ptr [eax], 20000000h`
+ `jz` pairs at `0x43B0BB` and `0x43B18A` (each is 6 bytes test + 2 bytes jz =
8 bytes; replace `jz` with two NOPs to make the stamp unconditional given the
earlier player-id match). Risk: scripts that intentionally use image-less
background helpers to continue running during `[PS]` will break; but the
entire point of `[PS]` is to freeze everything so this is arguably
correct-for-the-feature.

---

## SK Codeblock Bugs CSV

### 6. [RC] — "Quitting out during a 'Same' [RC] causes the opponent to start in the air."

**IDA addresses**

| Addr | Fn | Role |
|---|---|---|
| `0x43AB3E..0x43AB44` | `ExecuteAnimationScript` case 0x07 `[RC]` | Sets opponent state+0x15E=0xA (forced hitstun) |

**What the code does**

`[RC]` Change Shape's Condition (Common Image) forcibly reshapes the opponent
into a specific reaction pose. The "Same" mode uses:

- `state+0x5C` (facing-anchor) copied from attacker
- Opponent `state+0xC` (Y position) computed as `attacker.y + esi[6..7]`
  (relative offset from attacker)
- Opponent `state+0x15E = 0xA` (forced-hitstun state flag)

When a round ends **during** the `[RC]`, the attacker's `state` is reset to
idle by round-end logic, but the opponent's Y coordinate is already an
absolute world value derived from the attacker's mid-air Y. The round-end
code doesn't re-ground the opponent for the next round start, so the opponent
spawns at whatever Y the previous round's RC set.

**Patchability**: **Localized**. At round-end reset, loop the entity pool and
for any entity with `state+0x15E == 0xA` force `state+0xC = groundY(player)`.
Add 6 instructions to the existing round-reset path. Low risk.

---

### 7. [DS] — "[DS] will trigger from any object created by the entity that uses the [DS]. This includes objects that are inside another object created by that entity."

> *"Won't trigger if player itself didn't previously hit opponent. A [DS] inside an object won't trigger from a [DS] inside another object. This means putting a hitbox into an object and using a [DS] on the object itself is safe to do."*

**IDA address**

| Addr | Fn | Role |
|---|---|---|
| `0x43A4B1` | `ExecuteAnimationScript` case 0x02 `[DS]` sub 1 | Write `state+0x64` (Landing trigger) |

The tag at 0x43A4B1 documents the broader issue: the *hit-resolution* code
(not in the dispatcher — lives in the hit-junction callback reached from
`ProcessObjectPhysics`) checks the **parent player's** DS slots whenever any
object in that player's subtree registers a hit. The CSV's hierarchy
observations ("nested objects, but not grandchildren") match the following
code contract:

- A `[DS]` inside the player itself is owned by the player; any hitbox
  connected by the player or any child object invokes the player-side check.
- A `[DS]` inside an object is owned by that object; it's only checked when
  the object's own `state+342` (entity kind) equals the CURRENT entity doing
  the hit. Grandchildren have a different entity kind chain so they miss.

This is a direct consequence of the per-entity-kind `v1` base pointer
selection in the prologue (`0x439CE8..0x43A17F`) — the DS slots live on each
entity's own `+0x64..+0x78`, but the hit-junction consumer walks up to the
top-level player for DS evaluation.

**Patchability**: **Design**. Changing DS to be per-object instead of
per-player breaks the "player-aware cancel on any hit" design. Leave as-is;
document.

---

### 8. [DS] — "[DS] Throw can't trigger from inside objects."

**IDA address**

| Addr | Fn | Role |
|---|---|---|
| `0x43A4D9` | `ExecuteAnimationScript` case 0x02 `[DS]` sub 6 | Write `state+0x78` (While-throw trigger) |

**What the code does**

DS sub-type 6 ("While throw do") writes to the CURRENT entity's `state+0x78`.
However the throw-input matcher in the runtime's input subsystem reads
specifically `player->state+0x78`, not the child object's. When `[DS] throw`
is placed inside an object, the store goes to the object's state but the
consumer never reads it.

Asymmetric with other DS subtypes: sub-types 1..5 all end up on the same
player-vs-entity lookup mismatch (covered by bug #7), but sub-type 6 has an
additional asymmetry because the throw-input consumer is distinct from the
generic hit-resolution path.

**Patchability**: **Localized**. Runtime-side fix: change the throw-input
consumer to also check the player's child objects' `+0x78`. Editor-side the
right fix would be to reject `[DS] Throw` inside an object at script-compile
time (the editor's compile path doesn't currently error on this).

---

### 9. [RP] — "An opponent can think they're on the opposite side as they're supposed to be if they attempt to spawn (number slotted?) objects during hit animation an enemy's RP is involved."

> *"Really not sure the true horrors behind this one. This is hard to avoid doing, and the bug happens somewhat uncommonly. Results in the player being launched from the mirror of the RC/RP's X-Offset and/or the player not receiving corner pushback from the move."*

**IDA addresses**

Shares tag cluster at `0x43AB3E..0x43AB44` with bug #6 — the comment
explicitly notes "*Also related to the RP mirroring bug when objects spawn
during hitstun*".

**What the code does**

`[RP]` (case `0x14` at `0x43AB4A`) forces opponent into a hit-junction with
the hit-junction start address stored to `opponent->state+0x38`. The facing
flip is applied using `dword_44D8CC[flags&1]` — this LUT flips the sign of
horizontal offsets.

Object spawning via `[O]` (case `0x04`) at `0x43A5C6` in the opponent's
hit-animation checks `state+92 & 1` (facing bit) to decide whether to mirror
the spawn offset. Both `[RP]` and `[O]` modify **different** facing-adjacent
fields:

- `[RP]` → `opponent+0x5C` (RP facing-anchor)
- `[O]` → reads `opponent+0x5C` (= `state+92`) to mirror

The ordering within a frame is: prologue physics (applies RP facing updates)
→ item-fetch loop (runs `[O]` which reads the now-updated `+0x5C`). If the
RP's facing update happens in a different tick from the `[O]` read — which
can happen when the object-spawning entity is a child (its `ExecuteAnimationScript`
runs in a later position in the global tick order than the player-RP
consumer) — the read sees the pre-flip value and spawns objects at the mirror
X offset. The corner-pushback miss is a cascading consequence: pushback is
computed against the X offset that wasn't applied.

**Patchability**: **Invasive**. Either guarantee RP's facing write propagates
to all the player's descendants before any descendant ticks this frame, or
have `[O]` always read from the top-level player's `+0x5C` instead of the
current entity's. Both touch the global tick ordering.

---

## LilithPort Quirks CSV (netplay layer)

These are not engine bugs in `KGT2nd_EDITOR.exe`; they are behavioral
differences added by LilithPort (a rollback/delay-based netplay wrapper for
2DFM games that modifies inputs before they reach the engine). A rollback
netcode replacement (e.g. our GekkoNet layer) needs to make deliberate
decisions about each.

### 10. Forward-Back treated as Back

> *"Lilithport changes forward+back to be treated as back. Alters client-side fixes."*
> *"Usually fixes Walk-Block related issues in 2DFM, but means that there's not parity between online/offline experience."*

**What LilithPort does**

Intercepts the 11-bit input mask before it enters the engine's command
matcher. If both the forward and back bits are set on the same frame, it
clears the forward bit. This makes "hold back while walking forward" resolve
to "block" (back only), fixing the classic "walk through projectile" input
conflict.

**Engine-side code** (for reference — not a bug site): the runtime's input
parser treats F+B as a non-error both-bits-set, which can cause walk or block
depending on the command-matcher's order of evaluation on that frame.

**Rollback layer recommendation**

1. **Match parity**: apply the same F+B→B transform at input sample time,
   BEFORE input goes into the rollback ring buffer. Ensures input determinism
   across rollback replays and keeps offline/online parity.
2. Make this a user-configurable setting (default on) to preserve the
   LilithPort-era muscle memory for existing players.

---

### 11. Grab Turns hit-invulnerability delay

> *"You can get hit when grabbing a player facing away from you, if that player has a hitbox active that would hit the opponent after turning around."*
> *"In 2DFM, players are turned to face the opponent immediately when grabbed. This normally doesn't cause issues, but on LilithPort, the player's invulnerability updates a frame later. This makes active hitboxes hit the grab user when playing online."*

**What LilithPort does**

Input delay adds a one-frame lag between "grab lands" and "throw invuln
applied". The engine's grab state transition sets facing immediately
(`state+0x5C`), but the invuln bit (`state+0x15E` bit mask) gets its update
through a different path that only runs after the throw reaction script's
first frame — and LilithPort's input-delay buffer makes the throw reaction
script start one tick late relative to the facing flip.

**Rollback layer recommendation**

1. **Sample-time grab detection**: when both players submit inputs and one
   produces a grab that connects, apply the opponent's facing + invuln bit
   **atomically** (same tick) before dispatching `ExecuteAnimationScript`.
2. **Rollback implication**: because rollback replays inputs deterministically
   from a saved state, this is less of an issue online than with LilithPort's
   delay-only model — but the fix still requires the invuln-bit write to
   happen in the grab-detect path, not the reaction-script path.
3. Specifically: in our `FM2KHook` implementation, inject the invuln bit
   write into the grab-resolve callback before the frame's
   `ExecuteAnimationScript` dispatches any item.

---

## Runtime-only (not in KGT2nd_EDITOR.exe)

The following three Input-category bugs from the CSV are properties of the
**runtime game's** (e.g. `WonderfulWorld_ver_0946.exe`) input/command matcher,
not the editor's test-play interpreter. The editor binary has no references
to "hitstop", "input buffer", or a command-matching loop (verified via
`find_regex` — only user-facing UI strings like `"Command time (0-1023) 100
is about 1 second"` show up).

### 12. Input — "Command time doesn't account for hitstop"

> *Workaround: "Using Command Times as long as Hitstop times, or using DS attacking to account for it."*

The runtime's command-matcher decrements `cmd_time` every real tick even
during hitstop freezes. A command that needs 8 frames to complete will drop
if hitstop eats 4 of those 8 ticks. The editor exposes this via the
`Command time (0-1023)` field; the bug is in the runtime's decrement loop.

**To be mapped** when we pivot to `WonderfulWorld_ver_0946.exe`. Expected
fix: skip `cmd_time` decrement while `global_hitstop_counter > 0`.

### 13. Input — "'Hold' repeatedly fires off the move"

> *Workaround: "Use a hold into a button press to force the command list to properly fire off."*
> *"Negative edge is unsupported by the engine."*

Runtime input matcher fires the command on every frame the button state
matches, rather than only on the transition edge. Negative edge (release of
button as a command input) is unimplemented.

**To be mapped** when we pivot to the runtime binary. Expected fix: track
previous-frame input mask and only fire on 0→1 transition for press-match
commands; add a 1→0 transition handler for negative-edge.

---

## Latent issues observed

While tagging the 42 bug sites, a few adjacent issues surfaced that aren't in
the CSVs but are worth noting for the rollback port:

1. **`[PS]` double-loop mirror** — `ExecuteAnimationScript` iterates the
   1024-slot object pool twice (self + opponent) at `0x43B0A5` and `0x43B171`.
   Any fix to the `0x20000000` filter must patch **both** sites (tagged at
   `0x43B0BB` and `0x43B18A`). Trivial per-site but easy to miss.

2. **Creation-frame lag + [PS] filter interact** — an object created this
   frame won't have the `0x20000000` renderer flag yet. Combined with bug #5
   ([PS] filter) and bug #4 (creation delay), a freshly-created object is
   triply-invisible to pause: not drawn (1 frame), not paused (no image
   flag), and not dispatched this frame. Rollback state-save must still
   capture the slot even though it's renderer-invisible.

3. **`ChangeObjectAnimation` self-cancel state leak** — the early-return at
   `0x4398BD` leaks not just `state+0x129` (the bug #2 root) but also
   hit/hurtbox slot pointers at `state+0x89..state+0x128` (20 hurtbox +
   20 hitbox `KgtScriptItem*`), the reaction selector at `state+0xBA59..BA5E`
   (6 bytes), and the active `[AI]` after-image slot at `state+0x151`. Any
   rollback state-save that uses `state+0x2C` as a "script changed" marker
   will miss these leaks because `+0x2C` is not written on self-cancel.

4. **Opcode 0x29 alias** — two different editor emission paths for `[E]`
   (opcodes `0x05` and `0x29`) are distinguishable in saved project files.
   Rollback state-diffing should hash normalized opcodes (`0x29` → `0x05`)
   to avoid false-positive desync detection on old scripts.

5. **`v1` base-pointer corruption under rollback** — the prologue re-derives
   `v1` from `state+346` (entity kind) every dispatch. Any rollback replay
   that restores `dword_7D5190` but forgets `state+346` will get `v1` into a
   wrong entity-kind table (5 tables: `unk_4551E0`, `unk_5F1BC0`, `unk_4B2960`,
   `unk_602240`, or the per-slot player block). See opcode_dispatcher.md
   §"Global per-frame prologue" for the full table.

---

## Summary

| # | Bug | Fn | Addr | Patchability |
|---|---|---|---|---|
| 1 | Delay (meta) | — | — | n/a |
| 2 | C-Cancel delay | `ChangeObjectAnimation` + `ExecuteAnimationScript[C]` | `0x4398BD`, `0x43A85E` | Localized |
| 3 | +1 startup out of blockstun | `ProcessObjectPhysics` | `0x439AE5`, `0x439B41` | Localized |
| 4 | Object 1-frame visual delay | `CreateAnimationObject` | `0x436960` | Invasive |
| 5 | Physics during pause | `ExecuteAnimationScript[PS]` | `0x43B0BB`, `0x43B18A` | Trivial |
| 6 | Same-RC mid-quit airborne | `ExecuteAnimationScript[RC]` | `0x43AB3E` | Localized |
| 7 | [DS] from any child object | `ExecuteAnimationScript[DS]` | `0x43A4B1` | Design |
| 8 | [DS] Throw blocked inside object | `ExecuteAnimationScript[DS]` sub 6 | `0x43A4D9` | Localized |
| 9 | [RP] object-spawn mirror | `ExecuteAnimationScript[RC]/[RP]` | `0x43AB3E` | Invasive |
| 10 | F+B → B (LilithPort) | input sampler | — | Rollback-layer decision |
| 11 | Grab turn invuln delay | runtime grab-resolve | — | Rollback-layer decision |
| 12 | Command time vs hitstop | runtime cmd-matcher | — | TBD (WW binary) |
| 13 | Hold re-fires / no negative edge | runtime cmd-matcher | — | TBD (WW binary) |

Tagged-comment count: **43** (42 original + 1 added for `[PS]` mirror loop at
`0x43B18A`).
