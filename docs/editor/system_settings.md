# KGT2nd_EDITOR — Game System Settings, Stage & Demo Editors, Test-Play

Reverse-engineered by Agent 6 from `KGT2nd_EDITOR.exe`. Covers the project-wide
`KgtGameSystemData` block (66,108 bytes), the 1,033-byte stage/demo extensions,
the test-play dialog, and all the WndProcs that bind controls to those fields.

Cross-references to 厉猛's C++ port in `2dfm/` where applicable.

## 1. `g_kgtGameSystemData` — Revised Layout (66,108 bytes)

`g_kgtGameSystemData` lives at absolute address `0x765594`. It is loaded in one
`ReadFile(..., 0x1023C, ...)` call at the end of `ReadCommonResourcePart`, and
written symmetrically in `SaveProjectFile` / `WriteCommonResourcePart`.

The struct has been redeclared in IDA with fully-typed fields (total size
still 66,108):

| Offset | Size  | Field                       | Source (`2dfmFile.hpp` / `KgtGame.hpp`) |
|-------:|------:|-----------------------------|-----------------------------------------|
| `0x00000` | 12,800 | `KgtNameInfo playerNames[50]`         | `KgtGame::playerNames` |
| `0x03200` | 7,200  | `KgtReactionItem reactionItems[200]`  | `KgtGame::reactions`   |
| `0x04E20` | 1      | `byte unknownByte0` (default **2**)  | purpose unknown — initialised by `InitializeCharacterSlot@0x4082c6` but no read path currently found. Possibly legacy field. |
| `0x04E21` | 3      | `byte unknownPad1_3[3]`              | zero-filled |
| `0x04E24` | 4      | `KgtRecoverTimeConfig recoverTimeConfig` | `{ gap, attackRecoverTime, defenceRecoverTime, clashRecoverTime }` defaults `{0,30,30,50}` |
| `0x04E28` | 12,800 | `KgtNameInfo stageNames[50]`          | `KgtGame::stageNames`  |
| `0x08028` | 25,600 | `KgtNameInfo demoNames[100]`          | `KgtGame::demoNames`   |
| `0x0E428` | 8      | `KgtGameDemoConfig demoConfig`        | title/story/1v1/team/continue/opening demo IDs + 2 tag bytes |
| `0x0E430` | 4      | `KgtProjectBaseConfig projectBaseConfig` | bitfield, see §1.1 |
| `0x0E434` | 6,400  | `KgtThrowReaction commonImageScripts[200]` | **editor calls these "Common Images"**, 2dfmFile.hpp calls them `ThrowReaction` — 32-char names for reusable script slots (hit symbols, numbers, stage UI, cursors, etc.) |
| `0x0FD34` | 208    | `uint16 predefinedScriptIds[104]`    | maps fixed slot-index → `KgtScript` array index — see §1.2 |
| `0x0FE04` | 56     | `byte predefinedPad[56]`              | reserved/unused (within the 264-byte block; no xrefs) |
| `0x0FE3C` | 28     | `KgtCharSelectConfig charSelectConfig` | select-screen geometry (14 × int16) |
| `0x0FE58` | 50     | `byte playerSelectableInfos[50]`      | per-slot selectability |
| `0x0FE8A` | 946    | `byte trailingPadding[946]`           | no xrefs; slack |

Total = 66,108 = `0x1023C`. The **key correction** vs. the prior baseline: the
old `unknownBlock1[8]` is actually a `byte + byte[3] + KgtRecoverTimeConfig`
pair, and the old `positionData[264]` is actually a `uint16[104]` followed by
56 bytes of padding.

### 1.1 `KgtProjectBaseConfig` bit mapping (verified from `BasicRuleTabWndProc`)

`BasicRuleTabWndProc@0x4093E0` checkbox IDs vs. bit index:

| Bit | Check ID | Label                         | `KgtGame::projectBaseConfig` field |
|----:|---------:|-------------------------------|------------------------------------|
| 0 | 2316 | "offset" (group `0x908`)       | `pressToStart`       |
| 1 | 2312 | "Use Story Mode"               | `allowClash`         |
| 2 | 2313 | "Use VS Mode"                  | `enableStoryMode`    |
| 3 | 2314 | "Use VS Team Mode"             | `enable1V1Mode`      |
| 4 | 2315 | "The editor won't read…"       | `enableTeamMode`     |
| 5 | 2317 | "Numbers are shown"            | `showHpAfterHpBar`   |
| 6 | 2318 | "Cursor stays until…"          | (extra flag not in C++ port) |

Note the 1→1 mapping to `ProjectBaseConfig.value` isn't by bit-position; the
C++ port declares the bits in different order. The bit values above are what
the binary actually writes via `IsDlgButtonChecked(...) << bit`.

`InitializeCharacterSlot@0x4083a3` forces bits 2|3|4 on new projects
(`|= 0x1C`) — i.e. story + 1v1 + team modes default to enabled.

### 1.2 `predefinedScriptIds[104]` — the script-slot cross-reference table

**This is the reversed `positionData`.** It's a parallel array to the
editor's internal "predefined scripts" table at `off_446910` (a
`{char *name; uint8 flag}` record packed on a 5-byte stride — 104 records,
520 bytes ending at `dword_446B18`).

On every project load, `OpenProjectFile@0x408ca4` iterates:

```c
positionData = g_kgtGameSystemData.predefinedScriptIds;
v12 = &off_446910;
do {
    sprintf(g_projectSlot.scripts[*positionData].scriptName, "%s", *v12);
    v12 += 5;            // next 5-byte record
    positionData += 2;   // next uint16
} while (v12 < &off_446B18);
```

So `predefinedScriptIds[i]` = the `KgtScript` index that holds the predefined
animation for slot `i`. On a brand-new project `InitColorAndRenderBuffers@0x4081b6`
sets `predefinedScriptIds[i] = i` (identity mapping) and pre-populates the
scripts via `AddNewSkillEntry` using the flag byte from the `off_446910` record.

The 104 slot meanings are (verified via name strings in `.rdata`):

| Slot idx | KgtScript.flags raw | Name in editor                | C++ field in `KgtGame`                    |
|:--------:|:-------------------:|-------------------------------|-------------------------------------------|
| 0        | 0                   | "------None------"            | (reserved / "none")                        |
| 1        | 0x14                | "Hit Letter \"Hit\""          | `comboHitScriptId`                         |
| 2–11     | 0x04                | "Hit Number 0" .. "Hit Number 9" | `comboNumberScriptIds[0..9]`            |
| 12       | 0x04                | "Offset Hit Mark"             | (attack-clash symbol)                      |
| 13       | 0x07                | "Round Ani. StartTime"        | `roundStartScriptId`                       |
| 14       | 0x07                | "Round Ani. End Time"         | `roundEndScriptId`                         |
| 15–23    | 0x07                | "Round 1" .. "Round 9"        | `roundNumberScriptIds[0..8]`               |
| 24       | 0x07                | "Final Round"                 | `roundNumberScriptIds[9]` / `finalRoundScriptId` |
| 25       | 0x07                | "Spirits" (FIGHT)             | `fightScriptId`                            |
| 26       | 0x07                | "K.O."                        | `koScriptId`                               |
| 27       | 0x07                | "Perfect"                     | `perfectGameScriptId`                      |
| 28       | 0x07                | "You Win"                     | `youWinScriptId`                           |
| 29       | 0x07                | "You Lose"                    | `youLoseScriptId`                          |
| 30       | 0x07                | "1P Wins"                     | `player1WinScriptId`                       |
| 31       | 0x07                | "2P Wins"                     | `player2WinScriptId`                       |
| 32       | 0x07                | "Draw"                        | `drawGameScriptId`                         |
| 33       | 0x07                | "Double K.O."                 | `doubleKillScriptId`                       |
| 34       | 0x10                | "UNLIMITED SIGN"              | `timeNumberInfinityScriptId`               |
| 35–44    | 0x10                | "Time Number 0..9"            | `timeNumberScriptIds[0..9]`                |
| 45–54    | 0x20                | "Special stock Number 0..9"   | `skillPointNumberScriptIds[0..9]`          |
| 55       | 0x30                | "Victory mark on"             | `victorySymbolFillScriptId`                |
| 56       | 0x30                | "Victory mark off"            | `victorySymbolOutlineScriptId`             |
| 57–66    | 0x01                | "Stage Layout 1..10"          | `stageGuiScripts[0..9]`                    |
| 67       | 0x01                | "1P Life Gauge"               | `player1HpBarScriptId`                     |
| 68       | 0x01                | "2P Life Gauge"               | `player2HpBarScriptId`                     |
| 69       | 0x01                | "1P Special Gauge"            | `player1SpBarScriptId`                     |
| 70       | 0x01                | "2P Special Gauge"            | `player2SpBarScriptId`                     |
| 71       | 0x28                | "Position:Timer"              | `timerPositionScriptId` (TIMER_POS=131)    |
| 72       | 0x38                | "Pos: 1P Face"                | `player1AvatarPositionScriptId` (PLAYER_1_AVATAR_POS=195) |
| 73       | 0x48                | "Pos: 2P Face"                | `player2AvatarPositionScriptId` (PLAYER_2_AVATAR_POS=259) |
| 74       | 0x58                | "Pos: Special Stock 1P"       | `player1SkillPointPositionScriptId` (PLAYER_1_SKILL_POINT_POS=323) |
| 75       | 0x68                | "Pos: Special Stock 2P"       | `player2SkillPointPositionScriptId` (PLAYER_2_SKILL_POINT_POS=387) |
| 76       | 0x78                | "Pos: Victory Mark 1P"        | `player1VictoryFlagPositionScriptId` (PLAYER_1_VICTORY_POS=451) |
| 77       | 0x88                | "Pos: Victory Mark 2P"        | `player2VictoryFlagPositionScriptId` (PLAYER_2_VICTORY_POS=515) |
| 78       | 0x00                | "Title Cursor"                | `titleCursorScriptId`                      |
| 79       | 0x08                | "Position for the Story Mode" | `storyModePositionScriptId`                |
| 80       | 0x08                | "Position for the VS Mode"    | `pvpModePositionScriptId`                  |
| 81       | 0x00                | "Continue Cursor"             | `continueCursorScriptId`                   |
| 82       | 0x08                | "Position Cursor it does"     | `continuePositionScriptId`                 |
| 83       | 0x08                | "Position Cursor it does not" | `giveUpPositionScriptId`                   |
| 84       | 0x00                | "1P Vs screen Cursor"         | `player1CharSelCursorScriptId`             |
| 85       | 0x00                | "2P vs screen Cursor"         | `player2CharSelCursorScriptId`             |
| 86       | 0x00                | "1P Vs cursor after input"    | `player1CharSelConfirmCursorScriptId`      |
| 87       | 0x00                | "2P Vs cursor after input"    | `player2CharSelConfirmCursorScriptId`      |
| 88       | 0x08                | "Pos: Cursor for Team Battle" | `teamModePositionScriptId` — also flagged `|= 2` in script-flags when loaded, see `OpenProjectFile@0x408d01`. |
| 89       | 0x00                | "Pause"                       | `pauseScriptId`                            |
| 90–103   | 0x00                | "Spare6" .. "Spare19"         | reserved slots; **deleted** on legacy-format load (no `fileSignature[12]` flag) |

### 1.3 Reader behaviours

`OpenProjectFile@0x4089f0` postprocessing after reading the 66,108-byte block:

1. Walk all 104 `predefinedScriptIds` × 5-byte records in `off_446910`,
   overwriting `g_projectSlot.scripts[idx].scriptName` with the canonical
   editor name (strings are always in English in `.rdata`).
2. Set `g_projectSlot.scripts[predefinedScriptIds[88]].flags |= 2` — marks
   "Team Battle Cursor" as a special script.
3. For each of the 8 demo config IDs (`demoConfig.titleDemoId` through
   `unknownTag2`), clear to 0 if the referenced `demoNames[id]` is empty.
4. If the file-signature byte at `header.fileSignature[12]` is 0 (legacy
   format), iterate `predefinedScriptIds[90..103]` and call
   `DeleteScriptEntry(hWnd, &g_projectSlot, *v14)` on each — i.e. strip
   the old "Spare" slots. Then write `fileSignature[12] = 1` to upgrade.

`InitColorAndRenderBuffers@0x4081b6` (new-project init) only pre-populates
slots 0..89 (stops at `byte_446AD6`), leaving Spare6..Spare19 empty.

## 2. Editor WndProcs for `g_kgtGameSystemData`

### 2.1 `BasicRuleTabWndProc@0x4093E0` — "Basic Rules" tab

Class `byte_7D6490`. **Does NOT register control IDs on WM_CREATE**; instead
on `WM_COMMAND 0x10 (`wParam==10`) it builds the UI on first draw.

| Dialog ID | Type            | Bound field                               | Range | Tooltip key |
|----------:|-----------------|-------------------------------------------|-------|-------------|
| 2312 | CheckBox     | `projectBaseConfig` bit 1 (allowClash)    | —     | "Use Story Mode" |
| 2313 | CheckBox     | `projectBaseConfig` bit 2                 | —     | "Use VS Mode" |
| 2314 | CheckBox     | `projectBaseConfig` bit 3                 | —     | "Use VS Team Mode" |
| 2315 | CheckBox     | `projectBaseConfig` bit 4                 | —     | "The editor won't…" |
| 2316 | CheckBox     | `projectBaseConfig` bit 0                 | —     | "Numbers are shown" (HP bar) |
| 2317 | CheckBox     | `projectBaseConfig` bit 5                 | —     | (see §1.1) |
| 2318 | CheckBox     | `projectBaseConfig` bit 6                 | —     | (see §1.1) |
| 2000 | NumericEdit  | `recoverTimeConfig.attackRecoverTime` (= `unknownBlock1[5]`) | 0..255 | "100 in the max" ("Hit" label) |
| 2000 | NumericEdit  | `recoverTimeConfig.defenceRecoverTime` (= `unknownBlock1[6]`) | 0..255 | "100 in the max" ("Gaurd" label) |
| 2000 | NumericEdit  | `recoverTimeConfig.clashRecoverTime` (= `unknownBlock1[7]`) | 0..255 | "100 in the max" ("Offset" label) |
| 1210 | ComboBox     | `demoConfig.titleDemoId`                  | 0..N  | "Title" demo |
| 1211 | ComboBox     | `demoConfig.storyModeCharSelectDemoId`    | 0..N  | "Character Selec" (Story mode char-sel BGM demo) |
| 1212 | ComboBox     | `demoConfig.oneVsOneModeCharSelectDemoId` | 0..N  | "Character Selec" (1v1) |
| 1213 | ComboBox     | `demoConfig.teamModeCharSelectDemoId`     | 0..N  | "Character Selec" (team) |
| 1214 | ComboBox     | `demoConfig.continueDemoId`               | 0..N  | "Game Over" |
| 1215 | ComboBox     | `demoConfig.openingDemoId`                | 0..N  | "Opening Demo" |

The Basic Rules tab does NOT touch `recoverTimeConfig.gap` (byte [4]) — that
field appears to have no editor UI and stays at the default (0).

### 2.2 `CharSelectLayoutTabWndProc@0x409DE0` — "Char Select Layout" tab

Class `byte_7D7450`. Edits all 14 × int16 fields of `charSelectConfig`:

| Dialog label (EN) | Field                                          | Range      |
|-------------------|------------------------------------------------|------------|
| "Character Start X"       | `selectBoxStartX`                         | 0..640     |
| "Y"                        | `selectBoxStartY`                         | 0..480     |
| "Distance Between" (X)    | `iconWidth`                               | 1..640     |
| "Y" (icon height)          | `iconHeight`                              | 1..480     |
| "Columns and Rows" (X)    | `columnNum`                               | 1..50      |
| "Y" (row count)            | `rowNum`                                  | 1..50      |
| "Player 1 Cursor Pos" (X) | `player1PortraitX`                        | 0..640     |
| "Y"                        | `player1PortraitY`                        | 0..480     |
| "Player 1 Selection" (X)  | `player1PortraitTeamOffsetX` (SpinCtrl)   | -999..999  |
| "Y"                        | `player1PortraitTeamOffsetY` (SpinCtrl)   | -999..999  |
| "Player 2 Cursor Pos" (X) | `player2PortraitX`                        | 0..640     |
| "Y"                        | `player2PortraitY`                        | 0..480     |
| "Player 2 Selection" (X)  | `player2PortraitTeamOffsetX` (SpinCtrl)   | -999..999  |
| "Y"                        | `player2PortraitTeamOffsetY` (SpinCtrl)   | -999..999  |

### 2.3 `CommonImagesTabWndProc@0x409BC0`

Class `unk_7D6760`. This one is essentially a stub — only creates a group box
("DemoSel") and checks `projectBaseConfig` bit 1 state. The actual
common-images list is rendered by `CommonImageListPanelWndProc` (out of scope
for Agent 6's task — it handles the `commonImageScripts[200]` list editing).

### 2.4 `SystemSettingsPanelWndProc@0x422DA0` — mislabeled

Despite its name, this WndProc is **NOT** editing `g_kgtGameSystemData` — it
edits per-character "Adv.", "CPU1..CPU7" tab data inside
`g_characterSlots[g_currentEditorSlot]` (at offset `+1271828` into the
character's buffer, which is where character-specific game rules live).
Scope-wise this belongs to Agent 4 (character editor).

The only `g_kgtGameSystemData` touches in `SystemSettingsPanelWndProc` are:

* Iterating `stageNames[]` (@0x423080) to populate the "Stage" combo on the
  Adv. tab (which stage the CPU fights on).
* Iterating `demoNames[]` (@0x4244bd) for the Demo selector on the Demo tab.
* Iterating `playerNames[]` (@0x423a28) for the character selector on CPU tabs.

These are read-only lookups; no `g_kgtGameSystemData` fields are mutated here.

## 3. Stage Editor (`.stage` file extension)

### 3.1 File layout

A `.stage` file has the same 16-byte signature + `KgtFileHeader` as a `.kgt`,
then the common-resource part (scripts/items/pictures/palettes/sounds) via
`ReadCommonResourcePart`, then a fixed **1,033-byte** stage-specific block
read via `ReadFile(..., &g_stageSlots[idx][1], 0x409, ...)`
(see `LoadStageFile@0x4257a0`).

### 3.2 Fields in the 1,033-byte block (mostly reserved)

| Offset | Size | Field                         | Editor WndProc writing it | C++ match |
|-------:|-----:|-------------------------------|---------------------------|-----------|
| `+0x000` | 4 | `KgtStageConfig.bgmSoundId` (int32) | `BgBgmSettingsPanelWndProc@0x426BE0` dialog ID 1710 (combo, "BGM") | `_2dfm::KgtStageConfig::bgmSoundId` |
| `+0x004` | 1029 | reserved/padding               | (none — not editor-visible) | — |

The editor only surfaces the BGM selector, but the on-disk footprint is the
full 1,033 bytes (presumably reserved for future stage parameters).

### 3.3 Stage editor WndProc hierarchy

| Addr       | WndProc                              | Role |
|------------|--------------------------------------|------|
| `0x425250` | `StageEditorTabContainerWndProc`     | Top-level tab container (tabs: "StartCon", "Adv.") |
| `0x424E60` | `BgEditorTabContainerWndProc`        | Sub-tab container (tabs: "Adv.", "CPU1..CPU7") — *shared with per-character Adv editor; the stage container uses it for its CPU-specific overrides* |
| `0x4266B0` | `BgEditorMainWndProc`                | Main window for the currently-active stage tab |
| `0x426B30` | `BgEditorContentPanelWndProc`        | Hosts the BGM-settings inner panel |
| `0x426BE0` | `BgBgmSettingsPanelWndProc`          | **BGM combo-box** — writes `KgtStageConfig.bgmSoundId` |
| `0x425640` | `BgLayerPanelWndProc`                | placeholder (reserved for future layer editor) |
| `0x425690` | `BgObjectPanelWndProc`               | placeholder |
| `0x4256E0` | `BgPropertyPanelWndProc`             | placeholder |
| `0x426F20` | `BgPlaceholderPanelWndProc`          | placeholder |
| `0x426F70` | `BgSubPanel1WndProc`                 | placeholder |
| `0x426FC0` | `BgSubPanel2WndProc`                 | placeholder |
| `0x427010` | `BgSubPanel3WndProc`                 | placeholder |
| `0x427060` | `BgSubPanel4WndProc`                 | placeholder |

The Bg*Panel (Layer/Object/Property/Placeholder/SubPanel[1-4]) WndProcs are
all stubs that only implement `WM_CREATE` (register class pointer) and
`WM_CLOSE` (call `SaveTabState`) — they don't touch any fields. This confirms
the 1033-byte stage extension currently carries only the 4-byte `bgmSoundId`.

### 3.4 Save path

`SaveStageFile@0x425950` (previously mislabeled `SaveBgDataToFile`):

1. `WriteFile(f, g_stageSlots[idx], 0x10, ...)` — 16-byte file signature/header.
2. `WriteCommonResourcePart(g_stageSlots[idx], f)` — scripts / items / pictures /
   palettes / sounds.
3. `WriteFile(f, &g_stageSlots[idx][1], 0x409, ...)` — 1,033-byte stage
   extension.
4. Output file path: `"<stageName>.stage"` (or `"<stageName>.stage.t"` if
   `isTestPlay != 0`).

## 4. Demo Editor (`.demo` file extension)

### 4.1 File layout

Same as `.stage`: signature/header + `ReadCommonResourcePart` + **1,033-byte**
demo-specific block (see `LoadDemoFile@0x427120`).

### 4.2 Fields in the 1,033-byte block

Per `2dfmFile.hpp` `KgtDemoConfig` (9 bytes), confirmed byte-for-byte in
`DemoBgmTimePanelWndProc@0x428700`:

| Offset | Size | Field                     | Editor WndProc                 |
|-------:|-----:|---------------------------|--------------------------------|
| `+0x000` | 2 | `KgtDemoConfig.bgmSoundId`       | combo ID `0x6AE` / 1710 ("BGM")         |
| `+0x002` | 1 | `KgtDemoConfig.pressToSkip`      | checkbox ID 3030 ("Skip With Input")    |
| `+0x003` | 2 | `KgtDemoConfig.unknownGap`       | *(no editor UI — padding)*              |
| `+0x005` | 4 | `KgtDemoConfig.totalTime`        | numeric edit (range 0..900000) "1/1001 second if you…" |
| `+0x009` | 1024 | reserved/padding              | *(not editor-visible)*                  |

Total block = 1,033 bytes. The 9 bytes of `KgtDemoConfig` plus 1,024 bytes of
padding.

### 4.3 Demo editor WndProc hierarchy

| Addr       | WndProc                              | Role |
|------------|--------------------------------------|------|
| `0x4281A0` | `DemoEditorMainWndProc`              | Top-level demo editor window (creates tab control + content panel + slot list) |
| `0x428650` | `DemoEditorContentPanelWndProc`      | Hosts the inner `DemoBgmTimePanelWndProc` |
| `0x428700` | `DemoBgmTimePanelWndProc`            | **BGM combo + pressToSkip + totalTime** |

### 4.4 Save path

`SaveDemoDataToFile@0x4272D0` mirrors `SaveStageFile`: 16-byte header,
then `WriteCommonResourcePart`, then 1,033-byte extension. Output path
`"<demoName>.demo"` (or `".demo.t"` for test-play).

## 5. Test-Play Settings

### 5.1 `TestGameSettingsDialogProc@0x435130`

Modal dialog. Saves selections to loose global variables (not
`g_kgtGameSystemData`), intended for the next test-play launch:

| Dialog ID | Control            | Saved to   | Source                                               |
|----------:|--------------------|------------|------------------------------------------------------|
| 1006 | Player 1 character combo | `dword_5F1324` | `g_kgtGameSystemData.playerNames[]` |
| 1008 | Player 2 character combo | `dword_5F132C` | `g_kgtGameSystemData.playerNames[]` |
| 1009 | Player 1 control combo   | `dword_5F1328` | `off_44D5D0` list (CPU/Keyboard/Joy1/Joy2 etc.) |
| 1010 | Player 2 control combo   | `dword_5F1330` | same list                                            |
| 11002 | Life slider (1..1310721) | `dword_4517BC` → `uValue` | `GetDlgItemInt(12001)` display |
| 11003 | Time slider (1..script[114]+18) | `dword_4517B4` → `dword_5F1350` | `GetDlgItemInt(12002)` display |
| 12001 | Life value readout       | (display of `uValue`) | — |
| 12002 | Time value readout       | (display of `dword_5F1350`) | — |
| 13001 | Checkbox (enable)        | `uCheck` | — |
| 13002 | Checkbox                 | `dword_5F133C` | — |
| 13003 | Checkbox                 | `dword_5F1344` | — |
| 13005 | Checkbox                 | `dword_5F1354` | — |
| 14001 | Stage selector combo     | `dword_5F1340` | `g_kgtGameSystemData.stageNames[]` |
| 14002 | "Start From" combo       | `dword_5F134C` | `"From Title"` / `"From Battle"` literals |
| 10000 | OK button                | — | Commits all → `EndDialog(1)` |
| 10001/2 | Cancel                 | — | `EndDialog(-1)` |

Dialog ID 14002 toggles enable-state of 1006/1008/1009/1010 — you can only
pick player characters/controls when starting "From Battle". "From Title"
disables them (the title screen will do its own char-select).

### 5.2 `TestPlayFolderDlgProc@0x43C980`

Asks the user for a **target folder** to copy the test-play build to.
Dialog ID 12002 holds the path string (persisted in `byte_5F17F4`). Browse
button (`0x3A9C`) opens `SHBrowseForFolderA`. OK (`0x3A9A`) reads the path
back. Cancel (`0x3A9B`) returns 0.

### 5.3 `TestPlayExportDlgProc@0x43CB80`

Triggers `ExportTestPlayData(hDlg)` which writes `.stage.t` / `.demo.t` /
`.player.t` variants and the current project into the target folder. Dialog
IDs 15001 (Cancel), 15002 (Start Export → becomes "OK" after completion),
15003 (Close). Status text goes to `16003` ("In Progress" → "Done") and
path to `16004`.

## 6. `LoadProjectSettings@0x404B80` / `SaveProjectSettings@0x4052C0`

These read/write **`game.ini`** (editor preferences, not `g_kgtGameSystemData`).
For the full INI schema and key names, see
`docs/editor/game_ini_reference.md` — already generated in a prior session.

Key sections loaded/saved (abbreviated — see that doc for the exact keys):

* `[Game]` — last-opened project path, CWD, working dir.
* `[CurrentProject]` — `CurrentPlayer/CurrentStage/CurrentDemo` indices +
  `g_currentEditorSlot` + `demoIdx` + `stageIdx`.
* `[Slot States]` — per-slot dirty flags.
* `[Player 0..49] / [Stage 0..49] / [Demo 0..99]` — per-slot metadata.
* `[Flag/Left/Right/Top/Bottom/Tab N]` — editor-window layouts + last-active
  tab per mode.
* `[Palette]` — 256 palette entries (color RGBs) — `"%d"` keys.
* `[Position]` — per-sprite cursor positions `"%d x"` / `"%d y"`.
* `[Display]` — view flags like `g_previewZoomHalfScale`, etc.
* `[Default]` — 8 default-color indices.

`ReadEditorSettingsFromIni` is called at the tail to populate the remaining
per-tab persistent UI state.

## 7. Unused / Placeholder WndProcs in scope

These are registered via `RegisterEditorWindowClasses` but are **stubs** — only
`WM_CREATE` sets the class-ptr global and `WM_CLOSE` calls `SaveTabState`:

* `BgPlaceholderPanelWndProc@0x426F20`
* `BgSubPanel1..4WndProc@0x426F70 / 0x426FC0 / 0x427010 / 0x427060`
* `BgLayerPanelWndProc@0x425640`
* `BgObjectPanelWndProc@0x425690`
* `BgPropertyPanelWndProc@0x4256E0`
* `DemoPlaceholderPanelWndProc@0x427840` (registered but stubbed)

They're scaffolding for future stage/layer editors not yet wired up in the
current editor build.

## 8. Summary of Changes Made to IDA

1. **`KgtGameSystemData` struct redeclared** at ordinal 146, size 66108:
    * Split `unknownBlock1[8]` → `byte unknownByte0 + byte[3] unknownPad1_3 + KgtRecoverTimeConfig recoverTimeConfig`.
    * Renamed `throwReactions` → `commonImageScripts` (editor-visible name — the header's `ThrowReaction` is a misnomer; the editor presents these as "Common Images").
    * Split `positionData[264]` → `uint16 predefinedScriptIds[104] + byte predefinedPad[56]`.
2. The 104-entry predefined-scripts table at `off_446910` (520 bytes of
   `{const char *name; uint8 flag}` records) is now fully documented in
   §1.2 and tied to `KgtGame::*ScriptId` fields.

## 9. Cross-Reference Map

| Concept                                   | C++ source                               | IDA                                       |
|-------------------------------------------|------------------------------------------|-------------------------------------------|
| Predefined script names table             | *(implicit in `KgtGame::initBasicScriptInfos`)* | `off_446910` (104 × 5 bytes)              |
| Predefined script slot → script index     | *(mapped by name lookup)*                | `g_kgtGameSystemData.predefinedScriptIds` |
| Stage BGM                                 | `_2dfm::KgtStageConfig::bgmSoundId`      | `BgBgmSettingsPanelWndProc@0x426BE0`      |
| Demo BGM / time / pressToSkip             | `_2dfm::KgtDemoConfig`                   | `DemoBgmTimePanelWndProc@0x428700`        |
| Game-wide recovery times (hit/guard/clash)| `_2dfm::RecoverTimeConfig`               | `BasicRuleTabWndProc@0x4093E0` IDs 2000 (hit/gaurd/offset) |
| Character-select layout                   | `KgtGame::charSelectConfig`              | `CharSelectLayoutTabWndProc@0x409DE0`     |
| Project mode toggles                      | `KgtGame::projectBaseConfig`             | `BasicRuleTabWndProc@0x4093E0` IDs 2312..2318 |
| Demo selection (title/story/1v1/...)      | `_2dfm::GameDemoConfig`                  | `BasicRuleTabWndProc@0x4093E0` IDs 1210..1215 |
| Test-play dialog                          | —                                        | `TestGameSettingsDialogProc@0x435130`     |
| Test-play folder picker                   | —                                        | `TestPlayFolderDlgProc@0x43C980`          |
| Test-play exporter                        | —                                        | `TestPlayExportDlgProc@0x43CB80`          |
| Editor prefs INI                          | —                                        | `LoadProjectSettings@0x404B80` / `SaveProjectSettings@0x4052C0`, see `game_ini_reference.md` |

## 10. Open Items

* `KgtGameSystemData.unknownByte0` (offset `0x4E20`, default **2**) has an
  initialiser but no known reader — needs runtime-binary (WonderfulWorld)
  to cross-check.
* `recoverTimeConfig.gap` (offset `0x4E24`, byte [0] of the 4-byte struct) has
  no editor UI and defaults to 0 on new projects.
* `predefinedPad[56]` at offset `0xFE04` — zero xrefs. Likely end-of-block
  slack.
* The 1,029-byte tail of the stage extension and the 1,024-byte tail of the
  demo extension are unused by the editor. Runtime game (WonderfulWorld) may
  read additional fields from these regions.
