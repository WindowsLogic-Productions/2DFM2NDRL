# Hit-Junction & Reaction System

Reverse-engineered walk-through of the editor windows, data layouts, and
runtime bindings that make up 2DFM/Fighter-Maker-2nd's "hit junction" /
reaction system. All IDA addresses are from `KGT2nd_EDITOR.exe`.

Prerequisite reading:

- `docs/editor/2DFM Codeblocks.md` sections `[FA]`, `[FD]`, `[R]`, `[DS]`.
- `docs/editor/ida_progress.md` (structs and globals already declared).
- `2dfm/2dfmFile.hpp` lines 18-26 (`ReactionItem`, `ThrowReaction`).
- `2dfm/KgtPlayer.hpp` (`HurtBindInfo`, `ThrowActionInfo`).

## 1. What a "Hit Junction" actually is

A **hit junction** (inside the editor also called a *reaction* or
*受击反应*) is a named logical category of "what happens when somebody
gets hit/guards this move". It is NOT a script itself; it is a label that
acts as an ID into a table.

There are three independent tiers:

| Tier | Scope | Count |
|---|---|---|
| Reaction catalogue | Project-global. Shared by all characters. | 200 (`KgtReactionItem`, 36 B each) |
| Hurt-bind table | Per-character. Maps each reaction-ID to the character's local "hurt-animation script" + "hit-spark script". | 200 × 4 B per character |
| `[R]` script-item | Per-move. Picks which reaction to invoke given the defender's state (stand/crouch/air × hit/guard = 6 slots). | embedded in animation scripts |

When a character takes a hit:

1. The attacker's running `[R]` script-item is consulted. It has 6 slots —
   one per combination of {Stand, Crouch, Air} × {Hit, Guard}. Each slot
   holds a 1-byte **reaction-ID** (0 = "None").
2. The chosen reaction-ID is used as an index into the defender
   character's hurt-bind table (stored inside that character's project
   slot). That table produces the defender's **local** hurt-animation
   script + hit-spark script.
3. Those scripts are dispatched and the defender enters hitstun / block
   stun.

Thus one reaction-ID means *"for all characters, this is conceptually the
'stand-light-hit' response"* and each character chooses its own
per-character animation for it.

## 2. The 200-entry `reactionItems` table

Already declared in IDA Local Types:

```c
struct KgtReactionItem {          // size = 36
    char reactionName[32];        // "Hit WK Stand" etc. (GBK or ASCII)
    int  isHurtAction;            // bit 0 = "Doing" flag (damage-dealing)
};
```

Stored as `g_kgtGameSystemData.reactionItems[200]` at absolute address
`0x76A39C` (i.e. `g_kgtGameSystemData + 12800 = 0x765594 + 0x3200`).

Slot 0 is reserved for "None" (empty name). `InitHitJunctionNames` at
`0x40A4D0` zeroes the whole 7200-byte block and writes "None" into
slot 0.

When `InitializeCharacterSlot(0)@0x408280` builds a fresh project, it
fills slots 1..12 with the Cartesian product of 3 string tables at
`0x4472E0` / `0x4472E8` / `0x4472F0`:

- outer: `{"Hit   ", "Gaurd "}` — yes, "Gaurd" is misspelled in the binary
- middle: `{"WK", "SG"}` — "weak"/"strong" hitlevels
- inner: `{"Stand", "Crouching ", "Sky "}` — body posture

so the default names are "Hit WK Stand", "Hit WK Crouching", … ,
"Gaurd SG Sky" — 2 × 2 × 3 = 12 entries.

### `isHurtAction` semantics

The `[R]` codeblock docs note:

> If a move is "Guarded" it won't deal damage unless that hit junction
> has the "Doing" selection enabled.

The "Doing" checkbox is drawn by `ReactionIsHurtToggleWndProc@0x40B520`
(class `unk_7D6520`). It toggles bit 0 of
`reactionItems[g_currentReactionIdx].isHurtAction`. So `isHurtAction & 1`
is the global "this reaction deals damage even on block" / "this reaction
is a hurt action rather than a cosmetic/throw/push reaction" flag.

Higher bits of `isHurtAction` are currently unknown (probably unused).

## 3. Per-character hurt-bind table

**Storage trick**: 2DFM's `.player` file format is just a `KgtProjectSlot`
(1024 scripts + script items + pictures + sounds). To piggy-back extra
editor data without changing the file format, the editor re-uses script
slot **#488** and packs the 200-entry hurt-bind table into that script's
`char scriptName[32]` field — and the 32 bytes that follow it inside the
contiguous `scripts[]` array, writing past the nominal name into
subsequent script records.

Concretely, for character `c` and reaction `i`:

```c
int16 scriptId        = *(int16*)&g_characterSlots[c][1].scripts[488].scriptName[4*i + 6];
int16 effectObjectId  = *(int16*)&g_characterSlots[c][1].scripts[488].scriptName[4*i + 8];
```

This matches `HurtBindInfo` in `2dfm/KgtPlayer.hpp`:

```c
struct HurtBindInfo {
    int hurdId;          // == reaction ID (i)
    int scriptId;        // local script # for the hurt animation
    int effectObjectId;  // local script # for the hit-spark object
};
```

(File-format version uses 16-bit fields; HK port widens to 32-bit.)

The offset `+6` for `scriptId` and `+8` for `effectObjectId` means the
table actually starts 6 bytes into `scripts[488].scriptName` and extends
800 bytes further (200 × 4 B). It **overruns** into `scripts[489]`,
`scripts[490]`, … `scripts[508]` name fields, which are reserved for
this use. Similar trick is used by `ThrowReactionEditPanelWndProc` which
packs 1200 bytes into `scripts[508]..`.

The `[1]` in `g_characterSlots[c][1]` walks one full `KgtProjectSlot`
(1,271,828 bytes) forward from the base — the trailing block that
follows the common-resource part inside a `.player` file.

### Editor UI

`HitJunctionListPanelWndProc@0x4214B0` paints a 3-column grid of all 200
reactions for the currently-selected character (`g_currentEditorSlot`):

| X | Content | Source |
|---|---|---|
| 5..199 | `reactionName` | `g_kgtGameSystemData.reactionItems[i].reactionName` |
| 205..399 | "Alloment" — name of hurt-animation script | `scripts[488].scriptName[4*i+6]` (byte) → `g_mainHwnd+272+39*idx` (script name) |
| 405..    | "Spark" — name of hit-spark script | `scripts[488].scriptName[4*i+8]` (byte) → same lookup |

Clicking column 2 or 3 opens the **skill-selection popup** via
`CreateSkillListPopup@0x432BD0` (spawns a `SkillListPanelWndProc@0x4314A0`
child). The popup iterates `g_characterSlots[c]->scripts[0..1023]` and
on confirmation stores the chosen script index into
`*g_skillPickerResultPtr` (the pointer captured by
`HitJunctionListPanelWndProc` at click time).

The parent tab container `HitJunctionEditorPanelWndProc@0x421290`
(class `unk_7D5C20`) draws the column headers "Hit Junction" / "Alloment"
/ "Spark" and embeds the list panel.

## 4. The reaction-catalogue editor (`HitJunctionListWndProc`)

`HitJunctionListWndProc@0x40ACF0` (class `byte_7D65B0`) is the plain
left-hand vertical list of all 200 reaction names. Runs on the HWND
stored in `g_hitJunctionListHwnd` (=`0x7D6630`).

- `qword_44F218` — 8-byte `{scrollX, scrollY}` state.
- `g_currentReactionIdx` (`dword_44E670`, 1-based) — selected reaction.
- `g_reactionListCount` (`dword_44F0CC`) — number of populated entries.
- `g_reactionListColWidth` (`dword_44F0C8`) — longest-name-width / 2.
- `g_reactionListEditable` (`dword_5F1B92`) — master read-only flag.
- `RefreshHitJunctionList@0x40A4C0` → `InvalidateRect` on
  `g_hitJunctionListHwnd`.

Keyboard/mouse behaviour:

| Event | Action |
|---|---|
| LMB down on row `r` | `g_currentReactionIdx = r + scrollY`; repaint |
| LMB drag over another row | calls `SwapHitJunctions(srcIdx, dstIdx)` — full reorder |
| Double-click (WM_LBUTTONDBLCLK) | if populated: post `WM_COMMAND 0xABE2` (rename); else post `0xABE1` (insert) |
| RMB down | pop up add/rename/delete menu via `LoadMenuA` + `TrackPopupMenu` |

The RMB command IDs are handled in `HandleMenuCommand@0x4365A8`:

- `0xABE1` (44001): `InsertHitJunction(hWndParent, g_currentReactionIdx)`
- `0xABE2` (44002): `RenameHitJunction(hWndParent, g_currentReactionIdx)`
- `0xABE3` (44003): `DeleteHitJunction(hWndParent, g_currentReactionIdx)`

### `SwapHitJunctions@0x40AA70` (critical — demonstrates all three tiers)

When the user drags one reaction over another, the editor keeps everything
consistent by performing three swap passes:

1. **Global catalogue**: swap 36 bytes between
   `reactionItems[src]` and `reactionItems[dst]`.
2. **Per-character hurt-bind table**: for every populated
   `g_characterSlots[c]`, swap the 4-byte record at
   `scripts[488].scriptName[4*src+6..4*src+9]` with
   `scripts[488].scriptName[4*dst+6..4*dst+9]`.
3. **All script items in every character**: scan
   `scripts*items[0..65535]`; for every item with `type == 23`
   (the `[R]` codeblock — see section 5), call
   `SwapByteIndices@0x404360` on bytes `[0]`, `[2]`, `[4]`, `[6]`, `[8]`,
   `[10]` — i.e. the 6 reaction-ID fields embedded in the `[R]` cell.

Likewise, `InsertHitJunction@0x40A600` and
`DeleteHitJunction@0x40A8B0` use `AdjustByteIndexAfterInsert` /
`AdjustByteIndexAfterDelete` on the same 6 byte offsets to renumber
every `[R]` reference after a table shift.

The three-tier fixup is how the editor guarantees referential integrity
of `[R]` script items across insert/delete/swap.

## 5. The `[R]` Reaction codeblock (opcode 23 = 0x17)

`[R]` is not defined in the HK C++ port's `CommonScriptItemTypes` enum,
but the binary uses `type == 23` for it. Evidence:

- `SwapHitJunctions` scans for `type == 23` and swaps 6 byte-fields.
- `AttackPropertiesEditorWndProc` at `0x41CFC3` compares `eax, 17h` and
  then reads six `WORD`s at offsets `+1`, `+3`, `+5`, `+7`, `+9`, `+11`
  of the script-item struct.
- `HandleMenuCommand` at `0x435DC9` auto-inserts a type-23 cell before
  any hitbox/reaction cell if one isn't already present.

Inferred layout (16-byte `KgtScriptItem`):

```c
struct KgtReactionCmd {          // type == 23 ([R] Reaction)
    byte type;                   //  0   = 23 / 0x17
    byte standHit;               //  1   reaction-ID for Stand x Hit    (0..199)
    byte standHitFlags;          //  2   unknown (probably 0)
    byte crouchHit;              //  3
    byte crouchHitFlags;         //  4
    byte airHit;                 //  5
    byte airHitFlags;            //  6
    byte standGuard;             //  7
    byte standGuardFlags;        //  8
    byte crouchGuard;            //  9
    byte crouchGuardFlags;       // 10
    byte airGuard;               // 11
    byte airGuardFlags;          // 12
    byte _pad[3];                // 13..15 unused
};
```

The 6 reaction-ID bytes (at `bytes[0,2,4,6,8,10]` counting after the type
byte) are the ones that the three fixup helpers (`SwapByteIndices` /
`AdjustByteIndexAfterInsert` / `AdjustByteIndexAfterDelete`) rewrite
across all characters whenever the global catalogue changes.

**TODO**: verify whether the "flags" bytes at `+2/+4/+6/+8/+10/+12` are
actually zero-padding or hold per-slot flags (e.g. "override damage
ratio for this junction"). A grep over example `.kgt` files would
disambiguate.

### `[FA]` and `[FD]` — the hit/hurt-box attribute widget

`HitAttributeFlagsWndProc@0x40DE60` (class `lpClassName`) is the 160×16
custom control used by `AttackPropertiesEditorWndProc` to edit the
`[FA]` Attack Frame and `[FD]` Defense Frame attribute word. The 16-bit
parameter layout is:

| Bits | Meaning | v24 case |
|---|---|---|
| 0..3 | M. Number (hitbox/hurtbox identifier; widget caps at 13) | `v24==1`, `v26 = lparam + (val & 0xF)` |
| 4 | flag bit 0 → `Cancel` | `v24==2`, `XOR 1<<4` |
| 5 | flag bit 1 → `No Detection` | `v24==3`, `XOR 1<<5` |
| 6 | flag bit 2 → `Cont. Hits` | `v24==4`, `XOR 1<<6` |
| 7 | flag bit 3 → `No Sky Decision` | `v24==5`, `XOR 1<<7` |
| 8 | flag bit 4 → `Guard Fail` | `v24==6`, `XOR 1<<8` |
| 9 | flag bit 5 → `While Guard` | `v24==7`, `XOR 1<<9` |
| 14..15 | 2-bit selector (values 0..3) → `While Receiving` / `Shave` (2-bit because they're exclusive options, not separate flags) | `v24==8,9`, `val = lparam + (val>>14)` |
| 10..13 | unused |

Widget sub-regions (widget painted by a single `BitBlt` per slot):

- `(0, 0, 16, 16)` — error / active indicator (`(~val >> 8) & 0x10` =
  shows red if any bit 4..7 set **while** some other guard mask is
  set — used for visual error feedback).
- `(16, 0, 16, 16)` — draws the digit `(val & 0xF) + 2`, i.e. the
  M.Number.
- `(32, 0, 96, 16)` — six 16×16 tiles painted with
  `((val >> (bitNo+4)) & 1 | 4)` to show on/off for each of the 6
  checkbox bits.
- `(128, 0, 32, 16)` — the 2-bit selector.

The eighth checkbox ("Shave") listed in the codeblock docs is actually
one of the 4 possible values of the 2-bit selector (bits 14..15). The
doc's claim of "8 checkboxes" is wrong — only 6 are boolean bits; the
right-hand pair is an enum. Note from `2DFM Codeblocks.md`:

> "No Sky Decision" and "While Guard" names are likely mixed up
> in-engine. Newer translations probably fix this.

The bit assignment above treats the order as {Cancel, No Detection,
Cont. Hits, No Sky Decision, Guard Fail, While Guard} but the names are
from the in-engine UI and may be swapped relative to what the game
actually uses. The bit positions themselves are what matter for
rollback / runtime logic.

On edit the widget sends `WM_COMMAND wParam=propBase+0` (or `+1` for an
identifier-only poke) with `lParam` = new value. The parent
(`AttackPropertiesEditorWndProc`, handled by Agent 2) persists it into
the corresponding `[FA]`/`[FD]` script item.

## 6. The throw-reaction subsystem

Parallel to the hit-junction system is a **throw reaction** system, used
by the `[DS]` While-Throw-Do codeblock. It has the same 200-entry global
cadence but much less per-reaction data.

### Global catalogue

```c
struct KgtThrowReaction {  // size = 32
    char name[32];         // just a name; no flags
};
```

Stored as `g_kgtGameSystemData.throwReactions[200]` at
`g_kgtGameSystemData + 27852` (= `0x765594 + 0x6CCC`). `InitializeCharacterSlot`
doesn't pre-seed these — they start empty.

### Per-character throw-action table

Each character stores a 6-byte-per-throw-reaction record (1200 B total
for 200 slots) packed into `scripts[508].scriptName[6*idx + 26..]`:

```c
struct KgtThrowActionInfoRecord {       // 6 bytes
    int16 picNo;     // +0 -- 0..0x1FFF picture frame for the throw pose
    int16 offsetX;   // +2 -- -999..+999
    int16 offsetY;   // +4 -- -999..+999
};
```

Matches `ThrowActionInfo` in `2dfm/KgtPlayer.hpp` (except `throwActionId`
is the slot index, not stored).

### Editor UI — `ThrowReactionEditPanelWndProc@0x40FB70`

(class `unk_7D5A70`). Renamed from the earlier mislabel
"HitJunctionEditPanelWndProc" because it doesn't touch reactions — it
edits throw-actions.

Hosts three child controls on `WM_COMMAND wParam=10` (refresh):

1. A 4-digit numeric edit for `picNo` (`CreateNumericEditControl` id 291,
   range 0..0x1FFF, bound to `scripts[508].scriptName[6*idx + 26]`).
2. `CreateNumericSpinControl` for `offsetX` (id 420, range -999..+999).
3. `CreateNumericSpinControl` for `offsetY` (id 510, range -999..+999).

Each is live-bound to a specific 2-byte field of the current throw slot
(`g_currentThrowReactionIdx` = `dword_5F16B4`, 1-based). Controls are
disabled (flag bit 0x400 set on their structures) unless both
`throwReactions[idx].name[0] != 0` and the current character name is
non-empty.

The top painter region tallies "n < m" in the status bar showing
`g_currentThrowReactionIdx` / free throw-reaction slots.

## 7. WndProc summary table

| Addr | Name (new) | Class | Role |
|---|---|---|---|
| `0x40ACF0` | `HitJunctionListWndProc` | `byte_7D65B0` | The 200-reaction vertical list; drag-to-reorder; RMB add/rename/delete |
| `0x4214B0` | `HitJunctionListPanelWndProc` | `byte_7D5B90` | Per-character 3-column Reaction × Hurt × Spark table |
| `0x421290` | `HitJunctionEditorPanelWndProc` | `unk_7D5C20` | Parent tab — draws "Hit Junction/Alloment/Spark" headers |
| `0x40B520` | `ReactionIsHurtToggleWndProc` | `unk_7D6520` | "Doing" checkbox that toggles `reactionItems[current].isHurtAction & 1` |
| `0x40DE60` | `HitAttributeFlagsWndProc` | `lpClassName` | Custom 160×16 widget used by `[FA]`/`[FD]` editors to edit the 16-bit attribute word |
| `0x410ED0` | `SkillSelectListWndProc` | `byte_7D73C0` | Generic "pick a skill" popup list — stores chosen index into `*g_skillPickerResultPtr` |
| `0x4314A0` | `SkillListPanelWndProc` | `byte_7D6D90` | Another variant of the skill picker (spawned by `CreateSkillListPopup`) iterating all 1024 `g_mainHwnd`-rooted scripts |
| `0x40FB70` | `ThrowReactionEditPanelWndProc` | `unk_7D5A70` | Per-character throw-action picNo / offsetX / offsetY editor |

## 8. Helper functions in scope

| Addr | Name | Role |
|---|---|---|
| `0x40A4C0` | `RefreshHitJunctionList` | `InvalidateRect(g_hitJunctionListHwnd)` |
| `0x40A4D0` | `InitHitJunctionNames` | Zero the 200-entry table; set slot 0 = "None" |
| `0x40A500` | `RenameHitJunction(hWnd, idx)` | Prompt + update `reactionItems[idx].reactionName` |
| `0x40A600` | `InsertHitJunction(hWnd, afterIdx)` | Allocate new slot; shift table right; renumber `[R]` references via `AdjustByteIndexAfterInsert` |
| `0x40A8B0` | `DeleteHitJunction(hWnd, idx)` | Remove slot; shift left; renumber via `AdjustByteIndexAfterDelete` |
| `0x40AA70` | `SwapHitJunctions(hWnd, src, dst)` | 3-tier swap described above |
| `0x404360` | `SwapByteIndices(*byte, a, b)` | If `*byte == a` set to `b`; if `*byte == b` set to `a` |

## 9. Global variables (in addition to `ida_progress.md`)

| Addr | Name | Meaning |
|---|---|---|
| `0x44E670` | `g_currentReactionIdx` | 1-based index into `reactionItems[]`, set by LMB click in the reaction list |
| `0x5F16B4` | `g_currentThrowReactionIdx` | Counterpart for `throwReactions[]` — also reused as "current common-image idx" |
| `0x44F0CC` | `g_reactionListCount` | Populated-entry count for scrolling (recomputed on menu 10100) |
| `0x44F0C8` | `g_reactionListColWidth` | Longest reactionName length / 2 (scroll-bounds metric) |
| `0x7D6630` | `g_hitJunctionListHwnd` | HWND of the reaction list (`HitJunctionListWndProc` instance) |
| `0x5F1B92` | `g_reactionListEditable` | Master toggle: 0 suppresses LMB-dbl-click popup actions |
| `0x44E66E` | `g_skillPickerSelectedIdx` (WORD) | Shared cursor for the skill-picker popup |
| `0x5F1300` | `g_skillPickerResultPtr` | Pointer to the 2-byte field that will receive the picked skill index |
| `0x4B2940` | `g_skillPickerOriginalValue` (WORD) | Initial value when the picker opens (so "old" highlight is drawn) |
| `0x5F1BB8` | `g_skillPickerHostHwnd` | HWND that gets notified when the picker closes |

## 10. Cross-reference with HK port structures

| C++ (`2dfm/*.hpp`) | Binary location | Notes |
|---|---|---|
| `ReactionItem` (36 B) | `g_kgtGameSystemData.reactionItems[200]` | Global catalogue |
| `ThrowReaction` (32 B) | `g_kgtGameSystemData.throwReactions[200]` | Just names |
| `HurtBindInfo` | `scripts[488].scriptName[4*i+6..+9]` per char | 4 B/reaction: `{uint16 scriptId; uint16 effectObjectId;}` (HK port uses `int32`) |
| `ThrowActionInfo` | `scripts[508].scriptName[6*i+26..]` per char | 6 B/throw-reaction: `{int16 picNo; int16 offsetX; int16 offsetY;}` |
| `[R]` script-item | `KgtScriptItem{type=23, …}` (proposed `KgtReactionCmd`) | 6 × 1-byte reaction-IDs at offsets +1,+3,+5,+7,+9,+11 |

## 11. Known gaps / TODO

1. The 6 "flags" bytes at `+2,+4,+6,+8,+10,+12` in a type-23 script-item
   are treated as opaque by the editor's swap/insert fixups — they are
   neither renumbered nor ever modified by the picker. Likely padding,
   but worth verifying against game data. Grep sample `.kgt` files.
2. Whether `isHurtAction` has any bits beyond bit 0. Only bit 0 is ever
   toggled by the editor. Unused high bits may be padding or future
   flags.
3. The "Alloment" column label is a misspelling of "Allotment" in the
   binary — not worth patching.
4. `SkillSelectListWndProc@0x410ED0` and `SkillListPanelWndProc@0x4314A0`
   both look like generic script pickers but iterate different storage
   (`scripts[275].scriptName + 82 * v + 31` vs `g_mainHwnd + 39 * v + 272`).
   The stride-82 list at offset +275 looks like a per-character **command
   table** (100 × 82 B = 8200 B) — consistent with `KgtPlayer::commandListEntries`.
   Investigating the command editor is Agent 4's scope.
5. `HitAttributeFlagsWndProc`'s bit-to-checkbox naming depends on which
   UI translation you consult; the doc notes "No Sky Decision" and
   "While Guard" may be mislabelled. Bit positions are unambiguous; the
   human-readable labels are not.
