# `game.ini` Reference (2D Fighter Maker 2nd / 2002 editor)

This document describes every section and key written to, and read from,
`2D Fighter  Maker 2002.ini` (the "game.ini" / workspace file used by
`KGT2nd_EDITOR.exe`). Behaviour was reverse-engineered from the shipped
editor binary; each entry below cites the IDA address of the
`GetPrivateProfileIntA` / `GetPrivateProfileStringA` call that reads it
and the address of the `WritePrivateProfileStringA` call that writes it.

## Scope and source of truth

* Binary: `KGT2nd_EDITOR.exe` (the 2002 "2nd" editor — the 2nd Manual
  version uses the same layout).
* INI filename is hard-coded at `.rdata:0x4461B4` — `"2D Fighter  Maker 2002.ini"`
  (note the two spaces) and is concatenated with the project directory
  at the top of every accessor (`sprintf(FileName, "%s\\%s", Directory, ...)`).
* All four top-level accessors live at the start of `.text` right after
  `BuildColorBlendTable`:
  * `ReadEditorSettingsFromIni`  — `.text:0x404AB0`
  * `WriteEditorSettingsToIni`   — `.text:0x404AF6`
  * `LoadProjectSettings`        — `.text:0x404B74`
  * `SaveProjectSettings`        — `.text:0x4052B8`
  * `SaveTestPlaySettings`       — `.text:0x405DA6`
  * `LoadTestPlaySettings`       — `.text:0x406044`
* The ptr-table of section names lives at `.rdata:0x4460AC`
  (offsets `+0x00..+0x30` enumerated below).
* Key-name ptr-tables for keybinds (`[EditorKey]`, 35 entries) and
  TestPlay (13 entries) live at `.rdata:0x445200` and `.rdata:0x446180`
  respectively.
* The runtime ports in `/mnt/c/dev/wanwan/2DFM_Player/` and
  `/mnt/c/dev/wanwan/Unity2dfmRuntime/` do **not** consume this file —
  they load `*.kgt` / `.2dg` data directly. `game.ini` is strictly
  editor workspace state.

### Section name table (`.rdata:0x4460AC`)

| Offset | String | Section header |
|--------|--------|----------------|
| `+0x00` | `0x4461B4` | *(filename)* `"2D Fighter  Maker 2002.ini"` |
| `+0x04` | `0x4461D0` | `[EditorKey]` |
| `+0x08` | `0x4461DC` | `[Win]` |
| `+0x0C` | `0x4461E0` | `[File]` |
| `+0x10` | `0x4461E8` | `[Screen]` (unused — see notes) |
| `+0x14` | `0x4461F0` | `[Etc]` |
| `+0x18` | `0x4461F4` | `[ActionScreen]` |
| `+0x1C` | `0x446204` | `[Palette]` |
| `+0x20` | `0x44620C` | `[Anime]` |
| `+0x24` | `0x446214` | `[Colors]` |
| `+0x28` | `0x44621C` | `[TestPlay]` |
| `+0x2C` | `0x446228` | `[Action]` |

`[Screen]` is declared at `.rdata:0x4460BC` but has **no code xrefs**
(`xrefs_to 0x4461E8 ==> only the table entry itself`). It is dead data
left from an earlier build; the editor never reads or writes it.

### How keybind values are encoded

Keybind DWORDs in `[EditorKey]` are packed as:

```
bits  0..15  : Win32 virtual-key code (VK_*)
bit     16   : Ctrl  modifier (0x10000)
bit     17   : Shift modifier (0x20000)
bit     18   : Alt   modifier (0x40000)
```

Evidence: `UpdateMenuState` at `.text:0x401AAC` rebuilds menu captions
from the table, OR-ing in `"Ctrl+" / "Shift+" / "Alt+"` when bits
`0x10000 / 0x20000 / 0x40000` are set, then passing the low 16 bits to
`VirtualKeyToString`. Default values are stored as 32-bit words at
`.rdata:0x44528C` — see `[EditorKey]` section below.

---

## `[File]`

Project file paths. Read via `GetPrivateProfileStringA`
(`.text:0x404BCD`, `0x404BF0`, `0x404C13`), written at `.text:0x40530A`,
`0x405326`, `0x405342`.

| Key | Type | Default | In-memory | Meaning |
|-----|------|---------|-----------|---------|
| `Filename` | string (path) | `""` | `String` (fn-buffer) | Path of the `.kgt` (project) file last opened. Empty when the editor is in "fresh workspace" state. |
| `BmpFilename` | string (path) | `""` | `PathName` | Path of the last-imported BMP sprite sheet (Bitmap window state). |
| `GameCreateDir` | string (path) | `""` | `byte_5F17F4` | Destination directory used by `Compile Game` — the folder into which the runtime player + compiled game files are written. |

Key strings live at `.rdata:0x446230 / 0x44623C / 0x446248`.

---

## `[Etc]`

Editor/workspace state: which action is selected, which view flags
are on, various preview-pane cursors. Read over
`.text:0x404C2F..0x404FF8`; written over `.text:0x40535B..0x4059A2`.

| Key | Type | Default | In-memory symbol | Meaning |
|-----|------|---------|------------------|---------|
| `Action_nb` | uint | `1` | `dword_602234` | Currently selected **action (skill) index** within the active character/stage/demo entry. Drives every `NavigateToEntryAndCell` call (`.text:0x4044F7`, `0x404569`, …). |
| `Action_step` | uint | `0` | `dword_601B80` | Currently selected **cell index (step)** inside the action. Used together with `Action_nb` to restore the caret in the timeline. |
| `PlayerNb` | uint | `0` | `dword_5F16C4` | Currently selected Player slot index (character roster cursor). Also read by `EnsureSlotLoaded` / `UpdateEditorUIState` at `.text:0x40392C / 0x404426`. |
| `PlayerActionTab` | uint | `0` | `dword_5F18F4` | Which **action-type tab** is active inside the Player action editor (Skill/Effect/etc. tab group). |
| `DemoNb` | uint | `0` | `dword_5F16AC` | Currently selected Demo slot index. |
| `StageNb` (written as `StageNb`? actually: see note) | uint | `0` | `dword_5F16B0` | Currently selected Stage slot index. Key name is `off_446110` — *not* `StageNb`; the Etc section stores it under an entry whose literal name is a continuation of `Stage` — see the `[Etc]` raw-string table below. |
| `PlayerPutMPreView_x` | int | `200` | `dword_4550A8` | X coordinate of the "Put M" preview window (where the editor draws the Player sprite test placement). |
| `PlayerPutMPreView_y` | int | `256` | `dword_4550AC` | Y coordinate for same. |
| `CommonImagePreView_x` | int | `200` | `dword_6075D0` | X coordinate of the Common-Image preview pane. |
| `CommonImagePreView_y` | int | `256` | `dword_6075D4` | Y coordinate for same. |
| `Frame` | uint (0/1) | `0` | `dword_5F1B8E` | View-mode radio (menu items `0xC1D/0xC1E`) — frame outline display mode. `0` = off, `1` = on (drawn by `UpdateMenuState` at `.text:0x401AAC`). |
| `NameWClick` | uint (bool) | `0` | `dword_5F1B92` | Menu item `0xC1F` — "show name on click" toggle in editor lists. |
| `ValueInputType` | uint | `0` | `dword_5F1B96` | Radio group at menu items `0xC20/0xC21` — numeric entry style (slider / spin) used by `CreateNumericEditControl` / `CreateNumericSpinControl` (`.text:0x43098A`, `0x43136A`). |
| `ChildWindow` | uint (bool) | `1` | `dword_5F1B9A` | Menu item `0xC22` — whether tab child windows are displayed (drives `CreateEditorTabWindow` at `.text:0x407C24`). |
| `FormImgSize` | uint (bool) | `0` | `dword_5F1B9E` | Menu item `0xC23` — sprite preview "fit form" scaling. Switches the 640×480 preview canvas to full-size 640×480 / scaled output in `SpritePreviewWndProc` (`.text:0x417644`, `0x4172C1`). |
| `AutoReaction` | uint (bool) | `1` | `dword_5F1BA2` | Menu item `0xC24` — auto-apply AutoReaction codeblocks (`.text:0x435B67` / `0x435DAB`, `HandleMenuCommand`). |

Key-string pointers (.rdata, in read order):
`0x4460F0="Action_nb", 0x4460F4="Action_step", 0x446100="System",
0x446104="PlayerNb", 0x446108="PlayerActionTab", 0x44610C="DemoNb",
0x446110="StageNb"`(stored under the label you see in the INI),
`0x446114="PlayerPutMPreView_x", 0x446118="PlayerPutMPreView_y",
0x44611C="CommonImagePreView_x", 0x446120="CommonImagePreView_y",
0x446144="Frame", 0x446148="NameWClick", 0x44614C="ValueInputType",
0x446150="ChildWindow", 0x446154="FormImgSize", 0x446158="AutoReaction"`.

---

## `[Action]`

Per-entry action-count tables. This is how the editor remembers how
many actions are stored in each Player/Stage/Demo slot so it can size
its timeline widgets on reopen. Loaded by
`LoadProjectSettings` at `.text:0x404C67..0x404D36`, saved by
`SaveProjectSettings` at `.text:0x4053C5..0x405500`.

| Key pattern | Type | Default | In-memory | Meaning |
|-------------|------|---------|-----------|---------|
| `System` | uint16 | `1` | `LOWORD(dword_5F19D8)` | Number of **System actions** (the built-in "System" entry — always at least 1). Key string: `.rdata:0x446100`. |
| `Player %d` (formatted `"Player 0"`, `"Player 1"`, …) | uint16 | `0` | `dword_5F19DA..word_5F1A3C` (one per slot) | Number of actions stored in Player slot `%d`. Saved only when `*v1 != 0`, i.e. slots that actually contain data. |
| `Stage %d` | uint16 | `0` | `word_5F1A3E..word_5F1AA0` | Number of actions in Stage slot `%d`. Saved only when `*v3 > 1` (the first action is the default empty one). |
| `Demo %d`  | uint16 | `0` | `word_5F1AA2..word_5F1B68` | Number of actions in Demo slot `%d`. Saved only when `*v5 > 1`. |

The index ranges are determined by the pointer walk (`while (v1 <
word_5F1A3E)` etc.) and correspond to the fixed slot-count arrays in
`KgtGame`: 50 player slots, 50 stage slots, 100 demo slots (same
layout as `/mnt/c/dev/wanwan/2dfm/KgtGame.hpp`).

---

## `[Win]`

Main editor window layouts — one record per multi-doc child ("Character
edit", "Stage edit", "Demo edit", etc.). Read at
`.text:0x405012..0x40510B`, written at `.text:0x4059B9..0x405B06`.

For each record `N` (`N = 0..5` observed in the default INI — the
save-loop `while (v9 < &dword_5F19D0)` iterates until it has emitted
every `tagRECT` slot):

| Key pattern | Type | Default | Meaning |
|-------------|------|---------|---------|
| `flag  %d` | uint | `0` | Window state flags (low bit = visible / maximised). Stored in `&dword_5F19D0 - 24 + 4` (decompiler shows `v10[-1].bottom`, which is the pre-RECT slot used as `flag`). |
| `left  %d` | int | `16 * N` | `tagRECT.left` (window X). |
| `right %d` | int | `16 * N + 420` | `tagRECT.right`. |
| `top   %d` | int | `16 * N` | `tagRECT.top`. |
| `bottom%d` | int | `16 * N + 340` | `tagRECT.bottom`. |

Note the key strings use **double-spaces** for padding
(`"flag  %d"`, `"left  %d"`, `"top   %d"`, `"right %d"`, `"bottom%d"`
— 6 chars each, no trailing space after `"bottom"`). Pointer: `off_4460B4 = "Win"`.

### Tab cursors (also under `[Win]`)

Loaded by the second loop at `.text:0x405127..0x405156`, saved at
`.text:0x405B1E..0x405B9A`:

| Key pattern | Type | Default | In-memory | Meaning |
|-------------|------|---------|-----------|---------|
| `tab   %d` | uint | `0` | `dword_7D528C + 36*i` | **Selected-cell cursor per tab** in the action editor. `%d` ranges up to `(dword_7D768C - &dword_7D528C) / 36`. Only written when either the current value or the existing value in the INI is non-zero (the save-loop reads-then-writes so zeros aren't flushed, keeping the file sparse). |

---

## `[ActionScreen]`

The editor's "Action screen" (where sprites are composed) remembers a
camera / cursor origin per entry type. Loaded at
`.text:0x404E35..0x404EFB`, saved at `.text:0x4056A3..0x405820`.

| Key | Type | Default | In-memory | Meaning |
|-----|------|---------|-----------|---------|
| `Player_x` | int | `200` | `dword_5F1B6A` | Action-screen X origin when editing a **Player** action. |
| `Player_y` | int | `200` | `dword_5F1B6E` | Action-screen Y origin (Player). |
| `Obj_x`    | int | `200` | `dword_5F1B72` | Action-screen X origin when editing an **Object** (Hit Spark / Effect) action. |
| `Obj_y`    | int | `200` | `dword_5F1B76` | Action-screen Y origin (Object). |
| `Stage_x`  | int | `0` | `dword_5F1B82` | Action-screen X origin for Stage editing. |
| `Stage_y`  | int | `0` | `dword_5F1B86` | Action-screen Y origin for Stage editing. |
| `Demo_x`   | int | `0` | `dword_5F1B7A` | Action-screen X origin for Demo editing. Consumed by `SpritePreviewWndProc` at `.text:0x417650..0x4176D4`. |
| `Demo_y`   | int | `0` | `dword_5F1B7E` | Action-screen Y origin (Demo). |

Key-string pointers: `off_446124="Player_x", 0x446128="Player_y",
0x44612C="Obj_x", 0x446130="Obj_y", 0x446134="Stage_x",
0x446138="Stage_y", 0x44613C="Demo_x", 0x446140="Demo_y"`.

---

## `[Palette]`

User's saved **custom colours** dialog (the 16-slot row at the bottom
of Win32's common `ChooseColor` control). Loaded at
`.text:0x404F15..0x404F56`, saved at `.text:0x405837..0x405876`, and
passed to `ChooseColorA` via `lpCustColors = &unk_455060` in
`ChooseColorForPalette` (`.text:0x435896`).

| Key | Type | Default | In-memory | Meaning |
|-----|------|---------|-----------|---------|
| `0` .. `15` | `COLORREF` (0x00BBGGRR) stored as unsigned decimal | `0x101010 * N` (grey ramp — e.g. `0=0, 1=1052688, … 15=15790320`) | `unk_455060[N] .. byte_4550A0` | Slot `N` of the Windows custom-colour array. |

Default computation at the GetPrivateProfileInt call site:
`(16*v6) | (((16*v6) | ((16*v6) << 8)) << 8)` (`.text:0x404F4A`),
which is exactly the `0x101010 * N` grey ramp you see in the sample file.

---

## `[Anime]`

Animation-preview window state. Coordinate pairs per preview slot
plus a handful of scalars. Loaded at `.text:0x40516D..0x40524D`,
saved at `.text:0x405BB5..0x405D36`. Consumed by
`AnimationPreviewWndProc` (`.text:0x42EBF0..0x42F5A0`).

Coordinate pairs (loop `i = 0..3`, i.e. 4 slots):

| Key pattern | Type | Default | In-memory | Meaning |
|-------------|------|---------|-----------|---------|
| `%d x` | int | `0` | `dword_7D51C4 + 8*i - 4` | X origin of animation-preview slot `i`. |
| `%d y` | int | `0` | `dword_7D51C4 + 8*i`     | Y origin of animation-preview slot `i`. |

Scalar state:

| Key | Type | Default | In-memory | Meaning |
|-----|------|---------|-----------|---------|
| `kotei` | uint (bool) | `0` | `dword_62ED40` | "固定" — when on, the animation-preview origin is **locked** to a fixed screen position rather than tracking the sprite. Also initialised by `_WinMain@16` at `.text:0x402F64`. |
| `Frame` | uint (bool) | `0` | `dword_62ED44` | Whether the animation-preview window draws the axis / bounding-frame overlay. Same `Frame` semantics as the `[Etc]` one but for the Anime pane specifically. |
| `Sound` | uint (bool) | `1` | `dword_62ED48` | Whether sound effects are played during animation preview (`AnimationPreviewWndProc` at `.text:0x42EC2C / 0x42F59F`). |
| `Size1` | uint (bool) | `0` | `dword_62ED5C` | Preview zoom flag #1 — clamps the visible area via `ClampPreviewPosition` (`.text:0x42EA1F`). |
| `Size2` | uint (bool) | `0` | `dword_62ED60` | Preview zoom flag #2 (companion to `Size1` — together they form a 2-bit zoom-level). |

Key-string pointers: `off_44615C="kotei", 0x446160="Frame",
0x446164="Sound", 0x446168="Size1", 0x44616C="Size2"`.

---

## `[Colors]`

Eight UI overlay colours used by the **sprite preview window**
(`SpritePreviewWndProc` at `.text:0x417624`, `0x417889`, `0x417A8B`,
`0x417C65..0x417D6C`; also `BmpCanvasWndProc` `.text:0x42C134` and
`AnimationPreviewWndProc` `.text:0x42EF3C / 0x42F119`). Loaded at
`.text:0x405262..0x405296`, saved at `.text:0x405D4D..0x405D8E`.

Values are stored as **unsigned 16-bit** `0xRRRRRGGG GGGBBBBB` colours
(5-5-5 BGR — see `ChooseColorForPalette` at `.text:0x4358F7` which
converts between `COLORREF` and the 15-bit packed form via
`(b>>3) | ((g>>3) << 5) | ((r>>3) << 10)`).

| Key | Default (decimal / packed) | Default (R,G,B) | In-memory | Meaning |
|-----|-----|-----|-----|-----|
| `0` | `10570` / `0x294A` | (10, 10, 82) dark blue | `word_5F1684` | **Background fill** of the sprite preview window. First thing `SpritePreviewWndProc` pushes into `FillFramebufferRect` (`.text:0x417624`). |
| `1` | `8456`  / `0x2108` | (8, 8, 64) | `word_5F1686` | **Secondary background** / frame outside preview canvas. Used by `BmpCanvasWndProc` (`.text:0x42C134`). |
| `2` | `31710` / `0x7BDE` | (30, 30, 246) bright blue | `word_5F1688` | Player/obj **centre axis crosshair** colour (used by the `FillFramebufferRect` calls at `.text:0x4176AD / 0x417714`). |
| `3` | `30819` / `0x7863` | (30, 3, 99) | `word_5F168A` | Secondary axis / ghost crosshair. |
| `4` | `3198`  / `0x0C7E` | (12, 3, 190) | `word_5F168C` | Hit-box fill colour (codeblock `[C]` collision box). |
| `5` | `3872`  / `0x0F20` | (15, 9, 0) | `word_5F168E` | Hurt-box fill colour (body box). Used as first source in `BuildColorBlendTable` at `.text:0x404A74` for 2-box blending. |
| `6` | `3198`  / `0x0C7E` | (12, 3, 190) | `word_5F1690` | Throw-box / secondary collision fill. Used as second source in `BuildColorBlendTable`. |
| `7` | `3742`  / `0x0E9E` | (14, 6, 190) | `word_5F1692` | Third box colour; combined with `5` / `6` into an 8-entry blend table by `BuildColorBlendTable` at `.text:0x4049A5` so overlapping boxes render as an averaged colour. |

Defaults live as a 16-byte little-endian `uint16[8]` at `.rdata:0x446170`
(`nDefault` in IDA). These are the same values emitted by the
first-run path in `LoadProjectSettings` (`.text:0x405262`).

---

## `[EditorKey]`

All 35 editor keyboard shortcuts. Read/written by the dedicated
`ReadEditorSettingsFromIni` (`.text:0x404AD7`) and
`WriteEditorSettingsToIni` (`.text:0x404B65`) helpers, which simply
loop over `i = 0..34`:

```c
// Read   .text:0x404AD7
dword_7D5100[i] = GetPrivateProfileIntA("EditorKey",
                                         off_445200[i],       // key-name
                                         dword_44528C[i],     // default
                                         FileName);
// Write  .text:0x404B65
wsprintfA(buf, "%u", dword_7D5100[i]);
WritePrivateProfileStringA("EditorKey", off_445200[i], buf, FileName);
```

`off_445200[]` is the key-name table at `.rdata:0x445200` (35 × 4 bytes
of string pointers) and `dword_44528C[]` is the default-value table at
`.rdata:0x44528C` (35 × DWORD).

Values are packed as **`VK | (Ctrl<<16) | (Shift<<17) | (Alt<<18)`**
(see "How keybind values are encoded" above). The `dword_7D5100[]`
array also feeds `UpdateMenuState` (`.text:0x401B44..0x401BF2`) which
rewrites every menu caption with the current binding.

| # | INI key | Default (decimal / decoded) | Intent |
|---|---------|-----------------------------|--------|
| 0 | `Action*script compilation` | `45` = `VK_INSERT` | Compile current action's script. |
| 1 | `Action*Script Deleteion` | `46` = `VK_DELETE` | Delete current action's compiled script. |
| 2 | `Copy` | `65603` = `0x10043` = Ctrl+C | Copy selected element. |
| 3 | `Paste` | `65622` = `0x10056` = Ctrl+V | Paste. |
| 4 | `Cut` | `65624` = `0x10058` = Ctrl+X | Cut. |
| 5 | `Unknown` | `65613` = `0x1004D` = Ctrl+M | Unnamed action (literal INI key is `"Unknown"`; target command not labelled in the binary). |
| 6 | `Change Action Up` | `104` = `VK_NUMPAD8` | Previous action. |
| 7 | `Change Action Dwn` | `98`  = `VK_NUMPAD2` | Next action. |
| 8 | `Action step Left` | `100` = `VK_NUMPAD4` | Previous cell/step in action. |
| 9 | `Action Step Right` | `102` = `VK_NUMPAD6` | Next cell. |
| 10 | `Up 10 Actions` | `33` = `VK_PRIOR` (Page Up) | Jump 10 actions up. |
| 11 | `Down 10 Actions` | `34` = `VK_NEXT`  (Page Down) | Jump 10 actions down. |
| 12 | `To Begining of skill` | `97`  = `VK_NUMPAD1` | Jump to first cell of the skill. |
| 13 | `To End of Skill` | `99`  = `VK_NUMPAD3` | Jump to last cell. |
| 14 | `Call Bmp Window` | `112` = `VK_F1` | Focus the Bitmap (sprite sheet) window. |
| 15 | `Call Pallet Window` | `113` = `VK_F2` | Focus the Palette window. |
| 16 | `Call Ani Window` | `114` = `VK_F3` | Focus the Animation preview window. |
| 17 | `Call Skill List` | `115` = `VK_F4` | Focus the Skill list. |
| 18 | `Tab Back` | `49` = `'1'` | Previous main tab. |
| 19 | `Tab Forward` | `50` = `'2'` | Next main tab. |
| 20 | `Sub Tab Back` | `81` = `'Q'` | Previous sub-tab. |
| 21 | `Sub Tab ->` | `87` = `'W'` | Next sub-tab. |
| 22 | `Character up` | `65` = `'A'` | Previous character slot. |
| 23 | `Character Down` | `90` = `'Z'` | Next character slot. |
| 24 | `Stage up` | `65` = `'A'` | Previous stage slot. *(Shares the default with "Character up" because they belong to different modal contexts — the binding is only active while the Stage editor is focused.)* |
| 25 | `Stage Down` | `90` = `'Z'` | Next stage slot. |
| 26 | `Demo Up` | `65` = `'A'` | Previous demo slot. |
| 27 | `Demo DW` | `90` = `'Z'` | Next demo slot. |
| 28 | `New Game` | `65614` = `0x1004E` = Ctrl+N | File → New. |
| 29 | `Open` | `65615` = `0x1004F` = Ctrl+O | File → Open. |
| 30 | `Save` | `65619` = `0x10053` = Ctrl+S | File → Save. |
| 31 | `Save As` | `65601` = `0x10041` = Ctrl+A | File → Save As. *(Collides visually with "Character up" = 'A' — that binding lives on a different menu owner handle so both coexist.)* |
| 32 | `Compile Game` | `65607` = `0x10047` = Ctrl+G | Game → Compile. |
| 33 | `Test Play` | `65616` = `0x10050` = Ctrl+P | Game → Test Play. |
| 34 | `Quit` | `65617` = `0x10051` = Ctrl+Q | File → Quit. |

The padding spaces inside the key names (`"Copy                         "`)
are literally stored in the string table at `.rdata:0x4459C0` and
onward — the editor preserves them on round-trip because it uses them
verbatim as `lpKeyName` arguments.

---

## `[TestPlay]`

Remembers the last-used Test Play dialog settings. Read by
`LoadTestPlaySettings` (`.text:0x40606E`), written by
`SaveTestPlaySettings` (`.text:0x405DFE`).

Note that the **key strings** are fully-qualified
`"Editor.TestPlay.*"` identifiers (they live at `.rdata:0x4463F0` ff.),
even though the **section** they're written under is just `[TestPlay]`
(section pointer `off_4460D4 → 0x446530 "TestPlay"`).

The default `.ini` in `/mnt/c/dev/wanwan/2D Fighter  Maker 2002.ini`
has no `[TestPlay]` block — it's only created after the user opens
Test Play once and the `Save` button is pressed.

| Key | Type | Default | In-memory | Meaning |
|-----|------|---------|-----------|---------|
| `Editor.TestPlay.Player0.nb`  | uint | `0`  | `dword_5F1324` | Player 1 character-slot index to load in Test Play. |
| `Editor.TestPlay.Player0.cpu` | uint (bool) | `0` | `dword_5F1328` | Player 1 CPU-controlled flag (0 = human, 1 = CPU). |
| `Editor.TestPlay.Player1.nb`  | uint | `0`  | `dword_5F132C` | Player 2 character-slot index. |
| `Editor.TestPlay.Player1.cpu` | uint (bool) | `0` | `dword_5F1330` | Player 2 CPU flag. |
| `Editor.TestPlay.GameSpeed`   | uint | `10` | `uValue`       | Game speed / frame pacing (engine uses this as a tick divider — the FM2K runtime's 100 FPS base tick is scaled by this value; see `uValue` xrefs in the runtime). |
| `Editor.TestPlay.HitJudge`    | uint (bool) | `0` | `uCheck` | Whether to draw hit-box overlays during Test Play. |
| `Editor.TestPlay.GameInformation` | uint (bool) | `0` | `dword_5F133C` | Whether to display debug HUD (Action #, step, position, …). |
| `Editor.TestPlay.StageNb`     | uint | `0`  | `dword_5F1340` | Stage slot index to load. |
| `Editor.TestPlay.way`         | uint | `0`  | `dword_5F134C` | Player facing / "direction" preset. Loaded *fifth-from-last* in the read order despite its lower `off_4461A0` address — see the call at `.text:0x40616D`. |
| `Editor.TestPlay.JoyStick`    | uint (bool) | `1` | `dword_5F1344` | Whether joystick / gamepad input is enabled (default ON). |
| `Editor.TestPlay.time`        | uint | `60` | `dword_5F1350` | Round timer (seconds). |
| `Editor.TestPlay.exit`        | uint | `0`  | `dword_5F1354` | "Exit to editor after match" auto-flag. |

Save-order (`.text:0x405DFE..0x406026`) and load-order
(`.text:0x40607A..0x406197`) are the same and can be used as the
canonical sequence when hand-editing the file.

---

## Data-file on disk vs. in-memory

The sample file shipped with this repo
(`/mnt/c/dev/wanwan/2D Fighter  Maker 2002.ini`) shows the editor in
its **freshly-opened** state — `PlayerNb=0`, `DemoNb=0`, `StageNb=0`,
all `[File]` strings empty, palette at the default grey ramp, no
`[TestPlay]` section yet. Compare with the defaults column in each
table above; every field matches.

## Cross-references

* Editor state → runtime: **none**. The runtime reads `.kgt`
  (character), `.sta` (stage), `.dem` (demo) files directly from the
  compiled output directory and does not open `game.ini`. Verified by
  searching `/mnt/c/dev/wanwan/2DFM_Player/Source` and
  `/mnt/c/dev/wanwan/Unity2dfmRuntime/Assets` for
  `GetPrivateProfile` / literal INI keys — zero hits.
* In-memory counts → on-disk structs: the `[Action]` per-slot action
  counts mirror the `actionCount` fields the Hong Kong dev's C++
  reference defines in `/mnt/c/dev/wanwan/2dfm/KgtPlayer.hpp`,
  `KgtStage.hpp`, `KgtDemo.hpp`. They're redundant with what `.kgt`
  already stores — the editor persists them here purely to size
  timeline widgets before the file has been re-parsed.
* Codeblock semantics referenced by `[Colors][4..7]` (collision-box
  fill colours) are documented in `/mnt/c/dev/wanwan/docs/editor/
  2DFM Codeblocks.md` under `[C] – Collision box`.
* Keybind menu-ID constants (`0xBC5`, `0xCBC`, `0xC1D..0xC24`) live in
  `UpdateMenuState` at `.text:0x401A66..0x401BF8`.

## Summary table of unknowns

| Entry | Status | Evidence |
|-------|--------|----------|
| `[Screen]` section | **Dead code** | Section-name pointer at `.rdata:0x4460BC` has no code xrefs. |
| `EditorKey → Unknown` | **Opaque name** | Literal key name in the binary is `"Unknown"`; default `Ctrl+M` (`0x1004D`). Points into the editor command dispatcher but the surrounding table doesn't label it — the command is real, just never given a display name. |
| `EditorKey` padding spaces | **Intentional** | Strings stored with trailing spaces in the `.rdata` table; binary uses them verbatim as keys. |
