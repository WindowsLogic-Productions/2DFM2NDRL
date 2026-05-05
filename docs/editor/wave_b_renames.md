# Wave B Rename Report — `.data` 0x5F1000..0x620000

Session cleanup of the editor-state / AI-helper region. **119 auto-named globals enumerated, 115 renamed, 0 left**.

## Summary

| | Before | After |
|---|---|---|
| Auto-named globals (`dword_`, `word_`, `byte_`, `unk_`) in range | 115 | 0 |
| User-named globals in range | 57 | 172 (+115) |
| Types applied | 0 | 8 (HFONT × 3, HBITMAP, HGDIOBJ, int × 2, uint) |
| Disasm/pseudocode comments added | 0 | 33 |

No `__dword_<addr>` fallbacks; every global in the Wave B range now has a semantic name.

## Clusters identified

### 1. Editor font handles (0x5F1BAC..0x5F1BB4)
`InitializeEditorResources@0x401000` creates five `HFONT`s via `CreateFontA` with Shift-JIS face-name strings. `dword_5F1BAC` (66 xrefs — the most-referenced global in the entire range) is the main editor label font (MS PGothic 13×5 normal weight). `dword_5F1BB0` is the static-label font (MS UI Gothic 15×6 normal). `dword_5F1BB4` is the bold header font (MS PGothic 13×6 weight 700). The two pre-named oddities `wParam@0x60741C` (italic variant used by MainWndProc/tab containers) and `ho@0x5F1BA8` (used by SlotListWndProc) complete the set — not renamed because already user-named.

**Renames:** `g_editorLabelFont`, `g_editorStaticFont`, `g_editorBoldFont`.

### 2. Test-play settings (0x5F1324..0x5F1354)
Twelve `int` settings written by `SaveTestPlaySettings@0x405DA0` under the `[TestPlay]` INI section. Keys recovered from the `off_4460Dx` table: `Editor.TestPlay.{Player0.nb, Player0.cpu, Player1.nb, Player1.cpu, GameSpeed, HitJudge, GameInformation, StageNb, way, JoyStick, time, exit}`. `uValue`/`uCheck` at 0x5F1338/0x5F1334 map to GameSpeed/HitJudge (already user-named — left alone).

**Renames:** `g_testPlay_player0Slot/Cpu`, `g_testPlay_player1Slot/Cpu`, `g_testPlay_showGameInfo`, `g_testPlay_stageNb`, `g_testPlay_joyStick`, `g_testPlay_time`, `g_testPlay_exit`, `g_testPlay_demoIdx`.

### 3. Per-mode cursor cache (0x5F19D8..0x5F1B6A)
`SaveCursorPositionForTab@0x4047A0` packs `(skillIdx | (cellIdx<<8))` into a `word[]` array split by editor mode: [0x5F19D8..0x5F19DA]=system, [0x5F19DA..0x5F1A3E]=50 player slots, [0x5F1A3E..0x5F1AA2]=50 stage slots, [0x5F1AA2..0x5F1B6A]=100 demo slots. Matches the `PlayerNb`/`Player %d`/`Stage %d`/`Demo %d` INI keys in `LoadProjectSettings`.

**Renames:** `g_cursorCache_system`, `g_cursorCache_stagesBegin`, `g_cursorCache_demosBegin`.

### 4. Preview origin (action-screen) coordinates (0x5F1B6A..0x5F1B86)
Eight `int` preview-origin positions — one X/Y pair per editor mode {player, object, demo, stage}. INI keys `[ActionScreen]`/`Player_x`, `Player_y`, `Obj_x`, `Obj_y`, `Demo_x`, `Demo_y`, `Stage_x`, `Stage_y`. Defaults all 200/200 (x) 0 (y).

**Renames:** `g_previewPos_playerX/Y`, `g_previewPos_objectX/Y`, `g_previewPos_demoX/Y`, `g_previewPos_stageX/Y`.

### 5. Cached KgtProjectSlot pointers (0x5F1CD0..0x5F1CE0 and 0x602350..0x602360)
**Surprise: two parallel cached-pointer blocks.** Both hold base pointers into the currently-previewed `KgtProjectSlot`'s scripts / scriptItems / pictureHeaders / sharedPalettes / soundHeaders. 0x5F1CD0 set is populated by the `g_projectSlot` path in `SetupAnimationPreview`; 0x602350 set is populated by the `g_characterSlots[i]` path (player-slot preview). Both are read by inner render/opcode loops to avoid repeated +0x110/+0x9D10/etc offset math.

**Renames:** `g_preview_{scripts,scriptItems,pictureHeaders,sharedPalettes,soundHeaders}Ptr` and `g_preview2_*` variants.

### 6. Color-blend effect states (0x601B54 and 0x602381)
**Surprise: two independent color-blend states.** `UpdateColorBlendEffect@0x437540` takes an 11-dword struct `{mode, r, g, b, a, duration, initialR, initialG, initialB, initialA, timer}`. One instance is fed by the `[COLOR]` opcode at ~0x43AF44; a second mirror instance at 0x602381 is fed by a different dispatcher branch at 0x43AEDA. Fields map 1:1 between the two — probably primary (self) vs. secondary (target/opponent) tint.

**Renames:** `g_colorBlendStateA[...]` (11 fields) and `g_colorBlendStateB[...]` (11 fields).

### 7. Screen-shake effect (0x6023AD and 0x6023C1) — NEW DISCOVERY
`UpdateScreenShakeEffect@0x437460` handles 5-dword structs `{mode, offset, magnitude, timer, period}` with modes 1–4 (decay-to-zero, ramp-up, constant, random). Two parallel structs: X-shake and Y-shake (20 bytes apart). `RenderAllGameObjects` calls both each frame and applies the resulting `offset` fields to the global `g_cameraOffsetX/Y`. `g_frameCounter & 1` flips the sign of the shake output on even frames. Fed by opcode at 0x43AFB9 (likely a `[SH]`/`[VS]` screen-shake dispatcher case).

**Renames:** `g_screenShakeX_{mode,offset,magnitude,timer,period}`, same for `Y`, plus `g_cameraOffsetX/Y`, `g_frameCounter`.

### 8. Skill/cell clipboards (0x6023E0, 0x6021A0)
`CopySkillToClipboard@0x42AA60` uses stride 4108 bytes × 4 editor modes → `g_skillClipboard` with items starting at `g_skillClipboardItems+0x0C`. `CopySkillCellToClipboard@0x429940` uses stride 28 bytes × 4 modes → `g_cellClipboard` with 16-byte `KgtScriptItem` payload at +0x0C (`g_cellClipboardItems`).

**Renames:** `g_skillClipboard/Items`, `g_cellClipboard/Items`.

### 9. Afterimage pool phantom slot (0x606F90..0x606F9F)
Already documented in `runtime_entity.md` — `g_afterimagePool[-1]` addressed by `DestroyGameObject` to resolve 1-based `afterimageSlot` values. Four fields parallel to the `KgtAfterimageEntry` layout (active, state, scriptItemPtr, reserved).

**Renames:** `g_afterimagePool_phantomSlot0_{,state,scriptItemPtr,reserved}`; also named matching slot-0 fields at 0x6075E4/E8/EC.

### 10. Render framebuffer DIBSection (0x602168..0x602184)
`CreateRenderSurfaceFromWindow@0x406B20` allocates a 16-bit DIBSection framebuffer. `hdc@0x602168` (pre-named), `g_renderBitmap@0x60216C`, `g_renderBitmapOriginal@0x602170` (the HGDIOBJ returned by SelectObject). `g_frameCounter@0x602184` ticks each render pass.

### 11. ActionEditor column guides (0x5F18F8..0x5F1910)
Seven dwords hardcoded in `ActionEditorMainWndProc`: {1, 22, 28, 80, 132, 156, 1024}. Column/field-separator positions for the per-cell property panel layout.

**Renames:** `g_actionEditor_col0..col6`.

### 12. Editor tab entries table (0x5F1938..0x5F19C8)
6-entry × 24-byte table walked by `CheckEditorTabHasFocus@0x401C20`. Parallel to `unk_7D6A20[]` (window HWNDs, stride 144) — `+0*6` field indicates tab visibility; the looping loop compares `GetFocus()` against each visible tab's HWND to set `g_hasEditorTabFocus`.

**Renames:** `g_editorTabEntriesTable`, `g_editorTabEntriesEnd`.

### 13. Preview common-image preview coords (0x6075D0..0x6075D4)
`CommonImagePreView_x/y` INI keys at `[Etc]` — the "common-image list" preview window origin for the throw-reaction browser. Defaults 200/256.

**Renames:** `g_preview_commonImageX/Y`.

### 14. BMP canvas cursor state (0x5F16B8..0x5F16C0)
Three dwords tracked by `BmpCanvasWndProc`: cursor X, cursor Y, and a flag (likely "cursor visible" or "dragging"). Paired with pre-named `g_currentEditorSlot`/etc in the editor-state register cluster.

**Renames:** `g_bmpCanvas_cursorX/Y/Flag`.

### 15. Color-blend lookup table (0x5F168E..0x5F1694)
`BuildColorBlendTable@0x4049A0` populates `g_blendLookupTable` (256 words) by averaging `g_blendPrimaryColor` (RGB555) + `g_blendSecondaryColors` (two-channel packed RGB555) for each 8-bit blend mask. Used by RenderGameObject at 0x43831C/0x4383D0 and SpritePreviewWndProc for translucent-sprite compositing.

**Renames:** `g_blendPrimaryColor`, `g_blendSecondaryColors`, `g_blendLookupTable`.

### 16. Fixed-point scale constants (0x60222C, 0x602230)
Initialized by `InitializeRenderState`: `g_waitScaleConstant=100`, `g_moveScaleConstant=393`. Multiplied against `[I]` wait counters and `[M]` move/accel packed-byte values respectively. (A third constant, `dword_7D7C48=655`, lives outside Wave B range.)

## Surprises

1. **Two parallel color-blend states (A at 0x601B54, B at 0x602381).** The opcode dispatcher has two separate 11-dword struct writers — likely primary vs. secondary target color blending (opponent-tint effect). Not yet observed in the bsnes-reference or our rollback logic.
2. **Dedicated screen-shake system.** Two 5-dword structs (X/Y) with four interpolation modes, frame-sign-flipping, and global camera offsets. The `[SH]`-style opcode populating these wasn't documented — cross-check with `opcode_dispatcher.md` needed.
3. **Font `dword_5F1BAC` is the second-most-referenced global in the whole binary** (66 xrefs), behind only `g_mainHwnd` (241) and `g_currentEditorMode` (86). Every WndProc that renders text touches it.
4. **"Phantom slot -1" trick** (already documented for afterimages) has a literal field layout — the four dwords at 0x606F90/94/98/9C mirror `KgtAfterimageEntry` fields exactly because the pool is addressed via `base + 1616 * slot - 1616`.
5. **Two cached-pointer blocks** (0x5F1CD0 and 0x602350) hold the same 5 KgtProjectSlot base pointers for different preview contexts.
6. **Global frame counter at 0x602184** is used not just for render timing but also for sign-flipping in shake/strobe effects — replacing this with a monotonic counter would be a candidate rollback-state field (game-state determinism).

## Files affected

- **IDB renames:** 115 globals in `.data` range `0x5F1000..0x620000`
- **IDB type annotations:** 8 (HFONT × 3, HBITMAP, HGDIOBJ, int × 2, uint)
- **IDB disasm/pseudo comments:** 33 (key global usage explanations)
- **Docs updated:** this file (`/mnt/c/dev/wanwan/docs/editor/wave_b_renames.md`)

## Follow-ups

- Cross-reference the opcode at 0x43AFB9 with `opcode_dispatcher.md` to name the screen-shake opcode (probably `[SH]`, `[VS]`, or similar — Hantei4 docs have "screen shake" concepts).
- The A/B color-blend states may be per-opcode rather than primary/secondary; verify by finding which opcodes write each set.
- `g_actionEditor_col0..col6` values {1,22,28,80,132,156,1024} are suspiciously cliff-edge — likely pixel positions of the status bar columns; confirm by watching the status bar during editor sessions.
- `g_animPreviewRunningFlag@0x5F196C` has only 1 xref in `_WinMain@16` (the message loop throttle). Its setter is elsewhere — needs tracking when user pauses the preview.
