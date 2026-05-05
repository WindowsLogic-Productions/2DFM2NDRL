# Codeblock Property-Editor Window (Agent 2 scope)

This document maps the per-opcode property editor in `KGT2nd_EDITOR.exe` — the panel that dynamically rebuilds its controls depending on which codeblock (script-item opcode) is selected in the animation-cell grid.

Cross-reference: `docs/editor/2DFM Codeblocks.md` (authoritative UI description) and `2dfm/2dfmScriptItem.hpp` (struct layouts).

## Overview

### Key WndProcs

| Addr | Name | Role |
|---|---|---|
| `0x4194E0` | `CodeblockPropertyEditorWndProc` | The ~30 KB WndProc that dispatches to per-opcode sub-editors. Was originally mislabelled `AttackPropertiesEditorWndProc` — despite the name it handles every opcode, not just attacks. |
| `0x416340` | `AnimationCellPanelWndProc` | The left-side 16-column animation-cell grid that lets the user click a `KgtScriptItem`. Also hosts the `TrackPopupMenu` "insert opcode" context menu. |
| `0x4108E0` | `SpritePreviewPanelWndProc` | Sprite preview used by the `[I]` (PIC) sub-editor. |
| `0x430100` | `NumericEditBoxWithTextWndProc` | Numeric-edit rebar control. Accepts `WM_COMMAND wParam=10 (loadValue)` and `wParam=20 (addDropdownItem)`. On EN_UPDATE (`wParam=1515`) it calls `WriteParameterValue`, `MarkSlotModified`, and forwards the edit to its parent via `WM_COMMAND`. |
| `0x4303C0` | `NumericSpinControlWndProc` | Up/down spin variant of the numeric edit. |
| `0x430A50` | `NumericDisplayControlWndProc` | Read-only numeric display. |
| `0x42fc80` | `ReadParameterValue(descPtr)` | Reads a field value through a `ParameterDescriptor`. |
| `0x42fcf0` | `WriteParameterValue(descPtr, val)` | Clamps to `[minVal, maxVal]` and writes back via the descriptor's field pointer. |

### Key globals

| Addr | Name | Role |
|---|---|---|
| `0x4502C8` | `g_codeblockEditor_activeOpcodeLayout` | Cache of the opcode whose sub-editor controls are currently built. When a new cell is selected with the same opcode, the WndProc skips re-creating controls and just pushes new values via `WM_COMMAND wParam=10`. Set to `-1`/`-2` between layout switches. |
| `0x5F16D0` | `g_codeblockEditor_activeCodeblockKind` | Higher-level mode hint used by neighbouring panels (e.g. hitbox canvas, palette preview). Set to `0x14` for `[FA]`, `0x15` for `[FD]`, `0xE` for `[DB]`, `0xC` for `[RC]`, `0xD` for `[RP]`, `1` for `[I]`, `0` default. |
| `0x44E63C` | `g_codeblockEditor_statusBarHwnd` | Status-bar window updated on every WM_COMMAND with `sprintf("a %d %d", wParam, lParam)`. |
| `0x602234` | `dword_602234` | Index of the current character/stage/demo slot (used to find the active `KgtScript` in the slot's `scripts[]` table). |
| `0x601B80` | `dword_601B80` | Sub-position within the current script (the "column" in the animation grid). `GetAbsoluteCellIndex` = `dword_601B80 + *(g_mainHwnd + 39*dword_602234 + 304)` — the absolute `scriptItems[]` index of the selected cell. |

### Control-parameter descriptor (`ParameterDescriptor` / `struct` at `stru_450xxx`)

Each numeric control is driven by a 16+ byte descriptor living in the `.data` section. Layout (inferred from `ReadParameterValue` / `WriteParameterValue`):

```c
struct ParameterDescriptor {
    uint8_t  flags;        // +0: bit 0x40 = pointer-to-pointer, bit 0x80 = deref, bits [4:5] width flag (0=dword, 1=word, 2=byte)
    uint8_t  unused;       // +1
    uint16_t maxVal;       // +2 upper clamp
    uint16_t unused2;      // +4
    int16_t  minVal;       // +6 lower clamp
    void    *fieldPtr;     // +8 pointer to the KgtScriptItem byte(s) this control edits (set per-cell each WM_COMMAND wParam=10)
    void    *fieldPtr2;    // +10 alt deref path
    uint32_t notifyCtrlId; // +14 the control-ID forwarded to the parent on EN_UPDATE
};
```

The descriptors are pre-allocated statics shared across opcodes — when a sub-editor is built, each opcode "rents" a subset of them. When a value needs to be loaded the WndProc does `SendMessage(descHwnd, WM_COMMAND, 10, &scriptItem[off])` and the numeric-control WndProc re-reads/clamps via `ReadParameterValue`.

### Dispatch mechanism

On `WM_COMMAND` (in the PAINT-like refresh path, not the EN_UPDATE path), the WndProc:

1. Formats a `"a %d %d"` debug string to the status bar.
2. Looks up the selected `KgtScriptItem *edi = &projectSlot.scriptItems[absoluteCellIdx]`.
3. Switches on `edi[0]` (the opcode type byte) via the 42-entry jump table at `0x420B90`.
4. For each case:
   - If `g_codeblockEditor_activeOpcodeLayout == thisOpcode`, push current values into existing controls via `SendMessage(descHwnd, WM_COMMAND, 10, &edi[off])`.
   - Else, send `WM_COMMAND wParam=2` to destroy all children, then create fresh controls via the `Create{GroupBox,StaticLabel,NumericEdit,NumericSpin,CheckBox,RadioButton,SkillNameLabel,ComboBox}` helpers.

The `CreateNumericEditControl`/`CreateNumericSpinControl` helpers take a `ParameterDescriptor *` as their `hMenu` arg; the control uses `SetPropA` to store it, then later reads it via `GetPropA`.

### Edit flow (user types a new value)

1. User edits a text box. The inner `EDIT` child fires `EN_UPDATE` → `WM_COMMAND (273)` with `wParam=1515, HIWORD=1024`.
2. `NumericEditBoxWithTextWndProc` calls `WriteParameterValue(desc, atoi(text))` — this clamps and writes the new value through `desc.fieldPtr` directly into the `KgtScriptItem`.
3. It calls `MarkSlotModified()` (sets the character/stage/demo dirty flag).
4. It forwards `WM_COMMAND(0x111, desc.notifyCtrlId, newValue)` to its parent, which re-runs the sub-editor's load path to refresh any dependent controls.

### Opcode-type dispatch jump table

Located at `0x420B90`, 42 × 4-byte entries, indexed by `KgtScriptItem.type`:

| Idx | Type | Handler Addr | Codeblock | g_activeKind |
|---|---|---|---|---|
| 0 | `START` | `0x4196B3` | Position/timer marker (used by stage-START and empty cells) | 0 |
| 1 | `MOVE` | `0x419DC7` | **[M] Movement** | 0 |
| 2 | | `0x41A577` | **[DS] Detect Skill Divergence** | 0 |
| 3 | `SOUND` | `0x41A7B0` | **[S] Sound** | 0 |
| 4 | `OBJECT` | `0x41AB42` | **[O] Object** | 0 |
| 5 | `END` | `0x41B771` | **[E] End** (empty editor) | 0 |
| 6 | unused | `0x41B778` | empty editor | 0 |
| 7 | | `0x41B07D` | **[RC] Change Shape Condition (Common Image)** | 0x0C |
| 8 | unused | `0x41B77D` | empty editor | 0 |
| 9 | `LOOP` | `0x41B7AE` | **[SF] Loop** | 0 |
| 10 | `JUMP` | `0x41A3AC` | **[SG] GoTo** | 0 |
| 11 | `CALL` | `0x41A499` | **[SC] Call** | 0 |
| 12 | `PIC` | `0x41A126` | **[I] Image** | 1 |
| 13 | unused | default | — | — |
| 14 | | `0x41B923` | **[EB] Background Effect + Shake BG** | 0 |
| 15 | | `0x41C19A` | **[GS] Special Gauge Check** | 0 |
| 16 | | `0x41C3AC` | **[GL] Life Gauge Check** | 0 |
| 17 | | `0x41C607` | **[COM] Command-Input Divergence** | 0 |
| 18 | | `0x41C7F4` | **[DB] Basic Divergence** (variant 1) | 0 |
| 19 | | `0x41CAC8` | Script-for-special-character | 0 |
| 20 | | `0x41B475` | **[RP] Change Skill Partner (Script Mod)** | 0x0D |
| 21 | | `0x41CB96` | **[GC] Change Gauge Value** | 0 |
| 22 | | `0x41CD7F` | Combobox + numerics + radio (TODO identify) | 0 |
| 23 | | `0x41CFBE` | **[C] Cancel Condition** | 0 |
| 24 | | `0x41D232` | **[FA] Attack Frame (hitbox)** | 0x14 |
| 25 | | `0x41D5F1` | **[FD] Defense Frame (hurtbox)** | 0x15 |
| 26 | | `0x41D904` | **[PS] Player Stop (Time Stop)** | 0 |
| 27 | | `0x41D9D3` | **[DB] Basic Divergence** (variant 2) | 0x0E |
| 28–29 | unused | default | — | — |
| 30 | | `0x41DDC2` | **[V] Variable** (condition-only sub-mode) | 0 |
| 31 | `VARIABLE` | `0x41E0DC` | **[V] Variable** (full editor) | 0 |
| 32 | `RANDOM` | `0x41EA24` | **(R) Random** | 0 |
| 33–34 | unused | default | — | — |
| 35 | `COLOR` | `0x41EBFC` | **[COLOR] Color Modification** | 0 |
| 36 | | `0x41EE91` | 4-numeric editor + depth combobox (TODO identify) | 0 |
| 37 | `AFTERIMAGE` | `0x41F081` | **[AI] After Image** | 0 |
| 38–40 | unused | default | — | — |
| 41 | | `0x419D98` | Layout-reset trampoline (forces rebuild to type 1) | 0 |

## Per-opcode field maps

Each `KgtScriptItem` is exactly 16 bytes. The first byte is always the opcode. Remaining 15 bytes are opcode-specific. All byte offsets are **from the start of the item** (i.e. `item+0` = type).

### `[I]` PIC — opcode 12 @ `0x41A126`

Layout = `_2dfm::ShowPic` (from `2dfmScriptItem.hpp`).

| Bytes | Field | Control | Control ID | Descriptor |
|---|---|---|---|---|
| `item[1..2]` | `keepTime` (Wait frames 0-65535) | NumericSpin | — | `dword_4502EC` |
| `item[3..4]` & `0x1FFF` | `idxAndFlip & 0x1FFF` (Pic index 0-8191) | Image-picker listbox | — | `dword_45052C` → `dword_4505D0` |
| `item[3..4]` & `0x4000` | Flip X | CheckBox | `0x909` | — |
| `item[3..4]` & `0x8000` | Flip Y | CheckBox | `0x90A` | — |
| `item[5..6]` | `offsetX` (X axis) | NumericSpin | — | `dword_45038C` |
| `item[7..8]` | `offsetY` (Y axis) | NumericSpin | — | `dword_4503AC` |
| `item[9]` & `1` | `fixDir` (Ignore direction) | CheckBox | `0x90B` | — |

### `[M]` MOVE — opcode 1 @ `0x419DC7`

Layout = `_2dfm::MoveCmd`.

| Bytes | Field | Control | Control ID | Descriptor |
|---|---|---|---|---|
| `item[1..2]` | `accelX` (Gravity X) | NumericSpin | — | `stru_4502F0` |
| `item[3..4]` | `moveX` (Move X) | NumericSpin | — | `stru_450310` |
| `item[5..6]` | `moveY` (Move Y) | NumericSpin | — | `stru_450330` |
| `item[7..8]` | `accelY` (Gravity Y) | NumericSpin | — | `stru_450350` |
| `item[9]` & `1` | Replace/Add radio | Radio | `0x3F2` / `0x3F3` | — |
| `item[9]` & `2` | Stop moveX | CheckBox | `0x3F4` | — |
| `item[9]` & `4` | Stop moveY | CheckBox | `0x3F5` | — |
| `item[9]` & `8` | Stop accelX | CheckBox | `0x3F6` | — |
| `item[9]` & `0x10` | Stop accelY | CheckBox | `0x3F7` | — |

(Note: `2dfmScriptItem.hpp` has a bug — `isIgnoreAccelY` returns `flags & 0x16` but the editor writes `& 0x10` — likely typo in the HK source.)

### `[S]` SOUND — opcode 3 @ `0x41A7B0`

Layout = `_2dfm::PlaySoundCmd`.

| Bytes | Field | Control | Control ID | Descriptor |
|---|---|---|---|---|
| `item[1]` | `unknown` (Loop-to-Point flag?) | — | — | — |
| `item[2..3]` | `soundIdx` | ComboBox | — | `dword_450554` — populated from `g_projectSlot.soundHeaders[].name` |

### `[O]` OBJECT — opcode 4 @ `0x41AB42`

Layout = `_2dfm::ObjectCmd`.

| Bytes | Field | Control | Control ID |
|---|---|---|---|
| `item[1]` & `3` | Depth {0=Out,1=In,2=Point} | Radio | — |
| `item[1]` & `4` | UnCond | CheckBox | — |
| `item[1]` & `8` | Shadow | CheckBox | — |
| `item[1]` & `0x20` | Parent (attach as child) | CheckBox | — |
| `item[1]` & `0x40` | XY (use window coords) | CheckBox | — |
| `item[2..3]` | `targetScriptId` (Object skill) | Skill selector | — |
| `item[4]` | `targetPos` | Numeric | — |
| `item[5..6]` | `targetScriptIdIfExists` (It's Out skill) | Skill selector | — |
| `item[7]` | `targetPosIfExists` | Numeric | — |
| `item[8..9]` | `posX` | NumericSpin | — |
| `item[10..11]` | `posY` | NumericSpin | — |
| `item[12]` | `manageNo` (M.Number 0-9) | Numeric | — |
| `item[13]` | `layer` (Depth AP 0-127) | Numeric | — |

### `[SG]` JUMP / `[SC]` CALL — opcodes 10 / 11 @ `0x41A3AC` / `0x41A499`

Layout = `_2dfm::JumpCmd`.

| Bytes | Field | Control | Descriptor |
|---|---|---|---|
| `item[1..2]` | `jumpId` (target scriptId) | SkillNameLabel + Spin | `stru_4502D0` / `dword_4502EC` |
| `item[3]` | `jumpPos` | NumericSpin | `dword_45030C` |

### `[SF]` LOOP — opcode 9 @ `0x41B7AE`

Layout = `_2dfm::LoopCmd`.

| Bytes | Field | Control | Descriptor |
|---|---|---|---|
| `item[1]` | `loopCount` | NumericSpin | `dword_4502EC` |
| `item[2..3]` | `targetScriptId` | SkillNameLabel + Spin | `dword_45030C` |
| `item[4]` | `targetPos` | NumericSpin | — |

### `[V]` VARIABLE — opcode 31 @ `0x41E0DC` (also fragment at 30/`0x41DDC2`)

Layout = `_2dfm::VariableCmd`.

| Bytes | Field | Control | Descriptor |
|---|---|---|---|
| `item[1..2]` | `targetScriptId` (Cond Branch target) | SkillNameLabel | — |
| `item[3]` | `targetPos` | Numeric | — |
| `item[4]` | `targetVariable` (Var A-P, encoded via `byte_4547E1[eax*2]` LUT) | ComboBox | `dword_450554` |
| `item[5]` & `3` | Operation (0=None, 1=Assign, 2=Add) | Radio | — |
| `item[5]` & `0xC` | Condition (0=None, 4=Eq, 8=Gt, 12=Lt) | Radio | — |
| `item[5]` & `0x80` | CompareWithOtherVariable | CheckBox | — |
| `item[6]` | `compareVariable` | ComboBox | `dword_45057C` |
| `item[7..8]` | `operationValue` | NumericSpin | `stru_450310` |
| `item[9..10]` | `compareValue` | NumericSpin | `stru_450370` |

Special-value codes in the Var combobox (per `2dfmScriptItem.hpp` comment):
- `0x00-0x0F` Task A-P
- `0x40-0x4F` Character A-P
- `0x80-0x8F` System A-P
- `0xC0-0xC7` Data (X Coor / Y Coor / Map X / Map Y / Parent X / Parent Y / Time / # Rounds) — **compare-only, never assignable**

### `(R)` RANDOM — opcode 32 @ `0x41EA24`

Layout = `_2dfm::RandomCmd`.

| Bytes | Field | Control | Descriptor |
|---|---|---|---|
| `item[1..2]` | `randomMaxVal` (Total Numbers) | NumericSpin | `dword_4502EC` |
| `item[3..4]` | `moreThanVal` (When its above) | NumericSpin | `dword_45030C` |
| `item[5]` | `unknownGap` | — | — |
| `item[6..7]` | `targetScriptId` | SkillNameLabel | `dword_45036C` |
| `item[8]` | `targetPos` | Numeric | `dword_45038C` |

### `[COLOR]` — opcode 35 @ `0x41EBFC`

Layout = `_2dfm::ColorSetCmd`.

| Bytes | Field | Control | Control ID / Descriptor |
|---|---|---|---|
| `item[1]` | `colorBlendType` {0=Normal, 1=50%Alpha, 2=Add, 3=Subtract, 4=AlphaBlend} | ComboBox | `dword_450550` |
| `item[2]` | `red` (-32..+32) | NumericSpin | `dword_4502EC` |
| `item[3]` | `green` | NumericSpin | `dword_45030C` |
| `item[4]` | `blue` | NumericSpin | `dword_45032C` |
| `item[5]` | `alpha` (only enabled when `colorBlendType==4`) | NumericSpin | `dword_45034C` |

The code explicitly greys the alpha control unless blend type is 4 (AlphaBlend), matching the UI docs.

### `[AI]` AFTERIMAGE — opcode 37 @ `0x41F081`

Layout = `_2dfm::AfterimageCmd`.

| Bytes | Field | Control | Descriptor |
|---|---|---|---|
| `item[1..2]` | `unknownGap` | — | — |
| `item[3]` | `afterimageMaxCount` (num) | NumericSpin | `dword_4502EC` |
| `item[4]` | `afterimageGap` (time) | NumericSpin | `dword_45030C` |
| `item[5]` | `colorBlendType` (same 5 opts as COLOR) | ComboBox | `dword_45055C` |
| `item[6]` | `afterimageColorType` (Unused/Fixing/SmoothFade/ChikaChika/Random) | ComboBox | `dword_450558` |
| `item[7]` | `red` | NumericSpin | `dword_45032C` |
| `item[8]` | `green` | NumericSpin | — |
| `item[9]` | `blue` | NumericSpin | — |
| `item[10]` | `alpha` | NumericSpin | — |

### `[FA]` Attack Frame — opcode 24 @ `0x41D232`

Hitbox editor. Sets `g_codeblockEditor_activeCodeblockKind = 0x14` to switch the adjacent hitbox-canvas panel into "attack hitbox" mode.

| Bytes | Field | Control | Descriptor |
|---|---|---|---|
| `item[1..2]` | Top-Left X | NumericSpin | `dword_4502EC` |
| `item[3..4]` | Top-Right X (→ Y in struct) | NumericSpin | `dword_45030C` |
| `item[5..6]` | Bottom-Left (X-Radius) | NumericSpin | `dword_45032C` |
| `item[7..8]` | Bottom-Right (Y-Radius) | NumericSpin | `dword_45034C` |
| `item[9]` | M.Number (0-19, hitbox identifier) | Numeric | `dword_45036C` |
| `item[10]` | Flags — 8 checkboxes: Cancel `&1`, NoDetection `&2`, ContHits `&4`, NoSkyDecision `&8`, GuardFail `&0x10`, WhileGuard `&0x20`, WhileReceiving `&0x40`, Shave `&0x80` | CheckBoxes | — |
| `item[11]` | Power (0-255 damage) | Numeric | — |
| `item[12]` | (likely reaction-junction selector in FA too) | ComboBox | `dword_45038C` |

### `[FD]` Defense Frame — opcode 25 @ `0x41D5F1`

Hurtbox editor. Sets `g_codeblockEditor_activeCodeblockKind = 0x15`.

| Bytes | Field | Control | Descriptor |
|---|---|---|---|
| `item[1..2]` | Top-Left X | NumericSpin | `dword_4502EC` |
| `item[3..4]` | Top-Right (Y position) | NumericSpin | `dword_45030C` |
| `item[5..6]` | Bottom-Left (X-Radius) | NumericSpin | `dword_45032C` |
| `item[7..8]` | Bottom-Right (Y-Radius) | NumericSpin | `dword_45034C` |
| `item[9]` | M.Number (0-19) | Numeric | `dword_45036C` |
| `item[10]` & `0x2` → CheckBox (struct byte has `&0x04` as Striking inverted; stores 0=set/4=clear of second stru bit) | Striking | CheckBox | — |
| `item[10]` & (TBD — see Doing / Throwing checkbox IDs) | Doing | CheckBox | — |
| `item[10]` | Throwing | CheckBox | — |
| `item[11]` | Ratio (0-255, damage % multiplier) | NumericSpin | `dword_45038C` |
| `item[12]` | ComboBox at dialog ID `0x1266` — **hit-junction selector**, populated from `g_kgtGameSystemData.reactionItems[]` (see Agent 3's scope) | ComboBox | — |

### `[R]` Reaction — handled elsewhere (hit-junction selectors)

Note: the `[R]` codeblock from the HK doc is actually the 6-way hit-junction picker (Hits × {Stand, Crouched, Air}, Guard × {Stand, Crouched, Air}). Those 6 ComboBoxes each populate from `g_kgtGameSystemData.reactionItems[]` — see **Agent 3's scope** (`HitJunctionEditPanelWndProc` / `HitJunctionListWndProc` / `HitJunctionSelectListWndProc` / `HitAttributeFlagsWndProc`).

### `[C]` Cancel Condition — opcode 23 @ `0x41CFBE`

| Bytes | Field | Control | Descriptor |
|---|---|---|---|
| `item[1..2]` | Cancel mode (Fail/Hit/Uncod. 3-way radio or level-min) | Radio + ComboBox | `dword_450550` |
| `item[3..4]` | Level max (for Level-mode "Between" value) | ComboBox | `dword_450554` |
| `item[5..6]` | Level mode enabled / Skill selection | ComboBox | `dword_450558` |
| `item[7..8]` | (target skill) | ComboBox | `dword_45055C` |
| `item[9..10]` | (extra arg) | ComboBox | `dword_450560` |

(Exact bit meanings TBD — see [C] codeblock reference for the Level-vs-Skill mode distinction.)

### `[GS]` Special Gauge Check — opcode 15 @ `0x41C19A`

| Bytes | Field | Control | Control ID / Descriptor |
|---|---|---|---|
| `item[1]` | Little/Alot mode | ComboBox | `0x9CE` |
| `item[2..3]` | Special Gauge value (0-9) | NumericSpin | `dword_4502EC` |
| `item[4]` | `targetPos` (or Add-to-Advance) | Numeric | `dword_45030C` |
| `item[5..6]` | (If Failed skill) | SkillLabel | — |

### `[GL]` Life Gauge Check — opcode 16 @ `0x41C3AC`

Symmetrical to `[GS]` but with Life instead of Special Gauge (0-1000).

### `[GC]` Change Gauge Value — opcode 21 @ `0x41CB96`

| Bytes | Field | Control | Descriptor |
|---|---|---|---|
| `item[2..3]` | Yours Life | NumericSpin | `dword_4502EC` |
| `item[4..5]` | Yours Special | NumericSpin | `dword_45030C` |
| `item[6..7]` | His Life | NumericSpin | `dword_45032C` |
| `item[8..9]` | His Special | NumericSpin | `dword_45034C` |

### `[EB]` Background Effect + Shake BG — opcode 14 @ `0x41B923`

Two sub-panels:

**Palette Flash**:
| Bytes | Field | Control | Control ID |
|---|---|---|---|
| `item[1]` | Palette Flash type {Unused,Smooth Fading,Chika,Random} | ComboBox | `0x96A` |
| Color color-table descriptors at `stru_4502D0..stru_4503D0` use the standard ColorSet layout | |

**Shake BG** (descriptors `dword_450590` / `dword_450594`):
X-axis and Y-axis each have 5-option type (Unused/FadeOut/FadeIn/Fixed/Random), Duration (0-255), Shake (0-255).

### `[PS]` Player Stop (Time Stop) — opcode 26 @ `0x41D904`

| Bytes | Field | Control | Descriptor |
|---|---|---|---|
| `item[1..2]` | Your Time (0-255) | NumericEdit | `stru_4502F0` |
| `item[3..4]` | His Time (0-255) | NumericEdit | — |

### `[DS]` Detect Skill Divergence — opcode 2 @ `0x41A577`

| Bytes | Field | Control | Control ID |
|---|---|---|---|
| `item[1]` | DS type (7 options: Not/Landing/Attack Hits/Defending/Hit Wall/In Offset/While Throw Do) | ComboBox | `0x64A` |
| `item[2..3]` | `targetScriptId` | NumericSpin | — |
| `item[4]` | `targetPos` | NumericSpin | — |

### `[DB]` Basic Divergence — opcode 27 @ `0x41D9D3` (and variant 18 @ `0x41C7F4`)

| Bytes | Field | Control | Control ID |
|---|---|---|---|
| `item[1]` | DB setting (8 options: its not/Guarding/Standing/Crouching/Fwd/Back/Up/Down) | ComboBox | `0x132E` |
| `item[2]` | (extra) | NumericSpin | — |
| `item[3]` | `targetScriptId` low byte | NumericSpin | — |
| `item[4]` | `targetScriptId` high byte / `targetPos` | NumericSpin | — |
| `item[5]` | formed/failed selector | NumericSpin | — |

Sets `g_codeblockEditor_activeCodeblockKind = 0x0E`.

### `[COM]` Command-Input Divergence — opcode 17 @ `0x41C607`

| Bytes | Field | Control | Control ID |
|---|---|---|---|
| `item[2..3]` | Command Time (0-255) | NumericSpin | `dword_4502EC` |
| `item[4..5]` | `targetScriptId` | NumericSpin | `dword_45030C` |
| `item[5]` & `1` | (flag / `targetPos`?) | Radio | `0xA97` / `0xA98` |
| `item[6..7]` | Input 1 of the 5-input sequence | NumericSpin | `dword_45032C` |

Command-input bytes past `item[7]` likely extend into `item[8..15]` but layout varies — see codeblocks doc for the 5-cell cycling UI.

### `[RC]` Change Shape Condition (Common Image) — opcode 7 @ `0x41B07D`

Sets `g_codeblockEditor_activeCodeblockKind = 0x0C`.

| Bytes | Field | Control | Control ID / Descriptor |
|---|---|---|---|
| `item[1]` & `1` | Depth In/Out | Radio | `0x83F` / `0x840` |
| `item[1]` & `4` | Turn X | CheckBox | `0x85C` |
| `item[1]` & `8` | Turn Y | CheckBox | `0x85D` |
| `item[1]` & `0x10` | Same (sync with parent) | CheckBox | `0x85E` |
| `item[2..3]` | ComboBox selection — populated from `g_kgtGameSystemData.throwReactions[]` (200 × 32 bytes) | ComboBox | `0x848` (second combo `0x852` = character picker from `g_kgtGameSystemData.playerNames`) |
| `item[4..5]` | Position X | NumericSpin | `dword_45032C` |
| `item[6..7]` | Position Y | NumericSpin | `dword_45034C` |

### `[RP]` Change Skill Partner (Script Mod) — opcode 20 @ `0x41B475`

Sets `g_codeblockEditor_activeCodeblockKind = 0x0D`.

| Bytes | Field | Control | Control ID / Descriptor |
|---|---|---|---|
| `item[0]` | Final jump skill sel | ComboBox | `0xB40` populated from `g_kgtGameSystemData.reactionItems[]` (200 × 36 bytes = HitJunctions) |
| `item[1]` & `1` | Depth In/Out | Radio | `0xAFB` / `0xAFC` |
| `item[1]` & `4` | Turn X | CheckBox | `0xB18` |
| `item[2..3]` | (secondary selector or sel index) | — | `dword_45040C` |
| `item[4..5]` | Position X | NumericSpin | `dword_45032C` |
| `item[6..7]` | Position Y | NumericSpin | `dword_45034C` |

### Position marker / Stage START — opcode 0 @ `0x4196B3`

Used by:
- Stage-editor START item (`StageStart` struct: `flags` + `horiScroll` + `vertScroll`).
- All-mode position/timer markers (`KgtPos` / `KgtTimerPos`): `[x, y]` and optional `width`.

Dispatches by checking `item[0x133] & 0x60 == 0x60` (timer marker), else by `g_currentEditorMode`. The "Hit # Position. " group-box string, "Letter Width", and `dword_4502C8=0x28` (case 0x28 = position marker) confirm this.

### Cases 22, 36 — not fully identified

Structurally similar to `[DS]`-style jumps (combobox + 2-3 numerics + radio button). These may correspond to `[RS]` or unused/deprecated opcodes — the HK codeblocks doc enumerates ~25 opcodes but the editor reserves 42 slots.

## Remaining TODOs

- Decode the 8-checkbox flag order in `[FA]` item+10 — current mapping inferred from the codeblocks doc's ordering (Cancel/NoDet/ContHits/NoSky/GuardFail/WhileGuard/WhileRecv/Shave).
- Match control IDs `0x1266`, `0x113A`, `0x132E`, `0xA97`, etc. against the `.rc` / dialog-resource file (not available as source here — would require dumping from the `.rsrc` section).
- Confirm which global `dword_450xxx` descriptor drives which field in the more complex layouts (`[V]`, `[EB]`, `[COM]`) by following EN_UPDATE writes.
- Case 22 / case 36 identity.
- For `[O]` (OBJECT), confirm the Parent-attachment flag bit (`0x20` per `2dfmScriptItem.hpp`, but the baseline doc `isAttachAsChild` uses `0b00100000` = `0x20`).

## Files touched

- Renamed function `0x4194E0`: `AttackPropertiesEditorWndProc` → `CodeblockPropertyEditorWndProc`
- Renamed globals: `dword_4502C8` → `g_codeblockEditor_activeOpcodeLayout`, `dword_5F16D0` → `g_codeblockEditor_activeCodeblockKind`, `dword_44E63C` → `g_codeblockEditor_statusBarHwnd`
- Applied function names: `ReadParameterValue@0x42fc80`, `WriteParameterValue@0x42fcf0`, `NumericEditBoxWithTextWndProc@0x430100`, `NumericSpinControlWndProc@0x4303C0`, `NumericDisplayControlWndProc@0x430A50`, `AnimationCellPanelWndProc@0x416340`
- Added dispatch-level comments at the top-of-function, on the jump table, and on every per-case label inside `CodeblockPropertyEditorWndProc`.
