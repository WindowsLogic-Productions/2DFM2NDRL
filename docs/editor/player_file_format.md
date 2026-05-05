# `.player` File Format — Fighter Maker 2nd

Reverse-engineered layout of FM2K character files (`.player`). Validated against `KGT2nd_EDITOR.exe` (loader at `DisplayCharacterData@0x40D270`, writer at `WriteCharacterDataToFile@0x40D6B0`). This supersedes the TODOs in `2dfm/2dfmFileReader.cpp:251` `readPlayerFile`.

## On-Disk Layout

Character files are stored as `<name>.player`. The writer first emits the 16-byte file signature via a direct `WriteFile(..., slot, 0x10, ...)`, then calls `WriteCommonResourcePart(slot, hFile)`, then streams seven player-specific blocks. The loader mirrors this exactly.

| Order | Size | Description | In-memory offset |
|---|---|---|---|
| 0 | 16 | `KgtFileHeader.fileSignature` (magic "2DKGT2K\0...\1") | 0 (slot start) |
| 1 | 256 | `KgtFileHeader.name` (KgtNameInfo) | 16 |
| 2 | var | Common-resource part (scripts, script-items, pictures, palettes, sounds) — see `ReadCommonResourcePart@0x428BE0` | 272 onward |
| 3 | 4 | Trailer int after sound table | end of common-resource |
| 4 | 4 + 82 × N (count ≤ 100) | **Command inputs** — `KgtCommandInput[N]` | `slot[1] + 11110` |
| 5 | 4 + 4 × N (count ≤ 200) | **Reaction → skill map** — 2-byte script ID + 2-byte pad per reaction | `slot[1] + 19310` |
| 6 | 4 + 6 × N (count ≤ 200) | **Hurt-bind / throw-bind (compact)** — 6-byte entries (semantics partly unknown) | `slot[1] + 20110` |
| 7 | 11110 (0x2B66) | **CPU/AI block** — 10-byte header + 100 × 111-byte CPU entries | `slot[1] + 0` |
| 8 | 1867 (0x74B) | Unknown block (settings/misc) | `slot[1] + 21310` |
| 9 | 20604 (0x507C) | **Story events** — 4-byte header + 100 × 206-byte entries | `slot[1] + 23177` |
| 10 | 4524 (0x11AC) | Unknown tail block (likely palette/graphics overrides and basic script bindings) | `slot[1] + 43781` |

`slot[1]` refers to `g_characterSlots[slotIdx] + 0x136814` — immediately after the `KgtProjectSlot` main buffer. The character-slot allocation is `0x1424C5` (1,319,621) bytes total; the player-specific tail occupies the last 47,793 bytes.

## In-Memory Layout (the player-specific tail)

```c
// Total 48,305 bytes live in the character-slot's 47,793-byte player tail.
// The blocks overlap: some fields (e.g. basicScriptIds at tail+20796) live
// inside the hurtBind region in RAM, but on disk they are serialized
// separately because the writer emits fixed blocks in the order above.

struct KgtPlayerFileBlocks {  // starts at g_characterSlots[i] + 0x136814
    byte cpuBlock[11110];         // 0x0000..0x2B66 — CPU/AI entries (see below)
    KgtCommandInput commands[100];// 0x2B66..0x4B2E — 82 bytes/entry
    byte reactionSkillMap[800];   // 0x4B2E..0x4E4E — 200 × 4 (uint16 scriptId + 2 pad)
    byte hurtBindEntries[1200];   // 0x4E4E..0x52FE — 200 × 6-byte entries
    // basicScriptIds occupy bytes 686..735 of hurtBindEntries IN RAM only
    byte unknown74B[1867];        // 0x52FE..0x5A49 — 0x74B block
    byte storyBlock[20604];       // 0x5A49..0xAAC5 — 4-byte header + 100 × 206 story entries
    byte unknown11AC[4524];       // 0xAAC5..0xBC71 — final tail block
};
```

## Block 4: Commands (special-move inputs)

100 slots × 82 bytes. Empty slots have `name[0] == 0`. The editor windows that touch this block:

- `CommandListWndProc@0x40EC40` — scrollable list of command names
- `CommandEditPanelWndProc@0x40F450` — edits a single entry
- `CommandPriorityPanelWndProc@0x40EA40` — shows priority counter ("Priority lest crowed then %d")

```c
#pragma pack(push, 1)
struct KgtCommandInput {           // 82 bytes
    char name[32];                 // +0    command name (null-terminated)
    uint16 commandTime;            // +32   time window 0..1023 frames
    uint16 scriptIdA;              // +34   script fired by button A
    uint16 scriptIdB;              // +36   script fired by button B
    uint16 scriptIdC;              // +38   script fired by button C
    uint16 scriptIdD;              // +40   script fired by button D
    uint16 inputMasks[10];         // +42   10 input-direction slots
                                   //       bit 0x1000 = "this step is set"
                                   //       bit 0x2000 = "trailing marker"
                                   //       bit 0xC000 = modifier flags
                                   //       bit 0x10, 0x20 = cascade flags applied
                                   //           by the editor when a slot is enabled
    uint16 thresholds[10];         // +62   numeric thresholds 1..999 per slot
};
#pragma pack(pop)
```

Special-move inputs are encoded as a sequence of up to 10 direction/button bitmasks. The editor cascades flags when a slot is toggled (enabling slot `i` sets bit `0x10` on slots `> i` and bit `0x20` on slots `i+1..i+9`).

## Block 5: Reaction → skill map (200 × 4 bytes)

The writer labels this as "throw bindings" but the semantics match **reaction → script ID mapping**. For each of the 200 global `KgtReactionItem`s in `g_kgtGameSystemData.reactionItems`, this block stores the per-character script ID that handles that reaction. `InitializeCharacterSlotData@0x40C680` populates it by calling `AddNewSkillEntry(slot, reactionItems[i].reactionName, 0, 0)` and storing the returned script index.

```c
struct KgtReactionSkillMapEntry {  // 4 bytes
    uint16 scriptId;               // script-entry index returned by AddNewSkillEntry
    uint16 padding;
};
```

Writer skips trailing zero entries to minimize file size.

## Block 6: Hurt-bind / 6-byte entries (partly unknown)

200 × 6 bytes max. The writer starts iteration at `scripts[508].scriptName[28]` and checks bytes `[-1], [0], [+1]` (3 bytes) for non-zero presence, then advances by 6. This is a compact form of the `KgtHurtBindInfo` struct in `KgtPlayer.hpp` (which declared 12 bytes = 3 ints but the binary uses only 6 bytes). Likely layout:

```c
struct KgtHurtBindCompact {        // 6 bytes
    byte  byte0;                   // maybe reactionId low byte
    byte  byte1;                   // reactionId (non-zero = entry present)
    byte  byte2;                   // scriptId byte
    byte  byte3;                   // effectId / extra
    uint16 extraField;             // e.g. effect object id or timing
};
```

**NOTE**: the 25-entry `basicScriptIds` table (written by `InitializeCharacterSlotData` at `slot+0x13BB50`, tail offset 20796) physically lives INSIDE this block's RAM footprint (tail offsets 20796..20846). On disk these 50 bytes are part of the 6-byte hurt-bind block stream — which means `basicScriptIds` and `hurtBindEntries` share storage. Resolving this overlap is a **TODO** — possibly the basic-script table is serialized as part of the hurt-bind array with a reserved mid-block position.

`KgtPlayer.hpp` enumerates 24 basic-script names (`standScriptId`, `forwardScriptId`, …, `shadowScriptId`). The editor writes `[esi + i*2 + 0x13BB50]` for `i = 0..24`, so the full table is 25 × uint16 = 50 bytes.

## Block 7: CPU/AI block (11110 bytes total)

- 10-byte header (purpose unknown — first byte of entry 0 is at tail+10)
- 100 × 111-byte CPU entries

Referenced by `CpuListPanelWndProc@0x412800`, `CpuPropertiesPanelWndProc@0x4132F0`, `AddNewCpuEntry@0x412230`, `SwapCpuEntries@0x4124F0`. Priority counter: "%d cpu left" in `CpuMainPanelWndProc@0x4125F0`.

```c
#pragma pack(push, 1)
struct KgtCpuEntry {              // 111 bytes (exact field ordering needs more work)
    char name[32];                // +0    entry name (e.g. "Cpu 1")
    byte flags;                   // +32   bit0 = "character is in AI", bit1 = "enemy in AI"
    byte probability;             // +33   0..100 trigger probability
    uint16 intervalMin;           // +34   0..9998 frames (pattern between)
    uint16 intervalMax;           // +36   1..9999 frames
    byte reserved1[3];            // +38..40
    // 10 × 7-byte conditions starting at +41 or +42
    struct {
        uint16 flagsMask;         // default 0x2000 for first condition
        byte   reserved[3];
        uint16 thresholdValue;    // default 1
    } conditions[9];              // ends at +104 or so
    byte tail[6];                 // trailing bytes; SwapCpuEntries moves byte
                                  // at +108 to +54, byte at +110 is separate
} ;
#pragma pack(pop)
```

**Exact packing of the last condition and the trailing bytes is not fully resolved** — `AddNewCpuEntry` uses `memset(entry, 0, 0x6C)` (108 bytes) plus explicit writes at `+108` and `+110`, and `SwapCpuEntries` re-shuffles `[108]` to `[+54]` during swaps. Total: 111 bytes.

## Block 8: Unknown 0x74B (1867 bytes)

No direct WndProc references found in the player-editor scope. Starts at `slot[1].scripts[539].scriptName[17]` = tail offset 21310. Candidates for contents:
- Additional basic-action script IDs (beyond the 25 in block 6)
- Guard-related tables (the `guardButton` / guard-ratio settings that `CharacterPropertiesPanelWndProc` binds to `scripts[586].scriptName[19..29]`)
- Sound/voice bindings
- **TODO**: scan for WndProc references that read tail offsets 21310..23177.

## Block 9: Story events (20604 bytes)

- 4-byte header at tail offset 23177
- 100 × 206-byte `KgtStoryEntry` starting at tail offset 23181

Editor functions:
- `StoryModeSettingsPanelWndProc@0x421F90` — enables/disables story mode
- `StoryEventListPanelWndProc@0x4223B0` — list & per-row preview
- `InsertStoryEvent@0x421D70`, `DeleteStoryEvent@0x421E10`, `SwapStoryEvents@0x421EC0` — list ops
- `UpdateStoryEventEditorTab@0x421BC0` — switches sub-panel based on event kind

```c
#pragma pack(push, 1)
struct KgtStoryEntry {            // 206 bytes (0xCE)
    byte  kindAndFlags;           // +0  low 4 bits = event kind:
                                  //     1 = reaction bind (paramIndex indexes reactionItems)
                                  //     2 = stage trigger (paramIndex indexes stageNames)
                                  //     3 = numeric value (numericValue is the count)
                                  //     4 = other marker
    byte  paramIndex;             // +1  index into corresponding global table
    byte  reserved1[2];           // +2
    byte  numericValue;           // +4  used when kind == 3; default 100 for kind 1
    byte  reserved2[201];         // +5..205 — per-event body (text/params/sub-events)
};
#pragma pack(pop)
```

Per-slot enablement is gated by `g_kgtGameSystemData.playerSelectableInfos[slotIdx] & 1` ("Story Mode" checkbox in `StoryModeSettingsPanelWndProc`). Panel icons are drawn from a sprite strip indexed by `kindAndFlags & 0xF`.

## Block 10: Unknown 0x11AC tail (4524 bytes)

Starts at `slot[1].scriptItems[223].bytes[4]` = tail offset 43781. No direct player-editor WndProc references in scope. Likely candidates:
- Per-character palette overrides / color tables
- Precomputed collision data
- Additional story-mode assets (portraits, dialogue offsets)
- **TODO**: cross-reference with sprite/palette WndProcs (Agent 5's scope).

## Character-Properties Panel Field Layout

`CharacterPropertiesPanelWndProc@0x413A80` binds UI controls to fields inside the player-tail (via `scripts[N].scriptName[K]` addressing in the decompiler). These addresses illustrate WHERE meta-data lives:

| Field | Tail offset | Purpose |
|---|---|---|
| `scripts[541].scriptName[25]` | 21123 | Age (number) |
| `scripts[541].scriptName[29]` | 21127 | Sex combobox (Male / Female / Both / None) |
| `scripts[586].scriptName[15]` | 22918 | Y position of side HP bar (-3000..3000) |
| `scripts[586].scriptName[17]` | 22920 | Pattern interval (0..1280) |
| `scripts[586].scriptName[19]` | 22922 | Block-shave ratio (0..100) |
| `scripts[586].scriptName[20]` | 22923 | Start-pos (0..100) |
| `scripts[586].scriptName[21]` | 22924 | Correction ratio (0..100) |
| `scripts[586].scriptName[22]` | 22925 | Char revision (0..100) |
| `scripts[586].scriptName[23]` | 22926 | Guard-button combobox (A/B/C/D/E/asc_44920C) |
| `scripts[586].scriptName[24]` | 22927 | Life-gauge max (1..1000) |
| `scripts[586].scriptName[28]` | 22931 | Special-gauge max (1..1000) |
| `scripts[586].scriptIndex` | 22933 | Special stock max (0..9) |
| `scripts[586].flags` (byte 1) | 22934 | Flags: bit0 = 2312-check, bit1 = 2313-check, bit3 = 2314-check |
| `scripts[587].scriptName[5]` | 22958 | Gauge gain when attack hits (-1000..1000) |
| `scripts[587].scriptName[7]` | 22960 | Gauge gain when enemy hits (-1000..1000) |
| `scripts[587].scriptName[9]` | 22962 | Starting special stock (0..9) |

These all fall within the **block 7 + block 8** range (tail offsets 21122..22962, mostly in the 0x74B block), confirming block 8 largely holds character meta/config. The `playerSelectableInfos[i]` byte (1 per character) lives in `g_kgtGameSystemData`, NOT in the .player file itself — it's part of the KGT project.

## Loader Algorithm (`DisplayCharacterData@0x40D270`)

1. `AllocateCharacterSlotBuffer(slotIdx)` — ensure slot memory is allocated.
2. If `playerNames[slotIdx].name[0] == 0`, abort.
3. `ZeroMemoryBlock(slot, 0x1424C5)` — clear full character buffer (not just the slot).
4. `CreateFileA("%s.player" % playerName, GENERIC_READ, ...)`.
5. `ReadFile(..., slot, 0x10)` — 16-byte file signature.
6. `ValidateFileHeader(slot)` — magic check (same as project/stage/demo).
7. `ReadCommonResourcePart(slot, hFile)` — reads scripts/items/pictures/palettes/sounds.
8. Read 4-byte command count (≤ 100), then `82 * count` bytes → `slot[1] + 11110`.
9. Read 4-byte reaction count (≤ 200), then `4 * count` bytes → `slot[1] + 19310`.
10. Read 4-byte hurt-bind count (≤ 200), then `6 * count` bytes → `slot[1] + 20110`.
11. Read 0x2B66 (11110) bytes verbatim → `slot[1] + 0` (CPU block).
12. Read 0x74B (1867) bytes verbatim → `slot[1] + 21310`.
13. Read 0x507C (20604) bytes verbatim → `slot[1] + 23177`.
14. Read 0x11AC (4524) bytes verbatim → `slot[1] + 43781`.
15. `slot.scripts[0].scriptName = "------None------"`.
16. If `header.fileSignature[12] == 0` (legacy-format flag), iterate `off_448088..off_4480E0` (basic-script default-names table) and call `AddNewSkillEntry` for missing entries.
17. Set `header.fileSignature[12] = 1` (mark "initialized").

Load errors → "Reading Error", "P. Data Error", or "Player Error" status messages, then `DeleteCharacterSlot`.

## Writer Algorithm (`WriteCharacterDataToFile@0x40D6B0`, wrapped by `SaveCharacterSlotToPlayerFile@0x40D880`)

Save path:
1. `CreateFileA("%s.player" or "%s.player.t", GENERIC_WRITE, CREATE_ALWAYS)`.
2. `WriteFile(hFile, slot, 0x10)` — signature.
3. `WriteCommonResourcePart(slot, hFile)`.
4. Count non-empty commands (first field `name[0] != 0`) → write count + `82 × count` bytes.
5. Count non-empty reaction-skill entries → write count + `4 × count` bytes.
6. Count non-empty hurt-bind entries (any of bytes [-1], [0], [+1] non-zero) → write count + `6 × count` bytes.
7. Write fixed blocks: 0x2B66, 0x74B, 0x507C, 0x11AC (no count prefixes).
8. `CloseHandle`.

Any `WriteFile` failure → "file write error" status.

## Import Flow (`ImportCharacterFromFile@0x40CCC0`)

1. Enumerate empty slots in `g_kgtGameSystemData.playerNames[0..49]` to find a free position.
2. Show Open File dialog with `"%s.player"` filter.
3. Strip directory prefix + extension, validate chars, ensure name uniqueness across all character slots.
4. Shift existing slot buffers rightward from the end to make room at `slotIdx`.
5. `ZeroMemoryBlock(g_characterSlots[slotIdx], 0x1424C5)`.
6. Call `LoadCharacterDataIntoSlot(slotIdx, name)` → `DisplayCharacterData(slotIdx, 0)`.
7. Clear the `0x507C` story block (`memset(&slot[1].scripts[587].scriptName[12], 0, 0x507C)`) to wipe story events from the imported file (since story IDs would reference the SOURCE project's globals).
8. Mark slot dirty.

## IDA Symbols (renamed this session)

### Functions
| Addr | Name |
|---|---|
| `0x40CCC0` | `ImportCharacterFromFile(HWND, int)` |
| `0x40CC20` | `LoadCharacterDataIntoSlot(int, const KgtNameInfo*)` |
| `0x40D270` | `DisplayCharacterData(int, int)` — the actual player-file loader |
| `0x40D6B0` | `WriteCharacterDataToFile(HANDLE, KgtProjectSlot*)` |
| `0x40D880` | `SaveCharacterSlotToPlayerFile(int, int)` |
| `0x40EA40` | `CommandPriorityPanelWndProc` |
| `0x40EC40` | `CommandListWndProc` |
| `0x40F450` | `CommandEditPanelWndProc` |
| `0x412230` | `AddNewCpuEntry` |
| `0x412430` | `DeleteCpuEntry` |
| `0x4124F0` | `SwapCpuEntries` |
| `0x4125C0` | `RefreshCpuPanels` |
| `0x4125F0` | `CpuMainPanelWndProc` |
| `0x412800` | `CpuListPanelWndProc` |
| `0x4132F0` | `CpuPropertiesPanelWndProc` |
| `0x4139B0` | `CharacterPropertiesContainerWndProc` |
| `0x413A80` | `CharacterPropertiesPanelWndProc` |
| `0x421BC0` | `UpdateStoryEventEditorTab` |
| `0x421D70` | `InsertStoryEvent` |
| `0x421E10` | `DeleteStoryEvent` |
| `0x421EC0` | `SwapStoryEvents` |
| `0x421F90` | `StoryModeSettingsPanelWndProc` |
| `0x4223B0` | `StoryEventListPanelWndProc` |
| `0x42F870` | `PlayerTreePanelWndProc` |
| `0x42F780` | `PopulatePlayerTreeView` |

### Global state
| Addr | Name | Meaning |
|---|---|---|
| `0x5F16CC` | `g_currentCommandIdx` | Currently-selected command row in `CommandListWndProc` |
| `0x5F16DC` | `g_currentCpuIdx` | Currently-selected CPU row in `CpuListPanelWndProc` |
| `0x5F16D8` | `g_currentStoryIdx` | Currently-selected story row in `StoryEventListPanelWndProc` |

### Declared types (IDA Local Types)
| Struct | Size | Purpose |
|---|---|---|
| `KgtCommandInput` | 82 | One command-input entry (special-move definition) |
| `KgtCpuCondition` | 7 | One AI condition within a CPU entry |
| `KgtCpuEntry` | ~111 | One AI "Cpu N" routine (exact last-condition packing TBD) |
| `KgtStoryEntry` | 206 | One story-mode event |
| `KgtPlayerFileBlocks` | 48,305 | The player-specific tail that follows a `KgtProjectSlot` in memory |

## Remaining TODOs

1. **Exact `KgtCpuEntry` packing** (111 vs 112 bytes) — `AddNewCpuEntry` writes `[108]`, `[110]` and `memset(0, 0x6C=108)`, while `SwapCpuEntries` shuffles `[108]→[54]`. The last condition either occupies 5 bytes (not 7) or there's a trailing 1-byte marker after 10 full conditions.
2. **basicScriptIds overlap** — the 25 × uint16 table at tail offset 20796 physically resides inside the hurt-bind block. On disk it is serialized within the variable-length hurt-bind region; confirm how readPlayerFile should parse the boundary.
3. **Block 8 (0x74B)** — 1867-byte block between story and command data. Holds character meta (life-gauge max, guard settings, stock, pattern intervals per `CharacterPropertiesPanelWndProc`) but the full schema is not decoded.
4. **Block 10 (0x11AC)** — 4524-byte tail block is entirely unanalysed. Suspect palette tables or extended story-mode data.
5. **`KgtHurtBindCompact` byte layout** — writer only verifies 3 of 6 bytes for non-zeroness. Need to find the reader that interprets these (likely in the runtime game `.exe`, not the editor).
6. **`KgtStoryEntry.reserved2[201]`** — 201 unanalysed trailing bytes per story event. Hold text strings? Sub-events? Animation parameters?

## Cross-References

| Concept | C++ port | IDA symbol |
|---|---|---|
| `readPlayerFile` (incomplete) | `2dfm/2dfmFileReader.cpp:251` | `DisplayCharacterData@0x40D270` |
| `HurtBindInfo` (12-byte, doesn't match) | `2dfm/KgtPlayer.hpp:11` | binary uses 6-byte `KgtHurtBindCompact` |
| `ThrowActionInfo` (16-byte, doesn't match) | `2dfm/KgtPlayer.hpp:20` | binary uses 4-byte reaction-skill-map |
| `KgtPlayer.commandListEntries` (void*) | `2dfm/KgtPlayer.hpp:39` | `KgtCommandInput[100]` (this doc) |
| `KgtPlayer.aiEntries` (void*) | `2dfm/KgtPlayer.hpp:41` | `KgtCpuEntry[100]` (this doc) |
| `KgtPlayer.storyEntries` (void*) | `2dfm/KgtPlayer.hpp:43` | `KgtStoryEntry[100]` (this doc) |
| `KgtPlayer.standScriptId`..`shadowScriptId` | `2dfm/KgtPlayer.hpp:46..92` | 25 × uint16 at tail offset 20796 (inside hurt-bind block) |

## See Also

- `docs/editor/ida_progress.md` — master IDA state (structs, globals, renamed functions)
- `2dfm/2dfm_binary_analysis.md` — original block-size analysis (this doc refines the semantics)
