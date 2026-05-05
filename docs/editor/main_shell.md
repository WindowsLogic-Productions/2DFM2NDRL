# KGT2nd_EDITOR — Main Window Shell

This document transcribes the main-window / top-level UI layer of `KGT2nd_EDITOR.exe`: the main `WndProc`, its menu / keyboard-shortcut dispatchers, and the class registration that wires every editor sub-panel to its `WndProc`. All information below is sourced from IDA function comments left by previous reverse-engineering sessions; every row cites the instruction address where the comment sits. The sibling documents cover the interior editors — this doc is the shell around them.

Companion docs:
- `docs/editor/ida_progress.md` — IDA-wide inventory (structs, globals, loaders)
- `docs/editor/system_settings.md` — Settings tab + stage/demo editors interior
- `docs/editor/opcode_editor_ui.md` — Skill/opcode editor interior
- `docs/editor/sprite_palette_system.md` — BMP/palette editor interior
- `docs/editor/hit_junction_system.md` — HitJunction/CommonImage interior
- `docs/editor/player_file_format.md` — on-disk character file
- `docs/editor/game_ini_reference.md` — INI reference

---

## §1 Window-class → WndProc map

`RegisterEditorWindowClasses@0x4073d0` creates the full set of editor sub-window classes. It runs in two phases:

**Phase A** — Pre-allocate 62 × 144-byte class buffers starting at `g_windowClassBuffers = 0x7D5200` and auto-name them `"kgt2nd N"` (N = 0..61) (`0x4073d5..0x407409`).

**Phase B** — For each specific editor panel, fill the shared `WNDCLASSA` stack record with `lpfnWndProc` and the per-class `lpszClassName` pointer (one of the pre-allocated buffers), then `RegisterClassA`.

Class-buffer offset → WndProc table (name is what the class gets registered as — `"kgt2nd N"`):

| Index | Buffer addr | WndProc | Role |
|------:|:-----------:|:--------|:-----|
|  1 | `0x7D5290` | `EditorMainPanelWndProc` | Character-editor master panel |
|  2 | `0x7D5320` | `CharacterPropertiesContainerWndProc` | Character properties host |
|  3 | `0x7D53B0` | `CharacterPropertiesPanelWndProc` | Character properties inner panel |
|  5 | `0x7D54D0` | `NullPanelWndProc` | Empty/placeholder child |
|  6 | `0x7D5560` | `AnimationCellPanelWndProc` | Action-editor cell grid |
|  7 | `0x7D55F0` | `CodeblockPropertyEditorWndProc` | Codeblock (opcode) property editor |
|  8 | `0x7D5680` | `SpritePreviewWndProc` | Sprite preview |
|  9 | `0x7D5710` | `CpuMainPanelWndProc` | CPU / AI master panel |
| 10 | `0x7D57A0` | `CpuListPanelWndProc` | CPU entry list |
| 11 | `0x7D5830` | `CpuPropertiesPanelWndProc` | CPU entry properties |
| 12 | `0x7D58C0` | `CommandPriorityPanelWndProc` | Command-priority editor |
| 13 | `0x7D5950` | `CommandListWndProc` | Command list |
| 14 | `0x7D59E0` | `CommandEditPanelWndProc` | Command edit panel |
| 15 | `0x7D5A70` | `ThrowReactionEditPanelWndProc` | Throw reaction editor |
| 16 | `0x7D5B00` | `CommonImageListPanelWndProc` | Common-image list host |
| 17 | `0x7D5B90` | `SpritePreviewPanelWndProc` | Sprite preview host |
| 18 | `0x7D5C20` | `HitJunctionEditorPanelWndProc` | Hit-junction editor |
| 19 | `0x7D5CB0` | `HitJunctionListPanelWndProc` | Hit-junction list host |
| 20 | `0x7D5D40` | `BgEditorMainWndProc` | Stage/bg editor root |
| 21 | `0x7D5DD0` | `BgEditorContentPanelWndProc` | Stage editor content |
| 22 | `0x7D5E60` | `BgPlaceholderPanelWndProc` | Stage placeholder |
| 23 | `0x7D5EF0` | `BgSubPanel1WndProc` | Stage sub-panel 1 |
| 24 | `0x7D5F80` | `BgSubPanel2WndProc` | Stage sub-panel 2 |
| 25 | `0x7D6010` | `BgSubPanel3WndProc` | Stage sub-panel 3 |
| 26 | `0x7D60A0` | `BgSubPanel4WndProc` | Stage sub-panel 4 |
| 27 | `0x7D6130` | `DemoEditorMainWndProc` | Demo editor root |
| 28 | `0x7D61C0` | `DemoEditorContentPanelWndProc` | Demo editor content |
| 29 | `0x7D6250` | `DemoPlaceholderPanelWndProc` | Demo placeholder |
| 30 | `0x7D62E0` | `DemoSubPanelWndProc` | Demo sub-panel |
| 31 | `0x7D6370` | `EditorTabContainerWndProc` | **Main editor tab container** (see §6) |
| 32 | `0x7D6400` | `ScrollablePanelWndProc` | Generic scrollable panel |
| 33 | `0x7D6490` | `BasicRuleTabWndProc` | System-settings "Basic Rule" tab |
| 34 | `0x7D6520` | `ReactionIsHurtToggleWndProc` | Reaction-is-hurt toggle control |
| 35 | `0x7D65B0` | `HitJunctionListWndProc` | Hit-junction list |
| 36 | `0x7D6640` | `CommonImageContainerWndProc` | Common-image host |
| 37 | `0x7D66D0` | `CommonImageListWndProc` | Common-image list |
| 38 | `0x7D6760` | `CommonImagesTabWndProc` | Common-images tab |
| 39 | `0x7D67F0` | `ScriptPanelPlaceholderWndProc` | Script panel placeholder |
| 40 | `0x7D6880` | `ScriptTabWndProc` | Script tab |
| 41 | `0x7D6910` | `ToolbarRebarWndProc` | **Top rebar / toolbar** (see §6) |
| 42 | `0x7D69A0` | `BmpEditorMainWndProc` | BMP editor root |
| 43 | `0x7D6A30` | `PaletteEditorWndProc` | Palette editor |
| 44 | `0x7D6AC0` | `AnimationPreviewWndProc` | Animation preview |
| 45 | `0x7D6B50` | `AnimationTreePanelWndProc` | Animation tree list |
| 46 | `0x7D6BE0` | `BmpCanvasWndProc` | BMP editor canvas |
| 48 | `0x7D6D00` | `PlayerTreePanelWndProc` | Player tree list |
| 49 | `0x7D6D90` | `SkillListPanelWndProc` | Skill list host |
| 50 | `0x7D6E20` | `SkillCellGridPanelWndProc` | Skill cell grid host |
| 51 | `0x7D6EB0` | `SlotListWndProc` | **Slot list** (character/stage/demo sidebar, §6) |
| 52 | `0x7D6F40` | `StoryModeSettingsPanelWndProc` | Story-mode settings |
| 53 | `0x7D6FD0` | `StoryEventListPanelWndProc` | Story event list |
| 54 | `0x7D7060` | `SystemSettingsPanelWndProc` | System-settings panel |
| 55 | `0x7D70F0` | `BgEditorTabContainerWndProc` | Stage editor tab container |
| 56 | `0x7D7180` | `StageEditorTabContainerWndProc` | Stage editor tab container (alt) |
| 57 | `0x7D7210` | `BgLayerPanelWndProc` | Stage layer panel |
| 58 | `0x7D72A0` | `BgObjectPanelWndProc` | Stage object panel |
| 59 | `0x7D7330` | `BgPropertyPanelWndProc` | Stage property panel |
| 60 | `0x7D73C0` | `SkillSelectListWndProc` | Skill-select list |
| 61 | `0x7D7450` | `CharSelectLayoutTabWndProc` | Character-select layout tab |
| 62 | `0x7D74E0` | `BgBgmSettingsPanelWndProc` | Stage BGM settings |
| 63 | `0x7D7570` | `DemoBgmTimePanelWndProc` | Demo BGM timing |

Gaps (indices 0, 4, 47) correspond to pre-allocated buffers that this function does not register a WndProc into — likely reserved or consumed elsewhere. Indices 62/63 are beyond the "62 × 144B" allocation and overrun into the `.data` past `g_bmpEditorBMI`; worth sanity-checking.

Eight additional classes are registered with **statically-named** class strings (not `"kgt2nd N"`), via indirection pointers in `.rdata`:

| Class-name pointer | String at target | WndProc |
|:-------------------|:-----------------|:--------|
| `off_4451D0` | `"kgt2kCounterInput"` | `NumericSpinControlWndProc` |
| `off_4451D4` | `"kgt2kCounterPM5Input"` | `NumericDisplayControlWndProc` |
| `off_4451D8` | `"kgt2kCounterInput2"` | `NumericEditBoxWithTextWndProc` |
| `off_4451DC` | `"kgt2kCounterPM5Input2"` | `nullsub_1` |
| `off_4451E0` | `"kgt2kWazaNbInput"` | `SkillNameLabelWndProc` |
| `off_4451E4` | `"kgt2kWazaNbInput_tab"` | `nullsub_2` |
| `off_4451F4` | `"kgt2kCpuInput"` | `CellButtonWndProc` |
| `lpClassName @ 0x4451f0` | `"kgt2kCommandInput"` | `HitAttributeFlagsWndProc` |

TODO: `RegisterEditorWindowClasses`' top comment (`0x4073d0`) says `off_4451F4` is `"kgt2kHitJunctionInput"`; the actual `.rdata` string is `"kgt2kCpuInput"`. Either the comment is stale or the string was later renamed. Verify whether a second `"kgt2kHitJunctionInput"` registration exists in another function.

### Key-capture popup class (registered separately)

`InitializeMediaPlayback@0x434b70` is a misnomer — the function actually registers one final WndClass named `"KeyInput"` with `lpfnWndProc = KeyInputCaptureWndProc` (comment @ `0x434ba2`: *"InitializeMediaPlayback [MISNOMER]: actually RegisterKeyInputClass. ... Should be renamed RegisterKeyInputCaptureClass."*). See §6 for the WndProc itself.

---

## §2 Menu-ID → handler table

`HandleMenuCommand@0x435920` is the main `WM_COMMAND` dispatcher, routed from `MainWndProc` on control-id-bearing menu commands. Inline comments identify every case of its giant switch. The same function is re-entered from `HandleKeyPress` (§3) for the 7 shortcuts that simply synthesize menu IDs.

Table (menu-ID is `wParam` low word):

### File / Help / Tools (base range ~2900–3260)
| Menu ID (hex / dec) | Label | Action | Addr |
|:-------------------:|:------|:-------|:----:|
| `0xB5E` / 2910 | Help → About | `ShowConfigDialog` | `0x435a0e` |
| `0xB68` / 2920 | Help → Show HLP | Opens `2DK_HELP.HLP` | `0x435a30` |
| `0xBC2` / 3010 | File → New | `CreateNewProject()` | `0x43598b` |
| `0xBC3` / 3011 | File → Open | `OpenProjectFile(currentPath, 0)` | `0x4359a2` |
| `0xBC4` / 3012 | File → Save As | `SaveProjectFile(currentPath, 1, 0)` | `0x4359da` |
| `0xBC5` / 3013 | File → Save | `SaveProjectFile(currentPath, 0, 0)` | `0x4359be` |
| `0xBC6` / 3014 | File → Exit | `DestroyWindow(hWnd)` | `0x4359f5` |
| `0xBC7..0xBCD` / 3015..3021 | Tools → Pick color N | `ChooseColorForPalette(hWnd, wParam-3015)`; then `BuildColorBlendTable()` | `0x435a73` |
| `0xCB2` / 3250 | File → Test Play | `LaunchTestGame` (CD audio + launch) | `0x435b86` |
| `0xCBC` / 3260 | File → Compile / Validate & Create Test Game | `ValidateGameData` + `CreateTestPlayGame` | `0x435bb2` |

### Settings (base range 3101–3399)
| Menu ID | Label | Action | Addr |
|:-------:|:------|:-------|:----:|
| `0xC1D/0xC1E` (3101/3102) | Settings → Round Time N (radio) | Radio select | `0x435ab1` |
| `0xC1F` (3103) | Settings → Toggle Reaction List Edit | Toggle | `0x435ac7` |
| `0xC20/0xC21` (3104/3105) | Settings → Difficulty N (radio) | Radio select; posts `WM_COMMAND 0x2724` to re-render | `0x435ae4` |
| `0xC22` (3106) | Settings → Toggle Floating Window Mode | Toggle | `0x435b17` |
| `0xC23` (3107) | Settings → Toggle Preview Zoom Half-scale | Toggle | `0x435b4b` |
| `0xC24` (3108) | Settings → Toggle Auto Hit-Junction Insert | Toggle | `0x435b67` |
| `0xCE4..0xD06` (3300..3334) | Settings → Rebind Shortcut N | `g_shortcutSlotToRebind = menuID - 3300`; `CreateKeyInputCaptureWindow` | `0x435c19` |
| `0xD47` (3399) | Settings → Reset All Shortcuts | `memcpy(g_defaultShortcutTable → g_keyboardShortcutTable)` | `0x435c41` |

### Skill editor (base range 20100–20293)
| Menu ID | Label | Action | Addr |
|:-------:|:------|:-------|:----:|
| `0x4E84` (20100) | Skill → New Skill | `CreateNewSkillDialog` | `0x435c76` |
| `0x4E8E` (20110) | Skill → Delete Current Skill | `DeleteScriptEntry` | `0x435cab` |
| `0x4E98` (20120) | Skill → Rename Skill | Rename | `0x435d3d` |
| `0x4E99` (20121) | Skill → Copy Skill to Clipboard | Copy | `0x435d15` |
| `0x4E9A` (20122) | Skill → Paste Skill from Clipboard | Paste | `0x435cf4` |
| `0x4E9B` (20123) | Skill → Paste Skill (as duplicate, extra arg=1) | Paste-as-copy | `0x435d68` |
| `0x4EE8..0x4F0D` (20200..20237) | Skill → Insert Opcode N | `InsertSkillCellAtPosition(type = wParam-20200)`; auto-inserts type 23 cell first if `g_autoHitJunctionInsert` is set | `0x435e48` |
| `0x4F42` (20290) | Skill → Delete Current Cell | Delete cell | `0x435ea8` |
| `0x4F43` (20291) | Skill → Copy Cell | Copy cell | `0x435f0e` |
| `0x5044` (20292) | Skill → Paste Cell | Paste cell | `0x435f35` |
| `0x4F45` (20293) | Skill → Cut Cell | Copy + delete | `0x435ffb` |

### Character / Stage / Demo (40000–40043)
| Menu ID | Label | Action | Addr |
|:-------:|:------|:-------|:----:|
| `0x9C40` (40000) | Character → New / Insert | New | `0x435f95` |
| `0x9C41` (40001) | Character → Rename | Rename | `0x435f73` |
| `0x9C42` (40002) | Character → Delete | Delete | `0x436051` |
| `0x9C43` (40003) | Character → Import from File | Import | `0x4360b4` |
| `0x9C4A` (40010) | Stage → New / Insert | New | `0x4360d8` |
| `0x9C4B` (40011) | Stage → Rename | Rename | `0x43611b` |
| `0x9C4C` (40012) | Stage → Delete | Delete | `0x4360fa` |
| `0x9C4D` (40013) | Stage → Import BG from File | Import | `0x43613d` |
| `0x9C54` (40020) | Demo → New / Insert | New | `0x436161` |
| `0x9C55` (40021) | Demo → Rename | Rename | `0x436182` |
| `0x9C56` (40022) | Demo → Delete | Delete | `0x4361a4` |
| `0xA017` (40023) | Demo → Import from File | Import | `0x4361f0` |
| `0xA02B` (40043) | (Popup editor tab toggle #2) | `CleanupPreviewState(0)` | `0x436211` |

### Rebar / popup editor tab toggles (40045–42012)
| Menu ID | Label | Action | Addr |
|:-------:|:------|:-------|:----:|
| `0xA02D..0xA41C` (40045..42012) | Rebar toolbar button clicks for popup editor tabs | Toggles visibility; sends `TB_CHECKBUTTON`; creates/hides editor tab window | `0x43625b` |

### BMP editor (42001–42020)
| Menu ID | Label | Action | Addr |
|:-------:|:------|:-------|:----:|
| `0xA411` (42001) | BMP → Load BMP File | `ShowOpenFileDialog` | `0x4362d5` |
| `0xA411 / 0xA412` common path | — | `LoadBmpFileToSpriteDIB`; set title bar | `0x43631d` |
| `0xA41B` (42011) | BMP → Flush | `_flushall` | `0x4362c0` |
| `0xA41C` (42012) | BMP → Edit Mode 3 (rectangle selection) | `ApplyBmpEditMode3()` | `0x4363e0` |
| `0xA41D` (42013) | BMP → Edit Mode 0 (default) | `ApplyBmpEditMode0()` | `0x436418` |
| `0xA41E` (42014) | BMP → Select All | `SelectEntireBmpCanvas()` | `0x436438` |
| `0xA41F` (42015) | BMP → Apply Selection | `ValidateAndRefreshSelection()` | `0x436428` |
| `0xA423` (42019) | BMP → Batch Import Folder | `SHBrowseForFolder`, `FindFirst *.bmp`, import each + capture | `0x43646a` |
| `0xA424` (42020) | BMP → Cleanup Unused Sprites | Cleanup | `0x436448` |

### Hit-Junction / Common-Image / Command (44001–46938)
| Menu ID | Label | Action | Addr |
|:-------:|:------|:-------|:----:|
| `0xABE1` (44001) | HitJunction → Insert | Insert | `0x4365b7` |
| `0xABE2` (44002) | HitJunction → Rename | Rename | `0x43660c` |
| `0xABE3` (44003) | HitJunction → Delete | Delete | `0x4365f6` |
| `0xAFC9` (45001) | CommonImage → Insert | Insert | `0x436622` |
| `0xAFCA` (45002) | CommonImage → Rename | Rename | `0x436643` |
| `0xAFCB` (45003) | CommonImage → Delete | Delete | `0x436659` |
| `0xB798` (46936) | Command → Insert | Insert | `0x4366e0` |
| `0xB799` (46937) | Command → Rename | Rename | `0x4366bf` |
| `0xB79A` (46938) | Command → Delete | Delete | `0x43669d` |

TODO: `HandleMenuCommand` continues past `0x4366e0` with additional ranges not touched by the cmt sweep. Those are covered elsewhere (rebar toolbar block `0xA02D..0xA41C` annotation is the catch-all the agent left for the entire popup-tab span). A future pass should expand the rebar range into individual tool IDs.

---

## §3 Keyboard-shortcut table

`HandleKeyPress@0x402460` (top cmt at `0x402460`): called from the `WinMain` message loop for `WM_KEYDOWN`. Modifier bits are OR'd into the shortcut slot value: **`0x10000 = Ctrl`, `0x20000 = Shift`, `0x40000 = Alt`**. Matches against the 35-entry `g_keyboardShortcutTable[i]` and dispatches. `VK_TAB` (0x09) is trapped to cycle focus through visible editor tab panes (Tab forward, Shift+Tab back).

The table below is the full 35-slot layout (ID N is the "Rebind Shortcut N" menu at `0xCE4 + N`, see §2).

| Slot | Label (from `g_shortcutLabels`) | Action when pressed | Addr |
|-----:|:--------------------------------|:--------------------|:----:|
|  0 | Action*script compilation | Send `WM_MOUSEMOVE` (0x204) to `dword_7D55E0` (ActionEditor) | `0x40259f` |
|  1 | Action*Script Deletion | If `cell > 0`: delete-cell (20290); else: delete-script | `0x4025bf` |
|  2 | Copy | If `cell > 0`: cell-copy (20291); else: skill-copy (20121) | `0x40262d` |
|  3 | Paste | If `cell > 0`: cell-paste (20292); else: skill-paste (20122) | `0x402669` |
|  4 | Cut | cell-copy (20291) + cell-delete (20290) | `0x4026a5` |
|  5 | (unlabeled / "Unknown") | Sends 20123 (skill-paste-as-copy) | `0x4026d5` |
|  6 | Change Action Up | `NavigateByRelativeOffset(-1, 0)` | `0x4026f6` |
|  7 | Change Action Dwn | `NavigateByRelativeOffset(+1, 0)` | `0x402736` |
|  8 | Action step Left | `NavigateByRelativeOffset(0, -1)` | `0x40274b` |
|  9 | Action Step Right | `NavigateByRelativeOffset(0, +1)` | `0x40278a` |
| 10 | Up 10 Actions | `NavigateByRelativeOffset(-10, 0)` | `0x40279f` |
| 11 | Down 10 Actions | `NavigateByRelativeOffset(+10, 0)` | `0x4027b5` |
| 12 | To Begining of skill | `NavigateByRelativeOffset(0, -10)` | `0x4027f4` |
| 13 | To End of Skill | `NavigateByRelativeOffset(0, +10)` | `0x402834` |
| 14..17 | Call Bmp / Pallet / Ani / Skill Window | Toggles `g_editorTabCheckedFlags[6*i]`; sends `TB_CHECKBUTTON` for ID `(i + 40987)`; calls `HandleMenuCommand(i + 40987)` | `0x402874` |
| 18/19 | Tab Back / Forward | Main-tab cycle 0..3; posts `WM_COMMAND 0x2724` to main window | `0x4028cc` |
| 20/21 (mode=0, System) | Sub-tab Back / Forward | Sub-tab 0..4 cycle via `dword_7D63F0` / `_63FC` | `0x402959` |
| 20/21 (mode=1, Player) | Sub-tab Back / Forward | Sub-tab 0..6 cycle via `dword_7D5310` / `_531C` | `0x402930` |
| 20/21 (mode=2, Stage) | Sub-tab Back / Forward | Sub-tab 0..1 cycle via `dword_7D5DC0` / `_5DCC` | `0x402981` |
| 20/21 (mode=3, Demo) | Sub-tab Back / Forward | Sub-tab 0..1 cycle via `dword_7D61B0` / `_61BC` | `0x4029a7` |
| 22/24/26 | Character / Stage / Demo Up | Posts `WM_KEYDOWN VK_UP` (0x26) to slot list `dword_7D6F30` | `0x402b0b` |
| 23/25/27 | Character / Stage / Demo Down | Posts `WM_KEYDOWN VK_DOWN` (0x28) to slot list `dword_7D6F30` | `0x402b28` |
| 28 | New Game | `HandleMenuCommand(hWnd, 0xBC2)` | `0x402b40` |
| 29 | Open | `HandleMenuCommand(hWnd, 0xBC3)` | `0x402b47` |
| 30 | Save | `HandleMenuCommand(hWnd, 0xBC5)` | `0x402b4e` |
| 31 | Save As | `HandleMenuCommand(hWnd, 0xBC4)` | `0x402b55` |
| 32 | Compile Game | `HandleMenuCommand(hWnd, 0xCBC)` | `0x402b5c` |
| 33 | Test Play | `HandleMenuCommand(hWnd, 0xCB2)` | `0x402b63` |
| 34 | Quit | `HandleMenuCommand(hWnd, 0xBC6)` | `0x402b6b` |

Notes:
- Slot 20/21's sub-tab cycle reads the **current main mode** from `g_currentEditorMode` (see `docs/editor/ida_progress.md`) and chooses one of four branch tables.
- Slots 28..34 are thin wrappers over `HandleMenuCommand`; the reserved menu IDs let the same code path execute from both menu click and hotkey.
- Default-shortcut assignments live in `g_defaultShortcutTable`, memcpy'd into `g_keyboardShortcutTable` by menu `0xD47`.

---

## §4 Main window lifecycle (`MainWndProc@0x401c50`)

Top comment @ `0x401c50`:

> Main editor window proc. Handles WM_CREATE (1): creates tooltip + status bar + posts WM_COMMAND 0xA (init project). WM_DESTROY (2): cleanup. WM_SIZE (5): resize status bar + active tab. WM_CLOSE (0x10): prompt save-if-dirty then DefWndProc. WM_COMMAND (0x111): routes to HandleMenuCommand; special-cases wParam==10 (init project after open) and wParam==10020 (TCN_SELCHANGE).

| Message | Handler behavior |
|:--------|:-----------------|
| `WM_CREATE` (1) | Creates tooltip window + status bar; posts self-`WM_COMMAND` with `wParam = 10` (the "init project after open" path) |
| `WM_COMMAND` wParam=10 | Init-project path (runs after `OpenProjectFile` completes; see `ida_progress.md §Key Loader Algorithm`) |
| `WM_COMMAND` wParam=10020 | `TCN_SELCHANGE` handler — the main tab selection changed; triggers refresh of the active editor pane |
| `WM_COMMAND` (general) | Routes to `HandleMenuCommand@0x435920` (§2) |
| `WM_DESTROY` (2) | Cleanup |
| `WM_SIZE` (5) | Resizes status bar + active tab pane |
| `WM_CLOSE` (0x10) | If `g_projectDirty`, prompts save-if-dirty; then falls through to `DefWndProc` |

The agent did not leave per-message inline comments beyond the top summary; sub-message dispatches are not individually annotated. The relevant globals are `g_mainHwnd = 0x5F1320` (241 xrefs) and `g_projectDirty = 0x5F16F0`.

TODO: no inline cmts for the ~20 inner dispatch arms (tooltip create, status-bar column-layout, `TCN_SELCHANGE` active-pane swap). A future pass should annotate each WM_* arm.

---

## §5 Menu-state update logic (`UpdateMenuState@0x401a40`)

Top comment @ `0x401a40`:

> UpdateMenuState: toggles enable/check state for menu items BC5 (Save), BC4 (SaveAs), CBC, CB2 (TestPlay) based on g_projectLoaded. Also syncs radio-check state for round-time/difficulty/floating-window/reaction-list/preview-zoom/auto-hitjunction toggles. Re-renders all 35 shortcut menu strings by composing '<label>\tCtrl+Shift+Alt+<VK_STR>' for menu IDs 3300..3334.

In practice the function:

1. **Enable/disable (based on `g_projectLoaded`)**:
   - `0xBC5` (Save), `0xBC4` (Save As), `0xCBC` (Compile), `0xCB2` (Test Play) — grayed out when no project is loaded.
2. **Sync radio/check state** (items from §2 Settings group):
   - Round Time N radio (`0xC1D/0xC1E`)
   - Reaction List Edit toggle (`0xC1F`)
   - Difficulty N radio (`0xC20/0xC21`)
   - Floating Window Mode toggle (`0xC22`)
   - Preview Zoom Half-scale toggle (`0xC23`)
   - Auto Hit-Junction Insert toggle (`0xC24`)
3. **Re-render shortcut menu labels** (items `0xCE4..0xD06`, IDs 3300..3334): composes each menu string as `"<label>\tCtrl+Shift+Alt+<VK_STR>"` reflecting the current binding in `g_keyboardShortcutTable[N]`. This is how the shortcut menu displays "Action*script compilation    Ctrl+Shift+F1" etc.

TODO: The agent left no inline cmts on the individual `ModifyMenu` / `CheckMenuItem` calls — precise per-item formatting flags aren't annotated.

---

## §6 Other WndProcs (toolbar, tab container, slot list, key capture)

### `ToolbarRebarWndProc@0x407e10` — class index 41 @ `0x7D6910`
Top comment: *"kgt2nd class index 41 (g_windowClassBuffers[41]) = 0x7D6910. Toolbar rebar wrapper."*
The top rebar/toolbar wrapper. Hosts the toolbar buttons whose clicks map to the `0xA02D..0xA41C` rebar-button range in §2 (`HandleMenuCommand` @ `0x43625b`).

TODO: No inline cmts — WM_CREATE setup (button-registration, icon load, initial `TB_INSERTBUTTON` loop) is not annotated.

### `EditorTabContainerWndProc@0x408dd0` — class index 31 @ `0x7D6370`
Top comment: *"kgt2nd class index 31 (g_windowClassBuffers[31]) = 0x7D6370. Main editor tab container."*
The master tab container that hosts the four main modes (System / Player / Stage / Demo); `TCN_SELCHANGE` surfaces back into `MainWndProc` as `WM_COMMAND` wParam=10020 (§4) which in turn swaps the visible sub-mode.

TODO: Per-WM inline comments absent; the tab-swap / child-resize logic is uncommented.

### `SlotListWndProc@0x43b970` — class index 51 @ `0x7D6EB0`
Top comment: *"kgt2nd class index 51 (g_windowClassBuffers[51]) = 0x7D6EB0. Slot list (character/stage/demo sidebar)."*
The sidebar list used for picking the active character/stage/demo slot. `HandleKeyPress` routes `VK_UP / VK_DOWN` arrow keys from shortcuts 22..27 directly to this window (posted to its HWND stored at `dword_7D6F30`).

TODO: 278 inline cmts were reported but on inspection they are all Hex-Rays API-arg hints; no substantive per-message annotation was left. The WM_PAINT drawing of the slot list entries + WM_LBUTTONDOWN hit-testing + WM_KEYDOWN navigation are all un-annotated.

### `KeyInputCaptureWndProc@0x434bd0` — registered separately as `"KeyInput"`
Top comment @ `0x434bd0`:

> KeyInputCaptureWndProc: modal keyboard-capture popup for rebinding shortcuts. WM_PAINT (0xF): draws 'Press a new key or Escape' prompt + current shortcut label. WM_KEYDOWN (0x100): reads key + modifiers (Ctrl/Shift/Alt state via GetAsyncKeyState) into g_keyboardShortcutTable[g_shortcutSlotToRebind], calls UpdateMenuState, DestroyWindow. ESC cancels. Triggered from HandleMenuCommand cases 0xCE4..0xD06 (menu IDs 3300-3334) via CreateKeyInputCaptureWindow. Class name 'KeyInput' registered separately in InitializeMediaPlayback (misnomer).

Message handling:

| Message | Behavior |
|:--------|:---------|
| `WM_PAINT` (0x0F) | Draws the prompt `"Press a new key or Escape"` + current shortcut label |
| `WM_KEYDOWN` (0x100) | Reads pressed VK + modifier snapshot (`GetAsyncKeyState` for Ctrl/Shift/Alt); writes the composite value into `g_keyboardShortcutTable[g_shortcutSlotToRebind]`; calls `UpdateMenuState` (§5) to rebuild the shortcut-menu labels; `DestroyWindow`. **ESC cancels** (no write). |

The class itself is registered by `InitializeMediaPlayback@0x434b70` (misnomer — real purpose is registering this one class; comment at `0x434ba2` proposes renaming it `RegisterKeyInputCaptureClass`).

---

## See also

- `docs/editor/ida_progress.md` — globals / structs / loaders
- `docs/editor/system_settings.md` — settings-tab and stage/demo interiors
- `docs/editor/opcode_editor_ui.md` — skill/opcode editor interior
- `docs/editor/sprite_palette_system.md` — BMP/palette editor interior
- `docs/editor/hit_junction_system.md` — hit-junction / common-image interior
