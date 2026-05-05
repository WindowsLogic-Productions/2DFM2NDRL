# Skill / Action Editor — KGT2nd_EDITOR.exe

This doc covers the seven WndProcs that together make up the "skill (action /
script) editor" — the central part of the editor where a user picks an entry
from `g_projectSlot.scripts[1024]`, navigates its run of
`g_projectSlot.scriptItems[65536]`, edits each cell through the per-opcode
property panel (see `opcode_editor_ui.md`), and plays it back through the
animation preview window.

All information in this doc is **transcribed from comments left in IDA** by
previous reversing sessions. No new analysis was performed. Cite addresses
are the top-of-function addresses in `KGT2nd_EDITOR.exe` (image base 0x400000).

## Data Model Overview

Every "skill" (the editor's label) / "action" / "script" is one entry in the
currently active `KgtProjectSlot`:

| Project slot base | Content |
|---|---|
| `&slot->scripts[i]` (`KgtScript`, 39 bytes) | `scriptName[32]` + `scriptIndex:u16` + `gap:u8` + `flags:u32` |
| `&slot->scriptItems[slot->scripts[i].scriptIndex]` | Start of this script's codeblock cells |
| `&slot->scriptItems[slot->scripts[i+1].scriptIndex]` | One-past-end (cells belonging to script `i` are a **contiguous range** between `i`'s and `i+1`'s `scriptIndex`) |

`scripts[]` capacity is 1024, `scriptItems[]` is 65536; the active slot
pointer is walked from either `g_projectSlot` (project-level scripts),
`g_characterSlots[g_currentEditorSlot]` (per-player), `g_stageSlots[...]`,
or `g_demoSlots[...]`, selected by `g_currentEditorMode` (0..3).

`KgtScript.flags` is the **`ScriptSpecialFlag`** bitfield. Only bit 0 has
been pinned down by surrounding code: `flags & 1` marks a predefined /
system-reserved script that the user cannot rename or delete (see
`RenameSkillDialog` at 0x429ED0 — returns -1 immediately when `(flags & 1)`
is set on the target script).

The **absolute cell index** into `scriptItems[]` for "the cell the user has
clicked" is computed everywhere as `g_currentSkillIdx` (dword_602234) selects
the script, and `g_currentCellIdx` (dword_601B80) plus the script's
`scriptIndex` gives the item index:

```
absoluteCellIdx = dword_601B80
                + *(g_mainHwnd + 39*dword_602234 + 304)       # = scripts[skill].scriptIndex
```

(from `opcode_editor_ui.md` §GetAbsoluteCellIndex)

IDA field offsets from the top-of-function comment on `SkillListPanelWndProc`:

- `+272` → `KgtScript.scriptName`
- `+304` → `KgtScript.scriptIndex` (first item)
- `+343` → `KgtScript[i+1].scriptIndex` (one past the last item for script `i`)
- `+306` → `KgtScript.gap`
- `+307` → `KgtScript.flags` (ScriptSpecialFlag)

The "empty-script" test used throughout this family of WndProcs is
`scripts[i].scriptIndex == scripts[i+1].scriptIndex` — an empty contiguous
range means the script is unused / deletable. The IDA decompile sometimes
renders this as a `+152` comparison; that's register-reuse, not a real
field.

## §1 SkillListPanelWndProc @ 0x4314A0

Top-level doc comment at 0x4314A0:

> The skill-list panel that shows all named scripts (skills) for the current
> character/slot. Each row is 16 px tall. Items enumerated by walking
> `g_activeSlotPtr->scripts[1024]`, filtering to entries with non-empty
> `scriptName`.

**State globals:**

| Name | Role |
|---|---|
| `g_skillPickerSelectedIdx` (`WORD` @ `0x44E66E`) | Currently selected skill row |
| `qword_451728` | Scroll offset (hi = y-scroll, lo = x-scroll) |
| `dword_451730` / `dword_451734` | Scroll maxima (width/height in cells) |
| `g_skillListPopupHostHwnd` | Parent window to notify on selection |

**Message map:**

- `WM_CREATE`: initialize scroll position from `g_skillPickerSelectedIdx`.
- `WM_PAINT` (`0xF`): enumerate `scripts[]`, draw each row with non-empty
  `scriptName`, highlight the selected one.
- `WM_COMMAND 0x2774`: recount valid scripts and update scroll bars.
- `WM_LBUTTONDOWN`: commit the selection by writing
  `*g_skillPickerResultPtr = selected` (result ptr at `0x5F1300`).
- `WM_KEYDOWN VK_ESCAPE` / `WM_COMMAND 6,0`: cancel the popup.

The switch at 0x4319C0 handles 11 cases (WM_* messages 512..522 +
WM_COMMAND). Note the comment calls out that the decompiler's "`+152`
compare" is a false positive — the real test is
`scripts[i].scriptIndex == scripts[i+1].scriptIndex` (empty script).

This WndProc is the **list mode** of the skill picker. It is shown by
`ShowSkillListPopup @ 0x432CC0`, which is invoked from
`SkillNameLabelWndProc` on left-click when the label is operating in
"script-idx operand" mode (see §4).

## §2 SkillCellGridPanelWndProc @ 0x431CC0

Top-level doc comment at 0x431CC0:

> Horizontal codeblock-cell timeline for the 'skill picker' pop-up. Each
> selected skill displays its cells (`scriptItems` from
> `scripts[i].scriptIndex` to `scripts[i+1].scriptIndex - 1`) as a row of
> 16x16 icons starting `16*12 = 192 px` into each row, left of which is the
> 32-byte script name.

**State globals:**

| Name | Role |
|---|---|
| `g_skillPickerSelectedIdx` (`WORD`) | Which script (row) |
| `g_skillCellGridPopupCellIdx` (`BYTE` @ `0x4550A0`) | Which cell within that script |
| `g_skillPickerOriginalValue` (`@ 0x4B2940`) | Value on popup open (for "original" highlight) |
| `g_skillCellGridPopupOrigCellIdx` (`@ 0x7D518C`) | Cell value on popup open |
| `g_skillPickerResultPtr` (`WORD*` @ `0x5F1300`) | Caller's result slot (filled on click) |
| `g_skillCellGridResultCellIdxPtr` (`@ 0x601B84`) | Caller's cell-idx result slot |
| `qword_451750` | Scroll offset |
| `dword_451740` / `dword_451744` | Scroll maxima |

**Rendering:**

- Each cell's icon index comes from the opcode byte at `slot + 40208 + 16*itemIdx`
  (the opcode byte is the first byte of each `KgtScriptItem`). It's passed
  through the byte-to-icon table to produce a source rect in `hdcSrc`:
  `(16 * icon_idx, 16)` in the tileset.
- The **arrow line** on the panel connects the "original" cell
  (`dword_601B80` in the selected script) to the currently-hovered cell.
  This is a visual indicator of what the popup is about to commit.

This is the **grid/cell mode** of the skill picker. Invoked by
`ShowSkillCellGridPopup @ 0x432D40` from `SkillNameLabelWndProc` when the
label operates in "cell-idx operand" mode (see §4).

The per-cell insert/delete/copy verbs do **not** live in this WndProc — it
only renders and returns a selection. Cell mutations live in:

- `InsertSkillCellAtPosition @ 0x4295A0`
- `DeleteSkillCellAtPosition @ 0x429C20`

and are triggered from `AnimationTreePanelWndProc` (§6) or
`CreateNewSkillDialog` (§9) / cell-level context menus elsewhere.

## §3 SkillNameLabelWndProc @ 0x432DF0

Top-level doc comment at 0x432DF0:

> One-line label control shown next to each codeblock-operand slot in the
> property editor. Displays the skill name referenced by the operand.

This is the **SkillNameLabel** that appears everywhere an opcode has a
"target scriptId" operand (`[O]`, `[SG]`, `[SC]`, `[SF]`, `[V]`, `(R)`,
`[GS]`, `[GL]`, ... — see `opcode_editor_ui.md` §per-opcode field maps for
which opcodes instantiate one).

**Window properties (`GetPropA` keys):**

- `'SKILL_DESC_STRUCT'` (`asc_44CF5C`) → pointer to a byte-array state blob
  whose fields are:
  - `byte 0` — flags (bit 0 = grid-popup mode; bit 10 = disabled)
  - `byte 1` — render flags (bit 2 = "unavailable" style)
  - `byte 2, 4` — popup origin offsets
  - `byte 5` — pressed flag
  - `byte 6` — cell-idx operand pointer (for grid popup)
  - `byte 10` — script-idx operand pointer (for list popup)
  - `byte 14` — `WM_COMMAND id` to post to parent on completion

**Message handlers:**

- `WM_LBUTTONDOWN` (`0x201`):
  - If flag bit 0 is set → `ShowSkillCellGridPopup` (§2).
  - Otherwise → `ShowSkillListPopup` (§1).
- `WM_COMMAND 10` (`0xA`): skill-idx committed by `SkillListPopup` (new idx
  in `ho`).
- `WM_COMMAND 11` (`0xB`): cell-idx committed by `SkillCellGridPopup`.
- `WM_COMMAND 20` (`0x14`): popup cancelled → notify parent using the
  `WM_COMMAND id` stored at `PropA[14]`.

In other words, this label is a thin controller that decides which popup to
open based on whether the operand it edits is a scriptId or a cellIdx, then
forwards the chosen value back to the operand byte through the parent's
`WM_COMMAND` refresh path.

## §4 ActionEditorMainWndProc @ 0x414940

Top-level doc comment at 0x414940:

> Top-level parent window for the action (script/skill) editor.

**Hosted children:**

- `AnimationTreePanel` (§6) on the left (256 wide).
- Action editor tabs (stats/properties) on the right.
- Numeric spinner controls at the top:
  - "script select" → `g_selectedScriptIdx`
  - "step select" → `g_selectedCellIdx`
- Status bar at the bottom.

**Layout in `WM_CREATE`:**

| Child handle | Rect |
|---|---|
| `g_animTreeTabEntry` (`dword_7D5670`) | `(0, 72, 256, h-100)` — left pane |
| `dword_7D5700` | `(256, 18, w-256, h-18)` — right pane |
| `dword_7D55E0` | `(0, 0, w, 18)` — toolbar |
| `dword_7D6990` | `(0, h-26, 256, 26)` — status bar |

**Painting:**

`WM_PAINT` draws the currently selected cell's opcode label + icon at
`(10..40, 50..70)`. The opcode→string table is `off_445078[opcode]` (the
per-opcode label strings, e.g. "[M]", "[I]", "[O]"...). The 16×16 tile for
the opcode comes from `16*opcode` in `hdcSrc` (the global icon tileset).

**Commands:**

- `WM_COMMAND 1000`: jump to `(script, cell)` via `NavigateToEntryAndCell`
  (@ `0x4044A0`) after resetting the two sentinel values.
- `WM_COMMAND 10`: refresh the spinner controls with current values.

This WndProc is the glue — it owns the layout, paints the "currently
selected opcode" header, and forwards the numeric spinners into the
global script/cell selection.

## §5 AnimationTreePanelWndProc @ 0x414DB0

Top-level doc comment at 0x414DB0:

> The 'tree' view on the LEFT side of the action editor — despite the name
> it's actually a flat hierarchical list of all `(script, cell)` pairs for
> the current project slot, one row per cell. Left column = script name,
> right area = cell icons (one 16×16 per cell).

**State globals:**

| Name | Role |
|---|---|
| `g_selectedScriptIdx` / `g_selectedCellIdx` | Current selection |
| `qword_450270` (hi/lo) | Scroll offset |
| `dword_4501C0` / `dword_4501C4` | Scroll maxima |
| `g_animTreeLeftButtonDown` / `g_animTreeRightButtonDown` | Drag state |
| `g_animTreeDraggedRowIdx` (`0x44938C`) | Row being drag-swapped with `SwapSkillEntries` |
| `g_animTreeTimerId` (`0x4502AC`) | Auto-scroll timer (100 ms) |

**Key behaviors:**

- **Left-drag** within the tree **swaps adjacent skill entries** via
  `SwapSkillEntries @ 0x42A6B0`.
- **Right-click** opens a context menu (`MenuTree` resource). The submenu
  index is chosen based on `g_currentEditorMode`:
  - mode 0, 2, 3 → *scripts* submenu
  - mode 1 → *skills* submenu
  - and whether the right-clicked row is an empty script modifies the menu
    further.
- **Double-click** (`WM_LBUTTONDBLCLK 0x203`) posts either
  `WM_COMMAND 0x4E84` (**create new** skill) or `WM_COMMAND 0x4E98`
  (**insert cell**) to the parent.
- `WM_COMMAND 12345`: scroll the tree to ensure `g_selectedScriptIdx` is
  visible.
- `WM_COMMAND 10100`: recount visible rows and update scroll maxima.

**Rendering:**

Double-buffered via a compatible DC. For each row:

- Script-name column filled with system color 5 (window) or 13 (highlight).
- Cell icons blitted from `hdcSrc` using the opcode byte as the tile index.
- Selected row gets a top/bottom highlight line in the window-background
  colour.

This is the editor's primary hub for **picking** a skill/cell to edit, as
well as for **reordering** skills (drag) and **creating** / **deleting** /
**inserting** them (context menu + double-click).

Note: the top comment says the tree is "hierarchical" in the sense that
groupings exist, but the implementation is ultimately a flat per-cell row
list — there are no visible group-by-`ScriptSpecialFlag` branches in the
paint path the comments describe. The grouping/category filtering happens
upstream (different tab = different `g_currentEditorMode`, different slot
source array).

## §6 AnimationPreviewWndProc @ 0x42EA80

Top-level doc comment at 0x42EA80:

> The 'play' window that shows the currently selected skill animating in
> real time. Hosts a child tool-bar (rebar) with buttons to toggle
> hurtboxes (`A7F9`), hitboxes (`A7FA`), sound (`A7FB`), and half-resolution
> (`A7FC`).

**Important:** the **per-frame tick** happens in
`UpdateAnimationFrame @ 0x436B40` — **NOT** here. (Note the earlier doc
comment says `0x436AF0`, which is the previous instruction; the actual
lookup resolves to `0x436B40` in the current IDA database.) This WndProc
only handles lifecycle and painting.

**Message handlers:**

- `WM_CREATE`: `SetupAnimationPreview @ 0x438AC0` sets up entity objects
  in the object pool for the currently selected script.
- `WM_CLOSE` / `WM_DESTROY` (`0x2` / `0x10`): cleanup + save window rect.
- `WM_PAINT`:
  1. `CreateRenderSurfaceWithSize @ 0x406B60`
  2. `FillFramebufferRect @ 0x4067C0` (background)
  3. `FillFramebufferRect` (grid lines)
  4. `RenderAllGameObjects @ 0x438810` — walks the object pool and calls
     `ExecuteAnimationScript @ 0x439CD0` per tick (see
     `opcode_dispatcher.md` for the full opcode table).
  5. `BitBlt` / `StretchBlt` to the window DC.
- `WM_LBUTTONDOWN` / `WM_MOUSEMOVE` / `WM_LBUTTONUP`: drag to pan the
  preview origin (`dword_7D51C0`, `dword_7D51C4`) with cursor confinement.
- `WM_COMMAND A7F9..A7FC`: toggle the display flags and re-set up the
  preview.

**Canvas sizing:**

`g_currentEditorMode` determines canvas size:

- SCRIPT mode (0): 640×480.
- SKILL / PLAYER mode (1, 2, 3): 640 wide + `GetSystemMetrics` padding.

`g_animPreviewCharHalfRes` (`dword_62ED5C`) and
`g_animPreviewNonCharHalfRes` (`dword_62ED60`) cause the render to be
2× framebuffer and `StretchBlt`'d down — the "half res" toolbar toggle.

**Playback loop shape:**

Preview → `RenderAllGameObjects` (entry per entity in the local object
pool) → for each entity, `ExecuteAnimationScript(entity@esi)` → opcode
dispatcher at `0x439CD0` consumes `KgtScriptItem`s one per frame until a
wait opcode (`[I]` PIC keepTime), a branch opcode, or `[E]` end.

## §7 Dialogs — RenameSkillDialog & CreateNewSkillDialog

### RenameSkillDialog @ 0x429ED0

No top-of-function doc comment; structure reconstructed from the decompile:

- Guards `(scripts[skillIdx].flags & 1) != 0` → returns `-1` immediately
  (can't rename predefined scripts).
- Prompts via `DialogBoxParamA(hInstance, "DIALOGNAMEINPUT", ..., TextInputDialog, 0)`.
- Validates input:
  - Non-empty (shows `aPleaseInputThe_2` error if empty).
  - Length `≤ 31` (shows `aTheNameYouUseT_0` if longer).
  - Uniqueness — walks `scripts[0..1024]` via `CompareStringsExact` against
    `scripts[i].scriptName`; if the name exists at some other index it
    rejects with `aSkillsNameAlre`. If the name exists and the index equals
    the one being renamed (no-op rename) it returns `-1` as well.
- On success: `MarkSlotModified` + `qmemcpy(&scripts[i].scriptName,
  newName, 32)` + `ResetEditorViewState`.

### CreateNewSkillDialog @ 0x429FF0

No top-of-function doc comment; structure reconstructed from the decompile:

1. **Default name prefix** — switched on `g_currentEditorMode`:
   - 0 → "Script"
   - 1 → "Skill"
   - 2 → "Script" (variant)
   - 3 → "Script" (variant)
2. **Capacity check** — if `*(slot + 40091)` is set, refuses with
   `aToMuchSkills_0`.
3. **Default-name uniqueness loop** — iterates `"Skill1"`, `"Skill2"`, …
   until one is unused (tests each against all 1024 `scripts[].scriptName`
   using `CompareStringsExact`).
4. **Prompt** — `DialogBoxParamA("DIALOGNAMEINPUT", ..., TextInputDialog, 0)`
   with the prefilled default.
5. **Validate** — same empty / length / uniqueness checks as
   `RenameSkillDialog`.
6. **Shift scripts down** — `memmove(scripts[insertIdx+1..1023],
   scripts[insertIdx..1022], 36 bytes each)` — note the copy size is
   0x24 = 36 (the first 36 bytes of `KgtScript`) and the 2 trailing bytes
   (`flags` high word) are copied separately. Also preserves the
   `scriptIndex` at the insertion slot so that the new script starts where
   the old one did.
7. **Install new entry** — zero the 39-byte slot, restore `scriptIndex`,
   copy the new name.
8. **Fix up back-references** — for every word-index in
   `g_projectSlot.scriptItems[]` that **references a scriptId**, shift it
   by `AdjustWordIndexAfterInsert`. The switch over `opcode` at the tail
   identifies which byte-offsets inside each 16-byte `KgtScriptItem` hold
   scriptId references:
   - cases `0x02` (`[DS]`), `0x08`, `0x09` (`[SF]` LOOP), `0x10`, `0x11`
     (`[GL]`), `0x12`, `0x13`, `0x16` (`[DB]`) → shift `item[2..3]`
   - case `0x04` (`[O]` OBJECT) → shift `item[2..3]` **and** `item[5..6]`
     (`targetScriptId` + `targetScriptIdIfExists`).
   - cases `0x0A` (`[SG]` JUMP), `0x0B` (`[SC]` CALL), `0x1F` (`[V]`) →
     shift `item[1..2]`.
   - case `0x1E` (`[R]` reaction) → shift `item[3..4]`.
   - case `0x20` (`(R)` RANDOM) → shift `item[6..7]`.
   - case `0x24` → shift `item[1..2]`.
9. **System-table references** — in `g_currentEditorMode == 0` (main
   project), the loop walks `g_kgtGameSystemData.predefinedScriptIds[104]`
   (208 bytes / 2 = 104 entries) and adjusts each. In
   `g_currentEditorMode == 1` (per-player `.player` slot), it walks three
   ranges inside the slot's footer area (offsets 1,282,972..1,291,172 by 82
   bytes; 0..800; 1,293,138..1,293,180; 1,293,180..1,293,186) and adjusts
   every scriptId word via `AdjustWordIndexAfterInsert`.
10. **Insert first cell** — `InsertSkillCellAtPosition(slot, insertIdx, 0, 0, initialOpcode)`
    seeds a single item so the new script isn't empty.
11. `ResetEditorViewState`.

The inverse function is `DeleteScriptEntry @ 0x42A3F0` (mislabelled
`LoadCharacterData` in legacy IDA) — same fix-up loop, but with
`AdjustWordIndexAfterDelete` instead. See `ida_progress.md` for the
rename.

## Cross-References

- Field layouts: `ida_progress.md` §File-format structs (`KgtScript`,
  `KgtScriptItem`).
- Per-opcode editors (the **right-pane** property editor that edits the
  cell the skill editor selects): `opcode_editor_ui.md`.
- Runtime semantics (what each selected cell **does** during preview
  playback): `opcode_dispatcher.md`.
- Script-entry mutation helpers: `InsertSkillCellAtPosition @ 0x4295A0`,
  `DeleteSkillCellAtPosition @ 0x429C20`, `SwapSkillEntries @ 0x42A6B0`,
  `AdjustWordIndexAfterInsert @ 0x404290`, `AdjustWordIndexAfterDelete`
  (inverse, called from `DeleteScriptEntry @ 0x42A3F0`).
- Navigation / view reset: `NavigateToEntryAndCell @ 0x4044A0`,
  `ResetEditorViewState @ 0x403C30`, `MarkSlotModified @ 0x403850`,
  `ShowStatusMessage @ 0x4034D0`.
- Popup entry points: `ShowSkillListPopup @ 0x432CC0`,
  `ShowSkillCellGridPopup @ 0x432D40`.

## See Also

- `docs/editor/ida_progress.md`
- `docs/editor/opcode_dispatcher.md`
- `docs/editor/opcode_editor_ui.md`
- `docs/editor/2DFM Codeblocks.md`
