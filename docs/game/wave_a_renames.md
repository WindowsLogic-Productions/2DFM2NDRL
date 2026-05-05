# Wave A Global Rename Pass — `WonderfulWorld_ver_0946.exe` (.data `0x41E000..0x450000`)

Cleanup pass over the auto-named globals in the game-runtime binary's `.data` region `0x41E000..0x450000` (scope assigned to Wave A). Pre-existing named globals (377 of them, named by a prior analyst) were **not touched**. Only globals with names matching the IDA dummy-prefix pattern (`dword_`/`byte_`/`word_`/`qword_`/`unk_`/`off_`/`stru_`/`flt_`/`dbl_`/`asc_`) were renamed.

## Tally

|                                                | Before | After |
|------------------------------------------------|-------:|------:|
| Auto-named globals in range                    |    264 |   176 |
| Auto-named with xrefs ≥ 10                     |     10 |     0 |
| Auto-named with xrefs 5–9                      |      8 |     0 |
| Auto-named with xrefs 2–4                      |    101 |    44 |
| Auto-named with xrefs 0–1                      |    145 |   132 |
| Total semantic renames applied                 |      — |    88 |
| Non-auto-named (user-named) globals in range   |    499 |   567 |

Every high-value auto-named global (xrefs ≥ 5) is resolved. The 176 remaining auto-named globals are all ≤ 3 xrefs and fall into two buckets:

1. **CRT internals** — low-xref `dword_419XXX`/`dword_418XXX`-adjacent symbols used only by `__XcptFilter`/`_NMSG_WRITE`/`__amsg_exit`/`__crtLCMapStringA`/etc. Not worth semantic names; they're already decompiled legibly.
2. **Sub-dialog state dwords** — per-WndProc temporary HWNDs & state (keyboard config / joystick config / settings dialog) with 2-3 xrefs each. Can be named opportunistically when their WndProc gets re-studied.

## Key discoveries

### 1. The `0x4213B0` cluster is a rotation/zoom sprite blitter scratch-state block

The highest-xref cluster — `dword_4213B0` (25 xrefs), `dword_4213B8` (10), `dword_4213BC` (5), `dword_4213C0` (10) — turned out to be the **rotation+zoom sprite blitter's per-frame scratch state**, sitting in an unnamed function at `0x4011FB`. Two contiguous 2048-byte precomputed tables at `unk_4203B0` (`g_rotzoom_rowIncTable`, 512 × 4-byte row-advance increments) and `unk_420BB0` (`g_rotzoom_srcOffsetTable`, 512 × 4-byte source-row offsets) drive the inner loop.

| Address | New name | Role |
|---|---|---|
| `0x4203B0` | `g_rotzoom_rowIncTable` | 2 KB: per-row src-pointer increments (fixed-point 24.8) |
| `0x420BB0` | `g_rotzoom_srcOffsetTable` | 2 KB: per-row dst-pointer increments |
| `0x4213B0` | `g_rotzoom_rowCounter` | inner loop-counter (height remaining) |
| `0x4213B8` | `g_rotzoom_height` | saved dst height (ebp+14h) |
| `0x4213BC` | `g_rotzoom_widthSaved` | saved dst width (ebp+10h) |
| `0x4213C0` | `g_rotzoom_startXFixed` | starting x-offset (fixed-point) |

### 2. The `0x4213C8 / 0x4213D0 / 0x421510 / 0x421A88 / 0x422E88` DirectPlay cluster

`dword_4213C8` (9 xrefs) is the **local `DPID`** (DirectPlay player id) set by `DPlay::CreatePlayer`. The surrounding globals form the DirectPlay enumeration state:

| Address | New name | Layout |
|---|---|---|
| `0x4213C8` | `g_dplay_localPlayerId` | `DPID` (4 bytes) |
| `0x4213D0` | `g_dplay_sessionGuidTable` | session GUIDs (16 bytes × 20 slots) |
| `0x421510` | `g_dplay_providerGuidTable` | service-provider GUIDs (16 bytes × 20 slots) |
| `0x421A88` | `g_dplay_providerNameTable` | service-provider name strings (256 bytes × 20 slots) |
| `0x422E88` | `g_dplay_sessionNameTable` | session name strings (256 bytes × 20 slots, ends at `0x424288`) |
| `0x41C398` | `g_guid_dplayAppGuid` | game's DirectPlay application GUID (16 bytes) |
| `0x41C3E8` | `g_guid_dplayTcpIpProvider` | TCP/IP service-provider GUID used to filter `EnumConnections` |
| `0x424288..424294` | `g_dpname_dwSize/dwFlags/lpszShortName/lpszLongName` | `DPNAME` struct fed to `CreatePlayer` |
| `0x4280D4` | `g_netplay_isHost` | 0 = joined, 1 = created |
| `0x41E3F8` | `g_netplay_localCharSlotIdx` | local player's slot in `KgtPlayerRuntimeSlot[8]` |

### 3. The `0x433350` cluster is the system-character (SysCHR) script/script-item table base

`dword_433350` (24 xrefs) + `dword_433354` (14 xrefs) form the **base pointers to the system-character's `KgtScript[]` (39-byte entries) and `KgtScriptItem[]` (16-byte entries) tables**, loaded from the game's `2d.2nd` SysCHR. Indexing pattern is `scripts[i] = base + 39*i`, matches `runtime_entity.md`'s `scriptsPtr` convention. They are referenced by `SpawnAnimationEffects`, `ui_state_manager`, `title_screen_manager`, `score_display_system`, `DisplayScoreNumbers`, `ProcessHitboxCollisions`, `CreateProjectileObject` etc.

A symmetrical pair `dword_425B70` / `dword_425B74` (renamed `g_demoChr_scriptTable` / `g_demoChr_scriptItemTable`) serves the demo-file character, with companion fields at `0x427C88` (`g_demoChr_objectCount`), `0x427C94` (`g_demoChr_flagsAndVolume` — demo header's rendering flags + vol byte).

### 4. INI key/section string-pointer tables

The sparse cluster at `0x41F290..0x41F374` (mirroring the editor's `0x4460AC..0x4461B0` table) is a **`LPCSTR[]` of INI file keys and section names**. 26 of them were renamed to match the pointed-to string:

- `g_iniKey_PlayerName`, `g_iniKey_SessionName`
- `g_iniFile_editorIni` ("2D Fighter Maker 2nd.ini"), `g_iniFile_gameIni` ("game.ini")
- `g_iniSec_File`, `g_iniKey_Filename`
- `g_iniKey_TP_Player0Nb/Player0Cpu/Player1Nb/Player1Cpu/GameSpeed/HitJudge/GameInformation/StageNb/JoyStick/time/exit/VSMode/VSSinglePlay/VSTeamPlay`
- `g_iniKey_GameWindowSize_x/y`, `g_iniKey_GameWindowPoint_x/y`, `g_iniKey_GameScreenMode`
- `g_iniKey_DemoNb`
- `g_iniSec_KeyInput` (0x41F838), `g_iniSec_JoyInput` (0x41F83C)
- `g_keyLabel_up` (start of 11-entry Shift-JIS key label array at `0x41F2E8..0x41F310`)
- `g_joyLabel_A` (start of 7-entry joystick label array at `0x41F338..0x41F350`)
- `g_joyConfig_defaultButtonsP1` — default joystick button indices (8 bytes player 1, 8 bytes player 2 at `0x41F354..0x41F360`)

The format and runtime meaning of these keys matches `docs/editor/game_ini_reference.md`.

### 5. Title-screen and demo state

In `title_screen_manager` / `ProcessDemoPlayback`:

| Address | New name | Role |
|---|---|---|
| `0x424F04` | `g_titleScreen_demoEnableFlag` | bit 0 = "demo mode active" (copied from `dword_427C94`'s HIBYTE) |
| `0x424F08` | `g_titleScreen_demoTimer` | frames until demo auto-start (copied from `dword_427C99`) |
| `0x424E40` | `g_titleMenu_modeList` | 3-entry int array, populated from `g_render_enabled_flag & 0x1C` |
| `0x424E60` | `g_titleMenu_maxIndex` | length-1 of `g_titleMenu_modeList` |
| `0x424F2C` | `g_stage_script_index_slot1` | companion to `g_stage_script_index[0]` |

### 6. "Saved round" block for continue/retry

`ProcessMenuSelectionInput` and `vs_round_function` preserve mid-round state to three adjacent dwords so the continue dialog can restore it:

- `0x424F34` → `g_savedRound_gameTimer` (backup of `g_game_timer`)
- `0x424F38` → `g_savedRound_matchPhase` (backup of `g_match_phase`)
- `0x424F3C` → `g_savedRound_subState` (backup of `g_round_sub_state`)

### 7. Sprite renderer and hit-debug

- `dword_41EDA0` (2 xrefs) → `g_debugColor_hitbox` — color arg passed to `RenderHitboxDebug`/`RenderHitboxOutline` for the object's **hitbox** slots (loop over `+0xD9..+0x128`).
- `dword_41EDA4` (2 xrefs) → `g_debugColor_hurtbox` — same, but for the **hurtbox** slots (`+0x89..+0xD8`). Gated on `g_hit_judge_config` (the `HIT_JUDGE_SET` INI toggle).
- `dword_424F60` (3 xrefs) → `g_stageEffect_imageTable` — indexed as `[5*v3]`, a table of 5-dword stage/background image descriptors (`{dataPtr, w, h, ?, compSize}`). Cleared by `InitializeMainWindow` (`0xA00` = 2560 bytes = 128 entries × 20 B).

### 8. Rendering scaffolding

- `dword_421A78` / `dword_421A7C` / `dword_421A80` → `g_statusBar_hdc/_hBitmap/_hOldBitmap` — the secondary 640×16 DIB section used for the status/debug overlay (set up in `InitializeMainWindow` right after the main `hdc` + `ppvBits` pair).
- `stru_421650` → `g_statusBitmapInfo` — the `BITMAPINFO` for that DIB.
- `stru_4259E0` → `g_cursorClipRect` — 1×1 rect at (100,100) passed to `ClipCursor` to hide the cursor when DirectDraw mode is active.

### 9. Settings/input-config dialog HWNDs

Three `DialogFunc`s scatter their control HWNDs across `0x4247B4..0x4247FC`. Decompiling `settings_dialog_proc` at `0x4161ce` exposes them:

- `0x4247FC` → `g_settingsDlg_repeatDelaySlider` (dlg item 2014)
- `0x424798` → `g_settingsDlg_inputDelaySlider` (dlg item 2016)
- `0x4247B8` → `g_settingsDlg_soundVolumeSlider` (dlg item 2018)
- `0x4247B4` → `g_settingsDlg_frameSkipSlider` (dlg item 2020)
- `0x4247C4` → `g_kbConfig_editKeyBuffer` (32-byte editing copy of `g_key_up[]`)
- `0x4249BC` → `g_joyConfig_playerIdx` (0 or 1)
- `0x4247F4` → `g_joyConfig_editAxisState`

### 10. CRT / RLE encoder

- `dword_424E30` (15 xrefs) → `g_rleEncoder_cursor` — output pointer advanced by the 3 RLE encoders (`RLEEncodeCommand` / `RLEEncodeValue` / `RLEEncodeCopyBlock` / `RLEEncodeRunLength`), paired with the existing `g_rle_encode_buffer` size counter.
- `byte_41E408` → `g_cdAudio_driveLetter` — first char of `GetCurrentDirectoryA` output, used as the CD drive selector.
- `dword_41E404` → `g_cdAudio_trackNumber`.
- `asc_41F798` → `g_midi_tempFilename` (`"\2dfightermaker2ndRound.mid"` — MCI Sequencer target).
- `byte_43012C` → `g_iniFile_nameOverride` (256-byte CSTRING from `GetPrivateProfileStringA(g_iniSec_File, g_iniKey_Filename, ...)`).
- `byte_433250` → `g_sysFile_nameOverride` (same pattern for the `2d.2nd` SysCHR override).
- `dword_424810` → `_errno_val` (MSVC CRT `errno`)
- `dword_424814` → `_doserrno_val`
- `dword_42480C` → `crt_first_msg_exit_fn`
- `dword_424970` → `crt_mbcp_current`
- `dword_4249A0` → `crt_lcmapstring_cache`
- `dword_41FBB4` → `crt_xcptfilter_table`
- `dword_41FB24` → `crt_nmsg_banner_state`

## Surprises

- **`stru_421650` isn't a self-contained `BITMAPINFO`**: `InitializeMainWindow`'s `memory_clear(&pbmi, 0x438u)` clears **1080 bytes** in one shot, spanning from `pbmi` → through `stru_421650` → through `dword_421A78..A84`. So these five adjacent "globals" are a single logical struct (primary DIB + secondary DIB + saved DC/HBITMAP/HGDIOBJ/pixel-bits). They're retained as distinct names for readability but readers should remember they're zeroed together.
- **`0x424F60` is a 5-dword-stride table, not a 4-dword one**: most other image-metadata tables in FM2K use 4-dword stride. Callers use `dword_424F60[5*v3]` then `[5*v3+1]` etc — 20 bytes/entry, 128 entries = 2560 bytes (matches the `memory_clear` size).
- **`dword_41F354`/`dword_41F358` are compressed 4-byte packs**: the default joystick button indices for Player 1 (`{0,1,2,3;4,5,9,0}`) fit in 8 bytes as two dwords. Player 2 defaults (`{1,2,3,4;5,9,0,0}`) follow at `0x41F35C..0x41F360`. Not an `int[]` — literally a packed-byte DWORD.

## Out-of-scope but adjacent to namings

- `dword_4451C4..4451E8`, `dword_445218..445720`, `dword_445850`, `dword_447968/447974` — these are low-xref (2 each) but sit inside the large KgtProjectSlot / per-stage block at `0x445xxx..0x447Fxxx`. Their exact struct-field meaning is best resolved by declaring the `KgtProjectSlot` struct (outside Wave A scope) rather than labeling each dword individually.
- `byte_4247F0` (3 xrefs) is an adjacent joystick-config staging dword to `word_4247F4`; skipped for now as a 2-struct pair — better to declare a single `JoyConfigStaging` struct covering `0x4247EC..0x4247F8`.

## Next steps for follow-up waves

1. Declare a `DPNAME` struct type over `0x424288..0x424298` (the 4 contiguous DirectPlay-name dwords). IDA already recognizes their `dwSize=16` at compile time.
2. Declare a `MSVCRT_XcptInfo` struct over `0x41FBA0..0x41FBC0` to collapse the remaining `__XcptFilter`-related dwords into a single typed block.
3. Retype `g_rotzoom_rowIncTable` / `g_rotzoom_srcOffsetTable` as `uint32_t[512]` (currently still `unk_*`).
4. Remaining 176 low-xref auto-named globals should be handled opportunistically while studying their respective WndProcs (keyboard-config / joystick-config / settings-dialog).
5. The `KgtProjectSlot` struct port from the editor (per `PORTING_README.md`) would resolve most of the remaining `dword_445*`/`dword_447*` auto-names in one pass.
