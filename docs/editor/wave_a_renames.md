# Wave A Global Rename Pass — `KGT2nd_EDITOR.exe` (.data `0x445000..0x5F1000`)

Cleanup pass over the 782 auto-named globals in Wave A's scope. Baseline: `docs/editor/ida_progress.md`; subsystem context from `opcode_editor_ui.md`, `skill_editor.md`, `sprite_palette_system.md`, `system_settings.md`, `test_play_and_audio.md`, `game_ini_reference.md`.

## Tally

| | Before | After |
|---|---|---|
| Auto-named (`dword_`/`byte_`/`word_`/`qword_`/`unk_`/`off_`/`stru_`/`flt_`/`dbl_`/`asc_`) in range | 782 | 553 |
| Globals processed | — | 229 renamed, 553 skipped (all xref ≤ 4) |
| Auto-named with xrefs ≥ 10 | 60 | 0 |
| Auto-named with xrefs 5–9 | 62 | 0 |
| Auto-named with xrefs 2–4 | — | 199 |
| Auto-named with xrefs 0–1 | — | 354 |
| User-named globals in range | 1,761 | 1,990 (+229) |
| `ParameterDescriptor` struct declared | no | yes (32-byte, 15 instances typed) |

Every high-value auto-named global is now resolved. Remaining items all have ≤ 4 xrefs and are low-leverage sub-panel state (deferrable).

## Key discoveries

### 1. `ParameterDescriptor` is a 32-byte struct, not 16

Reading `opcode_editor_ui.md` §ParameterDescriptor claimed 16 bytes. Decompiling `CreateNumericEditControl@0x430960` + `ReadParameterValue@0x42fc80` + `WriteParameterValue@0x42fcf0` revealed the true layout:

```c
struct ParameterDescriptor {       // 32 bytes
    uint16_t flags;        // +0  (bit 0x40 = pointer-to-pointer, 0x80 = deref, [4:5] = width)
    uint16_t maxVal;       // +2  upper clamp
    uint16_t pad4;         // +4
    int16_t  minVal;       // +6  lower clamp
    uint8_t  pad8, pad9;   // +8..+9
    void    *fieldPtr;     // +10 pointer to KgtScriptItem field (rebound each selection)
    uint32_t notifyCtrlId; // +14 control ID forwarded to parent on EN_UPDATE
    uint16_t pad12..pad1A; // +16..+1B
    HWND     hWndChild;    // +1C child HWND written by CreateWindowExA
};
```

The 4-byte HWND at `+1C` is what IDA saw as a separate `dword_XXX` 28 bytes after each `stru_XXX`. Declaring the struct collapses each `stru_XXX` + its +28 `dword_XXX` companion into a single typed slot.

Applied the type to 15 descriptor instances:
- `0x4502D0..0x450410` — 11 shared slots used by `CodeblockPropertyEditorWndProc` dispatch (`g_paramDesc_slot0..10`)
- `0x450510` — auxiliary slot (`g_paramDesc_slotAlt0`)
- `0x44F728`, `0x44F748`, `0x44F768` — `ThrowReactionEditPanelWndProc` picNo/offsetX/offsetY descriptors

### 2. INI string-pointer table at `0x4460AC..0x4461B0`

The cluster of `off_4460*` globals is a contiguous table of `LPCSTR` pointers passed to `GetPrivateProfileIntA`/`WritePrivateProfileStringA`. Named all 54 pointers by the ASCII string they point to (section headers like `Etc`/`Anime`/`ActionScreen`/`TestPlay`, and keys like `Action_nb`/`PlayerNb`/`Editor.TestPlay.GameSpeed` etc.). This is the static backbone of `LoadProjectSettings@0x404C0x`.

See `docs/editor/game_ini_reference.md` for the runtime meaning of each key.

### 3. Clusters identified

| Address range | Cluster | Sample renames |
|---|---|---|
| `0x4501B8..0x4502C8` | SpritePreview + AnimationCellPanel mouse/drag state | `g_spritePreview_savedCursorPos`, `g_animPanel_gridOriginPtr`, `g_animPanel_activeScriptItemPtr` |
| `0x4502D0..0x450410` | `ParameterDescriptor` shared slots (11) | `g_paramDesc_slot0..10` |
| `0x450550..0x4505B0` | Codeblock property-panel child HWNDs | `g_cbProp_groupBoxHwnd`, `g_cbProp_combo0..14`, `g_cbProp_shakeX_type` |
| `0x451600..0x4518A0` | BmpCanvas / SkillCellGrid / AnimationPreview state blocks | `g_bmpCanvas_dragMode`, `g_skillCellGrid_scrollOffset`, `g_skillCellGrid_scrollBase` |
| `0x4460AC..0x4461B0` | INI section/key name pointer table (54 entries) | `g_iniSec_Etc`, `g_iniKey_Action_step`, `g_iniKey_TP_GameSpeed` |
| `0x44E600..0x44E680` | Main-window UI handles (partially already named) | `g_editorMain_childPanelHwnd` |
| `0x44F720..0x44F784` | ThrowReactionEditPanel descriptor block | `g_throwReactionEdit_picNoDesc` etc. |
| `0x44F2*/0x44F9*` | Per-panel scroll-position dwords (`qword_*`) | `g_commandList_scrollPos`, `g_skillSelectList_scrollPos`, `g_hitJunctionList_scrollPos` |
| `0x4480EC` | 7-entry lookup of sub-tab indices per editor mode | `g_editorMode_subTabIdxMap` |
| `0x454390 / 0x4517AC` | Sprite-command encoder cursor + length | `g_spriteEncoder_outputCursor`, `g_spriteEncoder_outputLength` |
| `0x44DAC4 / 0x44DCD4 / 0x4519C4` | CRT-internal locale tables (do not rename further) | `g_crt_localeTable`, `g_crt_ctypeTable`, `g_crt_mbcp_state` |
| `0x5E92F8` | 8-entry `RC_CURSOR_%d` array loaded by `InitializeEditorResources` | `g_rc_cursorArray` |

### 4. Surprises

- **The 16-byte `ParameterDescriptor` in the published doc is wrong** — it's 32 bytes with HWND at `+0x1C`. The `opcode_editor_ui.md` table implicitly works because it always refers to the descriptor base (flags byte offset), but any code that wanted the HWND has been reading from a separate `dword_` label that was actually the same struct's tail. Declaring the real type folds those references together.
- **`off_4460AC..0x4461B0` is literally a contiguous `LPCSTR[]`** — one allocation, one logical object. Could be retyped to `LPCSTR g_iniStringTable[87]` as follow-up.
- `dword_4480EC` is a 7-int map (`{2,4,18,15,12,9,52}`) keyed on `g_editorMode` — those are picture-count multipliers used when sizing the tabbed editor's child layout.
- `dword_44F720` is NOT a descriptor despite sitting next to the 3 ParameterDescriptors at `0x44F728/48/68` — it's a state-change flag (-1/1). Separate logical object, just adjacent in storage.

## Regions largely left alone (deliberate)

- `0x445000..0x44E000` — string literals; already semantically carried by IDA's ASCII analysis.
- `0x4B3000..0x5F1000` — aux buffers, per-character caches. Most are inside the 47,851-byte `KgtPlayerRuntimeSlot` struct at `unk_4551E0` or other typed blocks; scanning showed **0 auto-named globals with xref ≥ 5** in this entire range.
- `0x460000..0x4B3000` — large pools. Nearly all addresses are offsets into `KgtPlayerRuntimeSlot[8]` / `KgtProjectSlot` and get named via struct-field access, not global label. Only 6 auto-named remain in this 332 KB region.

## Lists of renames

All renames applied via `mcp__ida__rename` batches. Highlights:

- **Sprite encoder**: `dword_454390` → `g_spriteEncoder_outputCursor`; `dword_4517AC` → `g_spriteEncoder_outputLength`.
- **Skill cell grid**: `qword_451750` → `g_skillCellGrid_scrollOffset`; `qword_451728` → `g_skillCellGrid_scrollBase`.
- **Throw reaction editor**: `dword_44F720` → `g_throwReactionEdit_stateChangeFlag`; `stru_44F728/48/68` → `g_throwReactionEdit_{picNo,offsetX,offsetY}Desc`.
- **Codeblock property panel**: `stru_4502D0..stru_4503D0` → `g_paramDesc_slot0..8` (+ 2 more unk_450xxx); `dword_450550..0x45058C` → `g_cbProp_{groupBoxHwnd,combo0..14}`.
- **INI table**: 54 pointers at `0x4460AC..0x4461B0` → `g_iniSec_*` / `g_iniKey_*` matching the pointed-to string.
- **Per-panel state**: 60+ sub-panel state dwords renamed to `g_<panel>_state<n>` (e.g. `g_cmdList_state0..9`, `g_sysSettings_state0..5`, `g_cpuProps_state0..3`, etc.).

## Next steps (for follow-up waves)

1. Retype the 11 `g_paramDesc_slot*` locations' referenced `KgtScriptItem` field pointers (i.e. apply the per-opcode `fieldPtr` meanings documented in `opcode_editor_ui.md`).
2. Build a single `LPCSTR g_iniStrings[]` array over `0x4460AC..0x4461B0` for cleaner decompiler output.
3. Remaining 553 low-xref auto-named globals are best handled opportunistically as each caller function is studied — not worth a standalone sweep.
4. Several `g_<panel>_state<n>` names are generic placeholders; a follow-up pass through each WndProc would let us assign semantic names (e.g. `g_cmdList_state0` is probably the command-list's selected-item index).
