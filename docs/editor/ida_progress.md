# KGT2nd_EDITOR.exe — IDA Reverse-Engineering Progress

Running reference that maps IDA state for `KGT2nd_EDITOR.exe` (the 2DFM/Fighter Maker 2nd editor binary) to the Hong Kong developer 厉猛's C++ port in `2dfm/`. When we later switch to analyzing the compiled-game binary (e.g. `WonderfulWorld_ver_0946.exe`), the same structs and naming conventions should apply — the game is an interpreter over the same file format.

## Binary

| | |
|---|---|
| File | `KGT2nd_EDITOR.exe` |
| Architecture | 32-bit Windows PE |
| Image base | `0x400000` |
| `.text` | `0x401000..0x443000` |
| `.rdata` | `0x4432D8..0x445000` |
| `.data` | `0x445000..0x7D9000` (3.6 MB — holds huge static editor state) |
| `.uro_` | `0x802000..0x803000` (empty; `VirtualSize=1`, `SizeOfRawData=0`; no xrefs — see `test_play_and_audio.md` §1) |
| Functions | 510 (495 user-named, 15 thunks/null stubs) |

## Declared Types (IDA Local Types)

All structs follow 厉猛's C++ layout from `2dfm/2dfmCommon.hpp` / `2dfm/2dfmFile.hpp` / `2dfm/2dfmScriptItem.hpp`. All are `#pragma pack(1)` — fields are byte-packed.

### File-format structs

| Struct | Size | Source |
|---|---|---|
| `KgtFileHeader` | 272 | `2dfmFile.hpp:45` — 16-byte signature + `KgtNameInfo` |
| `KgtNameInfo` | 256 | `2dfmFile.hpp:13` |
| `KgtScript` | 39 | `2dfmCommon.hpp:16` — name + index + flags |
| `KgtScriptItem` | 16 | `2dfmCommon.hpp:24` — type byte + 15 payload bytes |
| `KgtPictureHeader` | 20 | `2dfmCommon.hpp:30` |
| `KgtColorBgra` | 4 | union `2dfmCommon.hpp:45` |
| `KgtPalette` | 1024 | 256 × `KgtColorBgra` |
| `KgtSoundItemHeader` | 42 | `2dfmCommon.hpp:64` |
| `KgtReactionItem` | 36 | `2dfmFile.hpp:18` |
| `KgtThrowReaction` | 32 | `2dfmFile.hpp:24` |
| `KgtRecoverTimeConfig` | 4 | `2dfmFile.hpp:50` |
| `KgtGameDemoConfig` | 8 | `2dfmFile.hpp:57` |
| `KgtProjectBaseConfig` | 4 | `2dfmFile.hpp:69` — bitfield: encryptGame/allowClash/story/1v1/team/etc |
| `KgtCharSelectConfig` | 28 | `2dfmFile.hpp:83` |
| `KgtDemoConfig` | 9 | `2dfmFile.hpp:100` |
| `KgtStageConfig` | 4 | `2dfmFile.hpp:107` |
| `KgtHurtBindInfo` | 12 | `KgtPlayer.hpp:11` |
| `KgtThrowActionInfo` | 16 | `KgtPlayer.hpp:20` |

### Script-item opcode structs (`2dfmScriptItem.hpp`)

16-byte discriminated-union variants of `KgtScriptItem` based on the leading `type` byte:

| Struct | `type` enum (`CommonScriptItemTypes`) | Purpose |
|---|---|---|
| `KgtShowPic` | 12 (PIC) | Show sprite frame |
| `KgtMoveCmd` | 1 (MOVE) | Movement/acceleration |
| `KgtPlaySoundCmd` | 3 (SOUND) | |
| `KgtColorSetCmd` | 35 (COLOR) | |
| `KgtJumpCmd` | 10 (JUMP) | |
| `KgtLoopCmd` | 9 (LOOP) | |
| `KgtRandomCmd` | 32 (RANDOM) | |
| `KgtObjectCmd` | 4 (OBJECT) | Spawn child/object |
| `KgtVariableCmd` | 31 (VARIABLE) | Variable assign/compare/branch |
| `KgtAfterimageCmd` | 37 (AFTERIMAGE) | |
| `KgtStageStart` | — | Stage-specific START item |
| `KgtPos` | — | Position marker |

### Slot-buffer variants

The editor loads a slot into a contiguous buffer. After load, the first field of `KgtPictureHeader` / `KgtSoundItemHeader` is repurposed as the pointer to allocated picture/sound data (see `ReadCommonResourcePart`, where `*(v6-4) = v8;` overwrites the first 4 bytes with `GlobalAlloc()` result).

| Struct | Size | Notes |
|---|---|---|
| `KgtPictureSlotEntry` | 20 | `{ void *data; int width, height, hasPrivatePalette, size; }` |
| `KgtSoundSlotEntry` | 42 | `{ void *data; char name[32]; int size; byte soundType, track; }` |
| `KgtPaletteWithPad` | 1056 | `KgtPalette palette; byte padding[32];` |

### Top-level slot struct

```c
struct KgtProjectSlot {            // total = 1,271,828 bytes (0x136814)
    KgtFileHeader       header;                  // 272
    KgtScript           scripts[1024];           // 39,936
    KgtScriptItem       scriptItems[65536];      // 1,048,576
    KgtPictureSlotEntry pictureHeaders[8192];    // 163,840
    KgtPaletteWithPad   sharedPalettes[8];       // 8,448
    KgtSoundSlotEntry   soundHeaders[256];       // 10,752
    int32               trailer;                 // 4
};
```

### Game-system block (KGT-file-specific, 66,108 bytes)

This block follows a `KgtProjectSlot` on disk/in the main project buffer. For stage/demo files, a 1,033-byte block replaces it (see `docs/editor/system_settings.md`).

**Reversed in session 2** (Agent 6). `unknownBlock1[8]` is actually one unknown byte + 3 pad + `KgtRecoverTimeConfig`. `positionData[264]` is actually `uint16 predefinedScriptIds[104]` (parallel to the 104-entry `g_predefinedScriptTable` at `0x446910`) followed by 56 bytes of padding. Full details + per-slot meanings in `docs/editor/system_settings.md` §1.2.

```c
struct KgtGameSystemData {                          // total = 66,108 bytes (0x1023C)
    KgtNameInfo           playerNames[50];          // 0x00000: 12,800
    KgtReactionItem       reactionItems[200];       // 0x03200: 7,200
    byte                  unknownByte0;             // 0x04E20: default 2 (no reader found)
    byte                  unknownPad1_3[3];         // 0x04E21: default 0
    KgtRecoverTimeConfig  recoverTimeConfig;        // 0x04E24: {gap,hit,gaurd,offset}
    KgtNameInfo           stageNames[50];           // 0x04E28: 12,800
    KgtNameInfo           demoNames[100];           // 0x08028: 25,600
    KgtGameDemoConfig     demoConfig;               // 0x0E428: 8
    KgtProjectBaseConfig  projectBaseConfig;        // 0x0E430: 4
    KgtThrowReaction      commonImageScripts[200];  // 0x0E434: 6,400 (editor's "Common Image" list)
    uint16                predefinedScriptIds[104]; // 0x0FD34: 208 — parallel to g_predefinedScriptTable
    byte                  predefinedPad[56];        // 0x0FE04: 56
    KgtCharSelectConfig   charSelectConfig;         // 0x0FE3C: 28
    byte                  playerSelectableInfos[50];// 0x0FE58: 50
    byte                  trailingPadding[946];     // 0x0FE8A: 946
};
```

## Renamed Functions

### File I/O — corrected mislabels

Previously mislabeled loaders/savers (verified against the `.demo` / `.stage` format strings they use):

| Addr | New name | Old name | Evidence |
|---|---|---|---|
| `0x428be0` | `ReadCommonResourcePart` | `ReadProjectData` | Matches `readCommonResourcePart` in `2dfmFileReader.cpp:295` — reads scripts/script-items/pictures/palettes/sounds into a `KgtProjectSlot` |
| `0x428e60` | `WriteCommonResourcePart` | `SaveCharacterDataToFile` | Symmetric writer; called by all four save paths (project/stage/demo/character) — not character-specific |
| `0x427120` | `LoadDemoFile` | `LoadStageData` | Uses `"%s.demo"` format string; error strings say "demo Error on"/"demo Read Error" |
| `0x4257a0` | `LoadStageFile` | `LoadBackgroundData` | Uses `"%s.stage"`; error strings say "Stage Open error"/"Stage Read error" |
| `0x425950` | `SaveStageFile` | `SaveBgDataToFile` | Writes `"%s.stage"` / `"%s.stage.t"` |
| `0x42a3f0` | `DeleteScriptEntry` | `LoadCharacterData` | Prompts `"Do you wish to delete %s ?"`, shifts script slots, calls `DeleteSkillCellAtPosition` — this is a *delete* function, not a loader |

### Retyped entry points (KgtProjectSlot-aware signatures)

- `ReadCommonResourcePart(KgtProjectSlot *slot, HANDLE hFile)` @ `0x428be0`
- `WriteCommonResourcePart(KgtProjectSlot *slot, HANDLE hFile)` @ `0x428e60`
- `OpenProjectFile(LPCSTR path, int silent)` @ `0x4089f0`
- `SaveProjectFile(const void *path, int promptForName, int isTestPlay)` @ `0x4083d0`
- `LoadStageFile(int stageIdx)` @ `0x4257a0`
- `LoadDemoFile(int demoIdx)` @ `0x427120`
- `SaveStageFile(int stageIdx, int isTestPlay)` @ `0x425950`
- `SaveDemoDataToFile(int demoIdx, int isTestPlay)` @ `0x4272d0`

## Renamed Globals

### Big project buffers

| Addr | Name | Type | Size | Role |
|---|---|---|---|---|
| `0x62ED80` | `g_projectSlot` | `KgtProjectSlot` | 1,271,828 | The currently-loaded `.kgt` project's common-resource buffer |
| `0x62ED90` | `g_projectFilename` | `CHAR[256]` | 256 | Current project filename (trimmed of path/extension) — written by `SaveProjectFile` |
| `0x765594` | `g_kgtGameSystemData` | `KgtGameSystemData` | 66,108 | KGT-specific block (player/stage/demo names, configs) — directly follows `g_projectSlot` in memory |
| `0x7D7B80` | `g_characterSlots` | `KgtProjectSlot*[50]` | 200 | Per-character slot pointers (`.player` files) |
| `0x7757E0` | `g_stageSlots` | `KgtProjectSlot*[50]` | 200 | Per-stage slot pointers (`.stage` files) |
| `0x607440` | `g_demoSlots` | `KgtProjectSlot*[100]` | 400 | Per-demo slot pointers (`.demo` files) |

### Name buffers (overlap with `g_kgtGameSystemData` fields — also exposed as separate labels for historical xrefs)

| Addr | Name | Equivalent field |
|---|---|---|
| `0x76A3BC` | `g_stageNames` | `g_kgtGameSystemData.stageNames[0].name` |
| `0x76D5BC` | `g_demoNames` | `g_kgtGameSystemData.demoNames[0].name` |

### Editor state flags

| Addr | Name | Meaning |
|---|---|---|
| `0x5F1358` | `g_projectStateFlags` | First 8 bytes of a 808-byte flag block zeroed on load |
| `0x5F1360` | `g_playerSlotFlags[50]` | Per-player slot modification flags — `& 1` = dirty, triggers `SaveCharacterSlotToPlayerFile` |
| `0x5F1428` | `g_stageSlotFlags[50]` | Per-stage slot dirty flags → `SaveStageFile` |
| `0x5F14F0` | `g_demoSlotFlags[100]` | Per-demo slot dirty flags → `SaveDemoDataToFile` |
| `0x5F16A4` | `g_currentEditorMode` | Top-level editor mode (scripts/pictures/sounds/etc) |
| `0x5F16A8` | `g_editorTabIndex` | Active tab within the current mode |
| `0x5F16C4` | `g_currentEditorSlot` | Currently-selected slot index (character/stage/demo) |
| `0x5F16D4` | `g_currentCharacterIdx` | Character index for `DisplayCharacterData` |
| `0x5F16EC` | `g_projectLoaded` | 1 after `OpenProjectFile` success |
| `0x5F16F0` | `g_projectDirty` | Reset on open/save; title bar reflects this |
| `0x5F1320` | `g_mainHwnd` | Main window HWND (241 xrefs) |

## Key Loader Algorithm (verified in IDA)

`OpenProjectFile@0x4089f0` loads a `.kgt`:

1. Free existing character/stage/demo slot allocations (loop over the 3 slot-pointer arrays).
2. `InitializeCharacterSlot(1)` — reset per-slot state.
3. `memset(&g_projectStateFlags, 0, 0x328)` — clear 808-byte flag block.
4. `CreateFileA` → `ReadFile(..., g_projectSlot, 0x10, ...)` — read 16-byte file signature.
5. `ValidateFileHeader(g_projectSlot)` — magic check.
6. `ReadCommonResourcePart(g_projectSlot, hFile)`:
   - Read 256-byte name into `header.name`.
   - Read 4-byte scriptCount (≤ 0x400), then `scripts[count]`.
   - Read 4-byte scriptItemCount (≤ 0x10000), then `scriptItems[count]`.
   - Back-fill unused script-entry `scriptIndex` fields with item count.
   - Read 4-byte pictureCount (≤ 0x2000), then for each: read `KgtPictureHeader` → compute size (`width*height + (hasPrivatePalette?1024:0)`, override with `size` field if non-zero) → `GlobalAlloc` → read data → store pointer in the first 4 bytes of the header.
   - Read 8 × `KgtPaletteWithPad` (fixed).
   - Read 4-byte soundCount (≤ 0x100), then for each: read `KgtSoundSlotEntry` (42 bytes) → `GlobalAlloc(size)` → read data → store pointer in the first 4 bytes.
   - Read 4-byte trailer.
7. `ReadFile(..., g_kgtGameSystemData, 0x1023C, ...)` — KGT-specific block (player/stage/demo names, reactions, configs).
8. Iterate stage-name references: if `stageNames[refIdx].name[0] == 0`, null out the reference.
9. `ValidateFileHeader` again; if not pre-validated, call `LoadCharacterData` (actually `DeleteScriptEntry`) for each referenced script slot — TODO: verify this logic, may have additional meaning.
10. Set `g_projectLoaded = 1`; `DisplayCharacterData`; `RefreshMainWindow`.

## Stats

| | Before session | After session |
|---|---|---|
| Functions named | 495 / 510 (97.1%) | 495 / 510 (97.1%) — remaining are thunks/null stubs |
| `.data` user-named globals | 94 | 111 (+17 high-leverage names; ~65 auto-named absorbed into structs) |
| `.data` auto-named globals | 2815 | 2750 |
| Top-level structs declared | 0 | 30 |
| Key static buffers typed | 0 | 3 (g_projectSlot, g_kgtGameSystemData, g_*_Slots arrays) |
| Function prototypes applied | 0 | 8 (loader/saver signatures) |

## Next Steps

1. Apply `KgtProjectSlot` / `KgtGameSystemData` types to parameters across the call graph so the decompiler fully resolves field accesses in every editor function.
2. ~~Reverse `positionData[264]`~~ — **done in session 2**: `predefinedScriptIds[104]` + 56 bytes pad; see `docs/editor/system_settings.md` §1.2.
3. ~~Reverse `unknownBlock1[8]`~~ — **done in session 2**: `byte unknownByte0 + byte[3] pad + KgtRecoverTimeConfig recoverTimeConfig`.
4. ~~Model the 1,033-byte stage/demo-specific blocks~~ — **done in session 2**: stage carries only `KgtStageConfig` (bgmSoundId); demo carries `KgtDemoConfig` (9 bytes). Both have ~1 KB of unused tail bytes reserved for future fields. See `system_settings.md` §§3–4.
5. ~~Handle the `.uro_` overlay section at `0x802000`~~ — **done**: it is an empty, unused PE section (`VirtualSize=1`, `SizeOfRawData=0`, zero xrefs, zero relocations). See `docs/editor/test_play_and_audio.md` §1.
6. Move to `WonderfulWorld_ver_0946.exe` with the same type library — the runtime game is a script interpreter over the same `KgtScriptItem` opcodes.
7. `g_kgtGameSystemData.unknownByte0` still has no known reader (only initialiser sets it to 2). Re-check in WonderfulWorld.
8. The 28 bytes of `KgtCharSelectConfig` (int16 × 14) and the `playerSelectableInfos[50]` byte array aren't yet indexed in the struct's field-xref map — that's cheap follow-up to confirm all 14 charSelect fields are bound in `CharSelectLayoutTabWndProc`.

## Cross-Reference Map

| Concept | C++ source | IDA |
|---|---|---|
| Common-resource reader | `2dfm/2dfmFileReader.cpp:295 readCommonResourcePart` | `ReadCommonResourcePart@0x428be0` |
| KGT file reader | `2dfm/2dfmFileReader.cpp:63 readKgtFile` | `OpenProjectFile@0x4089f0` + `ReadCommonResourcePart` |
| Demo file reader | `2dfm/2dfmFileReader.cpp:187 readDemoFile` | `LoadDemoFile@0x427120` |
| Stage file reader | `2dfm/2dfmFileReader.cpp:219 readStageFile` | `LoadStageFile@0x4257a0` |
| Player file reader | `2dfm/2dfmFileReader.cpp:251 readPlayerFile` | TODO — likely `LoadCharacterDataIntoSlot@0x40CC20` or `ImportCharacterFromFile@0x40CCC0` |
| Picture-size calc | `2dfm/2dfmFileReader.cpp:387 get2dfmPictureSize` | Inlined in `ReadCommonResourcePart` (at 0x428D3C..0x428D50) |

## See Also

- `docs/editor/game_ini_reference.md` — complete INI config reference
- `docs/editor/system_settings.md` — game-wide settings, stage/demo editors, test-play (Agent 6, session 2)
- `docs/editor/test_play_and_audio.md` — test-play bundler/launcher, sound CRUD, audio pipeline (winmm+MCI), bg/demo import, `.uro_` investigation
- `docs/editor/2DFM Codeblocks.md` — codeblock opcode reference (Hong Kong dev's notes)
- `docs/editor/opcode_dispatcher.md` — `ExecuteAnimationScript@0x439CD0` opcode switch reference
- `docs/editor/opcode_editor_ui.md` — per-opcode property-editor WndProc reference
- `docs/editor/runtime_entity.md` — `KgtRuntimeObject` + pools + 12 helpers (Agent 9 replacement, this session)
- `2dfm/2dfm_binary_analysis.md` — original binary-vs-code analysis (against `WonderfulWorld_ver_0946.exe`)
- `2dfm/2dfm_player_repo_analysis.md` — structure of the HK dev's C++ port
- `2dfm/unity_vs_cpp_comparison.md` — comparison with `Unity2dfmRuntime/`
