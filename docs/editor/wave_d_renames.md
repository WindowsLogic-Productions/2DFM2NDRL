# Wave D Rename Report — `.data` 0x770000..0x7D9000

Scope: upper `.data` region of `KGT2nd_EDITOR.exe` — runtime object pools
and editor global state. Complementary to `docs/editor/ida_progress.md`,
`docs/editor/runtime_entity.md`, `docs/editor/skill_editor.md`.

## Counts

| Category | Before | After |
|---|---|---|
| Auto-named globals in range | 108 | 29 |
| Renamed to semantic names | — | +51 |
| CRT globals labeled with standard names | — | +16 |
| Typed struct absorbs (no rename needed) | — | 26 class-buffer slots + 3 BITMAPINFO palette bytes |

Net: 79 globals resolved. Remaining 29 auto-names are all inside typed
regions (26 × `g_windowClassBuffers[N]` slot aliases at 144-byte strides,
and 3 bytes inside `g_bmpEditorBMI.bmiColors[]` palette).

## Clusters Identified

### 1. `g_windowClassBuffers[64]` — 64 × 144 bytes @ 0x7D5200..0x7D7600

**Biggest finding.** The whole 9216-byte region between `g_windowClassBuffers`
(already labeled at `0x7D5200`) and `g_bmpEditorBMI` (at `0x7D7600`) is a
single array of 64 editor tab/panel entries, not individual globals.

Layout per 144-byte (`0x90`) slot:

```c
struct KgtEditorTabEntry {
    char className[128];   // "kgt2nd N\0" — used as WNDCLASS.lpszClassName
                           // by RegisterEditorWindowClasses @ 0x407A00.
                           // Also drives class-lookup chains in
                           // EditorMainPanelWndProc (reads *(slot+140)).
    HWND hwnd;             // +128 — instance HWND of this tab/panel
    int  unknown_132;      // +132 — used by a few tabs (BmpCanvas)
    int  activeSubTabIdx;  // +140 — persisted as INI "tab %d";
                           // LoadProjectSettings loops `for i in 0..63`.
};
```

Evidence: `RegisterEditorWindowClasses@0x407A00` fills 62 of 64 slots with
`sprintf(v1, "kgt2nd %d", v0); *(v1+34)=v0; v1+=144; while (v1 < &g_bmpEditorBMI)`.
`LoadProjectSettings@0x404d40` at offset `0x405113` loops
`for v11 in 0..; v12+=36 DWORDs` (= +144 bytes) reading `"tab %d"` into
`*(slot+140)`.

Named tab containers + HWND/active-idx pairs (47 total):

| Slot | Addr | Role | Renamed |
|---|---|---|---|
| 0 +140 | 0x7D528C | main-level tab index (0=System, 1=Player, 2=Stage, 3=Demo) | `g_mainTabActiveIdx` |
| 1 +128/140 | 0x7D5310 / 0x7D531C | Player-mode sub-tab container HWND + idx | `g_playerTabContainerHwnd`, `g_playerTabActiveIdx` |
| 3 +128 | 0x7D5430 | CharacterProperties tab HWND | `g_charPropertiesTabHwnd` |
| 4 +128 | 0x7D54C0 | NullPanel tab HWND | `g_nullPanelTabHwnd` |
| 5 +128 | 0x7D5550 | AnimationCellPanel HWND | `g_animationCellPanelHwnd` |
| 6 +128 | 0x7D55E0 | ActionEditor toolbar HWND | `g_actionEditorToolbarHwnd` |
| 7 +128 | 0x7D5670 | AnimTree tab entry HWND | `g_animTreeTabHwnd` |
| 8 +128 | 0x7D5700 | ActionEditor right pane HWND | `g_actionEditorRightPaneHwnd` |
| 9 +128 | 0x7D5790 | CpuList panel HWND | `g_cpuListPanelHwnd` |
| 10 +128 | 0x7D5820 | CpuMainPanel HWND | `g_cpuMainPanelHwnd` |
| 11 +128 | 0x7D58B0 | CpuProperties panel HWND | `g_cpuPropertiesPanelHwnd` |
| 12 +128 | 0x7D5940 | CommandPriority panel HWND | `g_commandPriorityPanelHwnd` |
| 13 +128 | 0x7D59D0 | CommandEdit panel HWND | `g_commandEditPanelHwnd` |
| 14 +128 | 0x7D5A60 | ThrowReactionEdit panel HWND | `g_throwReactionEditPanelHwnd` |
| 15 +128 | 0x7D5AF0 | CommonImageList panel HWND | `g_commonImageListPanelHwnd` |
| 16 +128 | 0x7D5B80 | SpritePreview panel HWND | `g_spritePreviewPanelHwnd` |
| 17 +128 | 0x7D5C10 | HitJunctionList panel HWND | `g_hitJunctionListPanelHwnd` |
| 18 +128 | 0x7D5CA0 | (sibling HitJunctionList panel) | `g_hitJunctionListPanelHwnd2` |
| 19 +128 | 0x7D5D30 | HitJunctionEditor panel HWND | `g_hitJunctionEditorPanelHwnd` |
| 20 +128/140 | 0x7D5DC0 / 0x7D5DCC | Stage tab container HWND + idx | `g_stageTabContainerHwnd`, `g_stageTabActiveIdx` |
| 27 +128/140 | 0x7D61B0 / 0x7D61BC | Demo tab container HWND + idx | `g_demoTabContainerHwnd`, `g_demoTabActiveIdx` |
| 31 +128/140 | 0x7D63F0 / 0x7D63FC | System tab container HWND + idx | `g_systemTabContainerHwnd`, `g_systemTabActiveIdx` |
| 33 +128 | 0x7D6510 | ReactionIsHurtToggle HWND | `g_reactionIsHurtToggleHwnd` |
| 34 +128 | 0x7D65A0 | HitJunctionList (alt) HWND | `g_hitJunctionListAltHwnd` |
| 37 +128 | 0x7D6750 | CommonImage preview HWND | `g_commonImagePreviewHwnd` |
| 41 +128 | 0x7D6990 | ActionEditor status-bar HWND | `g_actionEditorStatusBarHwnd` |
| 43 +128 | 0x7D6AB0 | PaletteEditor HWND | `g_paletteEditorHwnd` |
| 44 +128 | 0x7D6B40 | AnimationPreview top-level HWND | `g_animationPreviewTopHwnd` |
| 45 +128 | 0x7D6BD0 | AnimationTree list HWND | `g_animationTreeListHwnd` |
| 46 +128/132 | 0x7D6C60 / 0x7D6C64 | BmpCanvas HWND + sub-state | `g_bmpCanvasHwnd`, `g_bmpCanvasSubState` |
| 51 +128 | 0x7D6F30 | Character/Stage/Demo slot list HWND | `g_slotListHwnd` |
| 52 +128 | 0x7D6FC0 | StoryEventEditor HWND | `g_storyEventEditorHwnd` |
| 53 +128 | 0x7D7050 | StoryEditor sub-panel 1 | `g_storyEditorSubPanel1Hwnd` |
| 54 +128 | 0x7D70E0 | StoryEditor sub-panel 2 | `g_storyEditorSubPanel2Hwnd` |
| 55 +128 | 0x7D7170 | StageEditor tab container HWND | `g_stageEditorTabContainerHwnd` |
| 60 +128/140 | 0x7D7440 / 0x7D744C | SkillSelectList HWND + idx | `g_skillSelectListHwnd`, `g_skillSelectListActiveIdx` |
| 62 +128 | 0x7D7560 | BgBgmSettings panel HWND | `g_bgBgmSettingsPanelHwnd` |
| 63 +128 | 0x7D75F0 | DemoBgmTime panel HWND | `g_demoBgmTimePanelHwnd` |

Followup: declare `KgtEditorTabEntry` as an IDA struct and retype
`g_windowClassBuffers` as `KgtEditorTabEntry[64]`; then the 26 remaining
`unk_7D5xxx`-style aliases resolve as `g_windowClassBuffers[N]` and the
48 `+128`/`+140` aliases disappear into field accesses.

### 2. BMP editor selection & canvas (0x7D51A0..0x7D51E4)

Canvas selection-rectangle + canvas dimensions, driven by BmpCanvasWndProc
and consumed by ShrinkSelectionToContent / CaptureImageFrameFromSelection
/ SelectEntireBmpCanvas.

| Addr | New name | Role |
|---|---|---|
| 0x7D51A0 | `g_bmpSelectionX1` | Selection rect corner |
| 0x7D51A4 | `g_bmpSelectionY1` | Selection rect corner |
| 0x7D51A8 | `g_bmpSelectionX2` | Selection rect corner |
| 0x7D51AC | `g_bmpSelectionY2` | Selection rect corner |
| 0x7D51E0 | `g_bmpCanvasWidth` | Loaded BMP width (set by LoadBmpFileToSpriteDIB) |
| 0x7D51E4 | `g_bmpCanvasHeight` | Loaded BMP height |

### 3. Animation-preview scroll origin (0x7D51C0..0x7D51D4)

Three `(x,y)` pairs indexed by `2 * g_currentEditorMode`. AnimationPreviewWndProc
accesses as `dword_7D51C0[2 * g_currentEditorMode] += dx` on WM_MOUSEMOVE
drag; `ClampPreviewPosition` clamps per-mode; RenderAllGameObjects reads
the pair as the world-to-screen translation for each render tick.

| Addr | New name | Mode |
|---|---|---|
| 0x7D51C0 / 0x7D51C4 | `g_previewScrollOriginX_skill` / `Y_skill` | mode 0 (Script) |
| 0x7D51C8 / 0x7D51CC | `g_previewScrollOriginX_player` / `Y_player` | mode 1 (Player/Character) |
| 0x7D51D0 / 0x7D51D4 | `g_previewScrollOriginX_stage` / `Y_stage` | mode 2 (Stage) |

Persisted across editor restarts under INI section
`[Canvas] %d x / %d y` loop (indices 0..2). The `&dword_7D51E4` loop
terminator confirms exactly 3 pairs — the canvas width/height at
0x7D51E0 is the stop sentinel.

### 4. Current-skill cell count (0x7D51B0)

`g_currentSkillCellCount`: total number of script items (cells) in the
currently-selected skill. Set by `NavigateToEntryAndCell@0x404540`:
`v10 = scripts[skill].scriptEnd - scripts[skill].scriptStart` and stored
here for UI loop bounds elsewhere (SpritePreviewWndProc cell scan).

### 5. Render runtime state (post-character-slots)

`g_characterSlots[50]` ends exactly at 0x7D7C48 (50 × 4 = 0xC8);
`dword_7D7C48` is the first dword immediately after, populated by
`InitializeRenderState@0x4369E0` with value 655 and consumed by
`ExecuteAnimationScript` as a Y-axis scale factor (paired with
`dword_60222C = 393` for X). Named `g_renderPixelsPerUnitY`.

### 6. Object-pool neighbors (0x7758A8..0x7758F0)

| Addr | New name | Role |
|---|---|---|
| 0x7758A8 | `g_stageSlotsArrayEnd` | sentinel at end of g_stageSlots[50] (50*4 = 0xC8, 0x7757E0+0xC8=0x7758A8) |
| 0x7758E4 | `g_renderTickFrameCounter` | SetupAnimationPreview writes `ebp` (frame counter) |
| 0x7758F0 | `g_renderTickFrameLimit` | SetupAnimationPreview writes 0x26AC (= 9900) frame cap |

### 7. CRT internals (0x7D7C4C..0x7D8FD4)

16 globals labeled with their standard MSVC CRT names (not game state;
left in place for completeness so future readers don't hunt them):

| Range | Subsystem | Renames |
|---|---|---|
| 0x7D7C4C..0x7D7C5C | Small-block heap | `__sbh_sizeHeapList`, `__sbh_pHeapNext`, `__sbh_pHeapList`, `__sbh_cntHeapList`, `__sbh_indGroupDefer` |
| 0x7D7C64..0x7D7D81 | Locale / MBCS | `__mbcodepage`, `__mbulinfo_ptr`, `__mbcasemap`, `__mbctype` |
| 0x7D7E88..0x7D8FD4 | stdio / atexit / argv | `__piob_stdio`, `__lc_collate_ref`, `__atexit_fnPtr`, `__argv_ptr`, `__exit_atexit_ptr`, `__exit_atexit_max`, `__argc_value` |

## Sentinel globals

| Addr | New name | Meaning |
|---|---|---|
| 0x7D7680 | `g_toolbarRebarHwndArrayEnd` | terminator for 36-DWORD HWND array walked by `ResetEditorViewState@0x403C30` |
| 0x7D768C | `g_editorTabInfoArrayEnd` | terminator for the 64-entry 144-byte tab-info loop in `LoadProjectSettings`/`SaveProjectSettings` |

## Surprises

1. **No real `g_currentObject` sibling state at 0x7D519x.** The heavily-
   xref'd `dword_7D51A0..AC` cluster next to `g_currentObject@0x7D5190`
   is purely BMP-canvas selection state, unrelated to runtime objects.
   The adjacency is a layout coincidence — different subsystems sharing
   the same 64-byte `.data` slab.

2. **AnimationPreviewWndProc is one of the heaviest consumers of
   0x7D51xx, but uses it as per-mode-indexed scroll origin, not
   per-object state.** The `dword_7D51C0[2 * g_currentEditorMode]`
   pattern was the tell.

3. **The entire 0x7D5200..0x7D7600 range is a single 64-entry tab-info
   array**, not a grab-bag of panel state. 26 of the "auto-named"
   globals IDA surfaced are just base addresses of the array slots;
   48 more are references to +128 (HWND) or +140 (activeSubTabIdx)
   fields within slots. Declaring the struct type would clean all of
   these up in one pass (follow-up).

4. **`dword_7D7C48 = 655` sits exactly at the end of `g_characterSlots[50]`**
   and is a separate render-state global, not an off-by-one in the
   character-slot array. Paired with `dword_60222C = 393` (outside
   this wave's range) as a world-units-to-pixels scale.

5. **Most of the `dword_7D7Cxx..dword_7D8Fxx` cluster is MSVC CRT
   internal state** (sbh, mbcs, stdio, atexit). Not game state; named
   with conventional CRT names so they don't pollute future searches.

## Remaining work (follow-ups for future waves)

1. **Declare `KgtEditorTabEntry` struct** (144 bytes) and retype
   `g_windowClassBuffers` as `KgtEditorTabEntry[64]`. This would
   absorb all 26 remaining `unk_7D5xxx` auto-names and turn the 48
   `+128`/`+140` field names into proper struct member accesses.

2. **Cross-check `dword_7D7C48 / dword_60222C` pair in WonderfulWorld**:
   the editor's value 655 is a placeholder/default; the runtime game
   likely computes these from the screen dimensions.

3. **7 class-buffer slots (3, 5, 9, 12, 15, 18, 33, 43) appear unnamed
   by RegisterEditorWindowClasses** — the register sequence only hits
   ~55 of 64 entries. Investigate whether those slots are reserved,
   dynamically chosen, or simply unused in the editor build.

## See also

- `docs/editor/ida_progress.md` — global IDA state
- `docs/editor/runtime_entity.md` — `KgtRuntimeObject` + helpers in this region
- `docs/editor/skill_editor.md` §6 — AnimationPreviewWndProc
- `docs/editor/main_shell.md` §22..27 — slot-list HWND routing from HandleKeyPress
