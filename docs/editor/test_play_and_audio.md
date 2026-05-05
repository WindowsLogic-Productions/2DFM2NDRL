# KGT2nd_EDITOR — Test-play, Audio & Import Pipeline

Reference notes for the test-play bundler, the .uro section investigation, the sound-list CRUD operations, the audio playback pipeline (sndPlaySoundA + MCI sequencer + MCI cdaudio), and the BG / Demo import helpers. Builds on `docs/editor/ida_progress.md` and `docs/editor/system_settings.md` — structs and globals from those docs are not redefined here.

---

## §1  The mysterious `.uro` section

**Definitive answer: `.uro` is an empty, unused PE section. It contains no code, no data, no runtime modification. It is purely vestigial.**

### Disk layout (from the on-disk PE section table)

| Field | Value |
|---|---|
| Section name | `.uro` (8-byte, zero-padded) |
| `VirtualSize` | **`0x1`** (1 byte) |
| `VirtualAddress` | `0x402000` (RVA) |
| `SizeOfRawData` | **`0x0`** (not present on disk) |
| `PointerToRawData` | `0x78000` (same as EOF — no actual data) |
| `Characteristics` | `0xE0000020`  =  `CNT_CODE | MEM_EXECUTE | MEM_READ | MEM_WRITE`  (RWX code segment) |

IDA displays the section as `.uro_` because its name collides with an IDA-reserved character class; the underscore is an IDA rename, not a real suffix. IDA maps the single reserved byte up to a page boundary (4096 bytes, `0x802000..0x803000`) and fills it with `0xFF` — Windows' loader would actually zero-fill it at runtime because `SizeOfRawData == 0`.

### Runtime evidence that it is unused

Verified via IDA MCP (see `mcp__ida__py_eval` session log):

1. **Zero aligned pointer references anywhere in `.text`, `.rdata`, or `.data`.** Scanning every 4-byte value in each section for a target in `[0x802000, 0x803000)` returned 0 hits (an earlier byte-granular scan found 23 "hits" that all turned out to be stack-frame encodings like `mov reg, [esp+0x80]` where the bytes `80 24 ??` collide with a 32-bit immediate).
2. **Zero xrefs to any address in `.uro_`** (`idautils.XrefsTo` across all 4096 bytes returns empty).
3. **No relocation directory at all** (PE Data Directory entry 5 `BaseReloc` RVA=0, size=0). The binary is statically based at `0x400000` with no fixups.
4. **No TLS / LoadConfig** (entries 9 and 10 are also RVA=0). No TLS callback, no late-init patching.
5. **Only CRT consumers of `VirtualAlloc` / `LoadLibraryA` / `GetProcAddress`.** The six xrefs are all from the C runtime (`___sbh_alloc_new_region`, `___sbh_alloc_new_group`, `___crtMessageBoxA`, `__ms_p5_mp_test_fdiv`) — no application code touches dynamic memory/loader APIs.
6. **Entry point is a stock VC++ `_start` stub** at `0x43CFF7` (`push ebp; mov ebp, esp; push -1; push <SEH>; ...`). No unpacker prolog.

### Why is it there?

Best guess: a `#pragma section("uro", execute, read, write)` or an intrinsic `__declspec(allocate(".uro"))` declaration somewhere in the source that was never populated with any symbol. The 1-byte `VirtualSize` is the MSVC linker's way of recording "a section named `.uro` exists but has nothing in it." The high RVA (`0x402000` — 4 MB above image base, way past `.rsrc` at `0x3D9000`) suggests it was manually placed, possibly so it wouldn't conflict with other overlay loads.

**Conclusion: ignore `.uro` going forward. It is not an overlay, not a packer artefact, not a plugin hook, and not referenced by any code path.**

---

## §2  Test-play export + launch pipeline

Two entry points:

| Addr | Name | Role |
|---|---|---|
| `0x435770` | `LaunchTestGame(HWND hWnd)` | Top-level user command (File → Test Play). Orchestrates dialog → export → `WinExec`. |
| `0x43C450` | `ExportTestPlayData(HWND hDlg)` | Dumps all loaded slots into `g_testPlayOutputDir`. Called from the export dialog box, not directly from `LaunchTestGame`. |

### 2.1  `LaunchTestGame@0x435770`

```c
INT_PTR __cdecl LaunchTestGame(HWND hWnd)
{
    if (ValidateGameData(hWnd))              return -1;
    LoadTestPlaySettings();
    INT_PTR r = DialogBoxParamA(hInstance, "DIALOG_TESTGAMESETUP", NULL, TestGameSettingsDialogProc, 0);
    if (r == -1)                             return -1;
    if (ValidatePlayerSettings(hWnd))        return -1;

    CleanupPreviewState(NULL);
    SaveProjectSettings();
    SaveTestPlaySettings();
    SaveProjectFile(currentPath, /*promptForName=*/0, /*isTestPlay=*/1);

    sprintf(lpString, "%s\\", Directory);   // editor install dir
    SetCurrentDirectoryA(lpString);

    // Command-line built by AppendFilterString into lpString
    switch (g_testPlayLaunchMode) {
        case 0: AppendFilterString(lpString, "KGT2nd_GAME.exe /T"); break; // normal test play
        case 1: AppendFilterString(lpString, "KGT2nd_GAME.exe /F"); break; // fight-replay
        case 2: /* no flag */                                       break;
    }
    return WinExec(lpString, SW_SHOW /* 5 */);
}
```

**Key details**:
- The export step is done by `SaveProjectFile(..., isTestPlay=1)`, which in turn invokes `ExportTestPlayData` via the export dialog. `LaunchTestGame` itself is the orchestrator, not the exporter.
- Launch uses **`WinExec`**, not `ShellExecute` or `CreateProcess`. Synchronous for the launch itself (returns instance handle >31 on success).
- The working directory is set to the editor install path (where `KGT2nd_GAME.exe` lives) — not to `g_testPlayOutputDir`. The game binary then reads the test output via relative paths.
- Three launch modes controlled by `g_testPlayLaunchMode`:
  - `0` → `KGT2nd_GAME.exe /T` — standard test play (game reads the exported `.kgt`/`.player`/etc.)
  - `1` → `KGT2nd_GAME.exe /F` — fight replay mode
  - `2` → `KGT2nd_GAME.exe` — bare invocation, no flag

### 2.2  `ExportTestPlayData@0x43C450`

Already richly annotated (282 inline comments from a previous session). High-level flow, confirmed from the decompilation:

```
1. TestWritePermission(hDlg)    — bail if output dir is read-only.
2. LoadAllUnloadedResources()  — page in any lazy-loaded sprite/sound data.
3. Copy KGT2nd_GAME.exe into g_testPlayOutputDir as <project_name>.exe
     (SHFileOperation with wFunc=FO_COPY=2, fFlags=FOF_NOCONFIRMATION|FOF_SILENT|FOF_FILESONLY=20).
4. Flip cursor to IDC_WAIT, post "During System Installation" into the dialog label.

5. EnsureSlotLoaded(1)          — make sure project slot is resident.
6. Write <g_projectSlot.name>.kgt:
     - stamp 16-byte signature "2DKGT2G" (encrypted) OR "2DKGT2K" (plain)
       based on g_kgtGameSystemData.projectBaseConfig.rawValue & 1.
     - WriteCommonResourcePart(&g_projectSlot, hFile).
     - WriteFile(hFile, &g_kgtGameSystemData, 0x1023C).

7. For i in [0..49]: if g_kgtGameSystemData.playerNames[i].name[0] != 0:
     - DisplayCharacterData(i, 1) (force-load the slot if dirty flag bit 0 clear).
     - Write <g_testPlayOutputDir>\<playerNames[i]>.player:
         16-byte signature + WriteCharacterDataToFile.

8. For i in [0..49]: if stageNames[i].name[0] != 0:
     - LoadStageFile(i) if flag bit 0 clear.
     - Write <...>.stage:
         16-byte signature + WriteCommonResourcePart + WriteFile(..., slot[1], 0x409).
         (the 0x409 / 1033-byte block is the stage-specific tail; see system_settings.md §3.)

9. For i in [0..99]: if demoNames[i].name[0] != 0:
     - LoadDemoFile(i) if flag bit 0 clear.
     - Write <...>.demo: same shape as stage but 1033-byte tail is demo-specific (§4).

10. SetCursor(hCursor) to restore.
```

**The 16-byte signature magic**:
- `2DKGT2G` — "encrypted" format (signals the runtime to apply XOR/whatever encryption when reading).
- `2DKGT2K` — plain format.

The exporter temporarily stamps the first 16 bytes of the *in-memory* `g_projectSlot` / character / stage / demo buffer with whichever magic is required, writes it, then stamps it back to `2DKGT2K` for the next write (note the back-to-back `sprintf(ptr, "2DKGT2G"); WriteFile(...); sprintf(ptr, "2DKGT2K");` pattern). This is a classic "write header with alternate magic, restore original" idiom and means the in-memory signature is always `2DKGT2K` between write cycles.

**Retyped signature (applied)**: `int __cdecl ExportTestPlayData(HWND hDlg)`.

**Flag bit semantics** (confirmed):
- `g_playerSlotFlags[i] & 1` — slot is loaded / up-to-date in memory. Cleared when the slot is newly imported but not yet flushed. If clear, `DisplayCharacterData(i, 1)` force-reloads it.
- `g_stageSlotFlags[i] & 1` / `g_demoSlotFlags[i] & 1` — same meaning for stages / demos.

---

## §3  Sound list CRUD

All four ops operate on `g_projectSlot.soundHeaders[256]` (member of `KgtProjectSlot` — array of `KgtSoundSlotEntry[256]`, each 42 bytes). See `ida_progress.md` for the struct layout: first 4 bytes hold a `HGLOBAL` to the loaded sound data (overwriting `fileOffset`), followed by `char name[32]`, `int size`, `byte soundType`, `byte track`.

| Addr | Name | Purpose |
|---|---|---|
| `0x4336E0` | `AddSoundEntry(hWnd, slot, soundIdx, soundType, overwriteExisting)` | Insert new entry, load data from .wav/.mid or set up CD-track |
| `0x433BB0` | `DeleteSoundEntry(hWnd, slot, soundIdx)` | Remove entry, fix up all PLAY_SOUND script refs |
| `0x4335D0` | `RenameSoundEntry(hWnd, slot, soundIdx)` | TextInputDialog + name-uniqueness check |
| `0x433DE0` | `SwapSoundEntries(hWnd, slot, idxA, idxB)` | Swap two slots + swap all script refs |

### 3.1  Common patterns

- **Script-ref fixup**: after any insert/delete/swap, the ops walk all 65536 `g_projectSlot.scriptItems` (`KgtScriptItem[65536]`) and inspect `type == 3` (PLAY_SOUND opcode — see `2DFM Codeblocks.md`). For those items:
  - Insert → `AdjustWordIndexAfterInsert(soundIdx, &item.bytes[1])` bumps refs ≥ soundIdx.
  - Delete → explicit in-place fixup: ref == soundIdx → 0 (silence), ref > soundIdx → --ref.
  - Swap → `SwapWordIndices(&item.bytes[1], idxA, idxB)` swaps the two refs wherever they appear.
- If `g_currentEditorMode == 2` (stage) or `3` (demo), the same fixup is applied to the active stage / demo slot's script block at `g_stageSlots[stageIdx][1].header.fileSignature` / `g_demoSlots[demoIdx][1].header.fileSignature` (those offsets are where the stage/demo-specific script data starts relative to the slot buffer).
- Overflow: all inserts refuse if `soundHeaders[255].name[0] != 0` ("list full").
- Name uniqueness: inserts reject duplicates with a `MessageBox`; `RenameSoundEntry` allows the "rename to same index" no-op but rejects any other duplicate.
- Memory: sound data is `GlobalAlloc`'d on insert and stored as `HGLOBAL` in the first 4 bytes of the slot entry (overlapping the on-disk `fileOffset` field — see `ReadCommonResourcePart` for the same trick on load). Every delete does `GlobalFree(slot.soundHeaders[soundIdx].data)` if size > 0.

### 3.2  Sound type encoding (`KgtSoundSlotEntry.soundType`)

The byte is a bit-packed field:

| Bits | Meaning |
|---|---|
| `[3:0]` (low nibble) | type: 0=unused/stop, 1=WAV, 2=MIDI, 3=CD-track |
| `[4]`   (bit 4) | loop flag (set on MIDI → `g_midiLoopFlag`; set on CD-track → `g_cdPlaybackLoopFlag`; set on WAV → `SND_LOOP=0x8` vs `SND_ASYNC=0x1` passed to `sndPlaySoundA`) |
| `[7:5]` | unused / reserved |

`AddSoundEntry` passes the raw `soundType` byte; `DispatchSoundPlayback` masks `& 0xF` for the type and checks `& 0x10` for loop.

### 3.3  File dialogs

- WAV entries: `ShowOpenFileDialog(hWnd, "Wave file\0*.wav\0\0", path)`.
- MIDI entries: `ShowOpenFileDialog(hWnd, "MIDI file\0*.mid\0\0", path)`.
- CD entries: no dialog; name auto-generated as `sprintf("CDtrack %d", 1)` (default track 1, user edits later via `RenameSoundEntry`).

---

## §4  Sound playback pipeline

### 4.1  Dispatcher — `DispatchSoundPlayback@0x404860`

Central entry point invoked from script-item preview, sound-list preview, and live playback. Dispatches on `entry->soundType & 0xF`:

```c
int __cdecl DispatchSoundPlayback(KgtSoundSlotEntry *entry) {
    switch (entry->soundType & 0xF) {
        case 0: return CleanupPreviewState(NULL);  // stop everything
        case 1: // WAV via sndPlaySoundA
            StopAndFreeWavPlayback();                                // stop previous
            if (entry->size) {
                void *src = GlobalLock(entry->data);
                pszWavSoundBuffer = GlobalAlloc(0, entry->size);
                memcpy(pszWavSoundBuffer, src, entry->size);
                GlobalUnlock(entry->data);
                DWORD flags = (entry->soundType & 0x10 | 0xA) >> 1;  // SND_ASYNC(1)|SND_MEMORY(4) +/- SND_LOOP(8)
                return sndPlaySoundA(pszWavSoundBuffer, flags);
            }
            break;
        case 2: return PlayMidiFromSoundEntry(entry);                // MCI sequencer
        case 3: return InitializeCDAudio(entry->track,
                                         (entry->soundType & 0x10) != 0);  // MCI cdaudio
    }
}
```

The flag math `(entry->soundType & 0x10 | 0xA) >> 1`:
- Base: `0xA >> 1 = 5 = SND_ASYNC(1) | SND_MEMORY(4)`.
- Loop bit: `0x10 >> 1 = 8 = SND_LOOP`, OR'd in when bit 4 of soundType is set.

### 4.2  WAV path

Handled directly in `DispatchSoundPlayback` via `sndPlaySoundA` (winmm). Data is copied from the slot's `HGLOBAL` into a fresh `GlobalAlloc`'d buffer (`pszWavSoundBuffer@0x44E6F4`) so the MM system can own it asynchronously. The previous buffer is torn down by `StopAndFreeWavPlayback@0x404810`:

```c
HGLOBAL StopAndFreeWavPlayback() {
    if (pszWavSoundBuffer) {
        sndPlaySoundA(NULL, SND_PURGE /* 2 */);
        GlobalFree(pszWavSoundBuffer);
        pszWavSoundBuffer = NULL;
    }
}
```

### 4.3  MIDI path — MCI sequencer

Three-function state machine. Tempfile approach (MCI `open sequencer` wants a filename, so the in-memory data is first written out):

| Addr | Name | Role |
|---|---|---|
| `0x434AB0` | `PlayMidiFromSoundEntry(entry)` | Write MIDI bytes → `%WINDIR%\enterbrain2dkgt2.mid`, then `StartMidiPlayback()` |
| `0x4349A0` | `StartMidiPlayback()` | `mciSendCommand(MCI_OPEN "sequencer" + file)` then `MCI_PLAY` (with MCI_NOTIFY) |
| `0x434920` | `StopSoundPlayback()` | `mciSendCommand(MCI_STOP + MCI_CLOSE)` on the sequencer device |
| `0x434960` | `DeleteTempMidiFile()` | `DeleteFileA("%WINDIR%\enterbrain2dfightermak.mid")` — legacy Fighter-Maker-era filename; the current temp uses `enterbrain2dkgt2.mid` |

```c
// 0x434AB0
int PlayMidiFromSoundEntry(KgtSoundSlotEntry *entry) {
    g_midiLoopFlag = (entry->soundType >> 4) & 1;
    DeleteTempMidiFile();                                // delete old fightermak.mid if present
    StopSoundPlayback();
    CHAR path[264];
    GetWindowsDirectoryA(path, 261);
    sprintf(path, "%s\\enterbrain2dkgt2.mid", path);
    const void *src = GlobalLock(entry->data);
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD w; WriteFile(h, src, entry->size, &w, NULL);
    CloseHandle(h);
    GlobalUnlock(entry->data);
    return StartMidiPlayback();
}

// 0x4349A0
int StartMidiPlayback() {
    if (g_midiPlaybackActive) {
        // already open — seek to start: MCI_SEEK 0x807 with MCI_TO_START=0x100
        mciSendCommandA(g_midiMciDeviceId, 0x807, 0x100, 0);
    } else {
        CHAR path[264]; GetWindowsDirectoryA(path, 261);
        sprintf(path, "%s\\enterbrain2dkgt2.mid", path);
        MCI_OPEN_PARMS p = {0};
        p.dwCallback        = hWnd;            // reserved/HWND
        p.lpstrDeviceType   = "sequencer";
        p.lpstrElementName  = path;
        // flags = MCI_OPEN_TYPE(0x2000) | MCI_OPEN_ELEMENT(0x200) = 0x2200
        if (mciSendCommandA(0, MCI_OPEN /* 0x803 */, 0x2200, &p))
            return 1;
        g_midiMciDeviceId = p.wDeviceID;
    }
    MCI_PLAY_PARMS pp; pp.dwCallback = hWnd;
    // MCI_NOTIFY = 1
    if (mciSendCommandA(g_midiMciDeviceId, MCI_PLAY /* 0x806 */, MCI_NOTIFY, &pp)) {
        mciSendCommandA(g_midiMciDeviceId, MCI_STOP /* 0x808 */, 0, 0);
        mciSendCommandA(g_midiMciDeviceId, MCI_CLOSE /* 0x804 */, 0, 0);
        return 1;
    }
    g_midiPlaybackActive = 1;
    return 0;
}

// 0x434920
MCIERROR StopSoundPlayback() {
    if (g_midiPlaybackActive) {
        mciSendCommandA(g_midiMciDeviceId, MCI_STOP  /* 0x808 */, 0, 0);
        mciSendCommandA(g_midiMciDeviceId, MCI_CLOSE /* 0x804 */, 0, 0);
        g_midiPlaybackActive = 0;
    }
}
```

Note the name "StopSoundPlayback" is slightly misleading — it only stops MIDI. WAV has its own teardown (`StopAndFreeWavPlayback`), and CD has its own (`StopCDAudio@0x434700`).

### 4.4  CD-audio path — MCI cdaudio

| Addr | Name | Role |
|---|---|---|
| `0x434890` | `InitializeCDAudio(track, loopFlag)` | Set `g_cdRequestedTrack`, probe drive letters, call `PlayCDAudioTrack` |
| `0x434760` | `PlayCDAudioTrack()` | `mciSendCommand(MCI_OPEN "cdaudio")` + `MCI_SET TMSF` + `MCI_PLAY` |
| `0x434700` | `StopCDAudio()` (was `PlayNextCDAudioTrack`) | `MCI_STOP + MCI_CLOSE` on `g_cdMciDeviceId` |
| `0x4346D0` | `IsDriveCDROM(driveIdx)` | `GetDriveTypeA("A:\\") == DRIVE_CDROM (5)` |

Drive-letter probing: `InitializeCDAudio` seeds `g_cdDriveLetter = 'A' + 0` and calls `PlayCDAudioTrack`. On failure, iterates `v2 = 0..25`; for each letter that `IsDriveCDROM` accepts, sets `g_cdDriveLetter = v2 + 'A'` and retries. Gives up with a `MessageBox("CDの再生に失敗しました" / "CD play failed")` if all 26 letters fail.

Play sequence:
```c
// 0x434760 — simplified
int PlayCDAudioTrack() {
    if (g_cdPlaybackActive) { StopCDAudio(); g_cdPlaybackActive = 0; }
    if (g_cdRequestedTrack == -1) return 0;

    MCI_OPEN_PARMS op = { .lpstrDeviceType = "cdaudio", .lpstrElementName = &g_cdDriveLetter };
    if (mciSendCommandA(0, MCI_OPEN, MCI_OPEN_TYPE|MCI_OPEN_ELEMENT /* 0x2200 */, &op)) {
        g_cdPlaybackError = 1; return 1;
    }
    g_cdMciDeviceId = op.wDeviceID;

    // MCI_SET with MCI_SET_TIME_FORMAT(0x400), dwTimeFormat=MCI_FORMAT_TMSF(10)
    MCI_SET_PARMS sp = { .dwTimeFormat = 10 };
    if (mciSendCommandA(g_cdMciDeviceId, MCI_SET /* 0x80D */, 0x400, &sp)) {
        mciSendCommandA(g_cdMciDeviceId, MCI_CLOSE, 0, 0);
        g_cdPlaybackError = 1; return 1;
    }

    // MCI_PLAY with MCI_FROM(4)|MCI_TO(8)|MCI_NOTIFY(1) = 0xD
    MCI_PLAY_PARMS pp = { .dwCallback = hWnd, .dwFrom = track, .dwTo = track + 1 };
    if (mciSendCommandA(g_cdMciDeviceId, MCI_PLAY, 0xD, &pp)) {
        mciSendCommandA(g_cdMciDeviceId, MCI_CLOSE, 0, 0);
        g_cdPlaybackError = 2; return 1;
    }
    g_cdPlaybackActive = 1;
    return 0;
}
```

### 4.5  Unified teardown — `CleanupPreviewState@0x404940`

Called from many places (file dialogs, preview panels, mode switches) to silence whatever is playing:

```c
HGLOBAL CleanupPreviewState(int kind) {
    switch (kind) {
        case 0: StopAndFreeWavPlayback(); StopSoundPlayback(); /*fallthrough*/
        case 3: return StopCDAudio();          // called PlayNextCDAudioTrack pre-rename
        case 1: return StopAndFreeWavPlayback();
        case 2: return StopSoundPlayback();
    }
}
```

### 4.6  Audio globals (renamed / indexed)

| Addr | Name | Type | Role |
|---|---|---|---|
| `0x44E6F4` | `pszWavSoundBuffer` | `HGLOBAL` | Currently-playing WAV (owned copy passed to `sndPlaySoundA`) |
| `0x44E690` | `g_midiPlaybackActive` | `int` | 1 when MCI sequencer is open |
| `0x44E694` | `g_midiMciDeviceId` | `MCIDEVICEID` | Open sequencer device |
| `0x44E698` | `g_midiLoopFlag` | `int` | Loop bit from last MIDI's soundType |
| `0x44E67C` | `g_cdPlaybackActive` | `int` | 1 when MCI cdaudio is open + playing |
| `0x44E680` | `g_cdPlaybackLoopFlag` | `int` | Loop bit for CD playback |
| `0x44E684` | `g_cdPlaybackError` | `int` | 1 = open/set failed, 2 = play failed |
| `0x44E68C` | `g_cdAudioInitialized` | `int` | Set once `InitializeCDAudio` has run |
| `0x44E678` | `g_cdMciDeviceId` | `MCIDEVICEID` | Open cdaudio device (was `mciId`) |
| `0x4451F8` | `g_cdRequestedTrack` | `int` | Track number to play |
| `0x4451FC` | `g_cdDriveLetter` | `char` | ASCII drive letter, used as `element` path to `MCI_OPEN cdaudio` |

**Temp files** (both live in `%WINDIR%`):
- `enterbrain2dkgt2.mid` — current MIDI tempfile (used by `PlayMidiFromSoundEntry` → `StartMidiPlayback`).
- `enterbrain2dfightermak.mid` — legacy Fighter-Maker-era tempfile, `DeleteTempMidiFile` removes it on every MIDI play for cleanup. The editor never *creates* this path; the delete is vestigial compat with an older release.

---

## §5  BG / Demo import helpers

Small symmetric pair. Each mode has:
- A **"from named file"** helper: takes an already-parsed `KgtNameInfo` and loads the corresponding `.stage`/`.demo` file into the right slot.
- A **"from file dialog"** helper: runs `GetOpenFileName`, strips path/extension, prompts for a new internal name (via `TextInputDialog`), makes room by shifting later slots down one, then calls the named-file helper.

| Addr | Name | Notes |
|---|---|---|
| `0x425E20` | `ImportBgFromNamedFile(stageIdx, const KgtNameInfo *name)` | Loader wrapper around `LoadStageFile` |
| `0x425EB0` | `ImportBgFromFileDialog(hWnd, insertAtIdx)` | Dialog-driven entry point |
| `0x427860` | `ImportDemoFromNamedFile(demoIdx, const KgtNameInfo *name)` | Loader wrapper around `LoadDemoFile` |
| `0x4278F0` | `ImportDemoFromFileDialog(hWnd, insertAtIdx)` | Dialog-driven entry point |

### 5.1  "From named file" shape

Identical flow for stage and demo, only the target array differs:

```c
int ImportBgFromNamedFile(int stageIdx, const KgtNameInfo *name) {
    char backup[256];
    memcpy(backup, lpString, 256);                                  // preserve current lpString
    memcpy(&g_kgtGameSystemData.stageNames[stageIdx], name, sizeof(KgtNameInfo));
    if (LoadStageFile(stageIdx))                                    // reads <...>.stage into g_stageSlots[stageIdx]
        return -1;
    // Loader overwrites name fields with what's in the file; restore the caller's desired name.
    memcpy(&g_stageSlots[stageIdx]->header.name, backup, 256);
    memcpy(&g_kgtGameSystemData.stageNames[stageIdx], backup, sizeof(KgtNameInfo));
    return 0;
}
```

The "backup lpString / copy name / load file / copy name back" sequence is there because `LoadStageFile` reads the stage's on-disk name into `g_kgtGameSystemData.stageNames[stageIdx]`, but the user-chosen name (which may differ from what's embedded in the `.stage` file they're importing) must take precedence.

### 5.2  "From file dialog" shape

```
1. Reject if slot 49 (stage) / 99 (demo) is occupied — list full.
2. ShowOpenFileDialog — "Stage file\0*.stage\0\0" / "Demo file\0*.demo\0\0".
3. Parse chosen path: strip all directory components (scan until '\\', reset), strip .stage/.demo extension.
4. Copy base name into lpString+0x80 (the TextInputDialog display area).
5. Uniqueness loop:
     - Scan stageNames/demoNames for a duplicate — if found, ShowStatusMessage + v8++.
     - ValidateFilenameChars — invalid chars → v8++.
     - If v8 > 0, prompt TextInputDialog, then re-check. User OK empty → error; name > 31 chars → error.
6. Shift later slots down one to make room at insertAtIdx:
     - memcpy(g_stageSlots[i+1], g_stageSlots[i], sizeof(KgtProjectSlot)=0x136C1D).
     - g_stageSlotFlags[i+1] = g_stageSlotFlags[i].
     - Copy stageNames[i] to stageNames[i+1].
     - EnsureSlotLoadedByIndex(kind, i) and (kind, i+1) for each pair being moved (pages dirty slots in before copy).
7. Zero-init the new slot: memset(g_stageSlots[insertAtIdx], 0, 0x136C1C).
8. g_stageSlotFlags[insertAtIdx] = 1 (mark loaded/dirty).
9. ImportBgFromNamedFile(insertAtIdx, String) — actually load the file.
10. Post-import fixups (demo only):
     - AdjustWordIndexAfterInsert on all 8 demoConfig.titleDemoId[] slots (see system_settings.md §4.2).
     - For each loaded stage, AdjustWordIndexAfterInsert on the stage's script-item demo references.
11. ResetEditorViewState + MarkSlotModified.
```

Slot size constant `0x136C1D` = 1,271,837 bytes. This is one byte more than the `KgtProjectSlot` size of 1,271,828 (0x136814) documented in `ida_progress.md`; the extra bytes are part of a per-slot overallocation and the shift copies the full physical region rather than just the logical struct. (Consistent with the editor's `0x136C1C` memset one byte shorter, leaving the final byte as a guard.)

---

## §6  IDA address index

### Functions (new / clarified this session)

| Addr | Name | Comment |
|---|---|---|
| `0x435770` | `LaunchTestGame` | Top-level test-play runner; `WinExec("KGT2nd_GAME.exe /T")` etc. |
| `0x43C450` | `ExportTestPlayData` | Bundles project + characters + stages + demos to g_testPlayOutputDir (282 existing inline cmts) |
| `0x4336E0` | `AddSoundEntry` | Sound-list INSERT |
| `0x433BB0` | `DeleteSoundEntry` | Sound-list DELETE + script-ref fixup |
| `0x4335D0` | `RenameSoundEntry` | TextInputDialog + uniqueness check |
| `0x433DE0` | `SwapSoundEntries` | Swap + SwapWordIndices fixup |
| `0x404860` | `DispatchSoundPlayback` | Central dispatch: stop / WAV / MIDI / CD |
| `0x434AB0` | `PlayMidiFromSoundEntry` | Write temp MIDI + start sequencer |
| `0x4349A0` | `StartMidiPlayback` | MCI sequencer MCI_OPEN + MCI_PLAY |
| `0x434920` | `StopSoundPlayback` | MCI sequencer MCI_STOP + MCI_CLOSE (MIDI only) |
| `0x434960` | `DeleteTempMidiFile` | Legacy `enterbrain2dfightermak.mid` cleanup |
| `0x404810` | `StopAndFreeWavPlayback` | `sndPlaySoundA(NULL, SND_PURGE)` + free |
| `0x404940` | `CleanupPreviewState` | Unified stop (kind=0 all / 1 wav / 2 midi / 3 cd) |
| `0x434890` | `InitializeCDAudio` | CD init + drive-letter probe |
| `0x434760` | `PlayCDAudioTrack` | MCI cdaudio open/set/play |
| `0x434700` | `StopCDAudio` | MCI cdaudio stop/close (was `PlayNextCDAudioTrack`) |
| `0x4346D0` | `IsDriveCDROM` | `GetDriveTypeA == DRIVE_CDROM` |
| `0x425E20` | `ImportBgFromNamedFile` | Load `.stage` into slot |
| `0x425EB0` | `ImportBgFromFileDialog` | Dialog + slot-shift + Import |
| `0x427860` | `ImportDemoFromNamedFile` | Load `.demo` into slot |
| `0x4278F0` | `ImportDemoFromFileDialog` | Dialog + slot-shift + Import + ref-fixup |

### Globals (new this session)

| Addr | Name | Replaced |
|---|---|---|
| `0x44E678` | `g_cdMciDeviceId` | `mciId` |

### Globals (already-named, reconfirmed)

| Addr | Name | Size |
|---|---|---|
| `0x44E6F4` | `pszWavSoundBuffer` | 4 |
| `0x44E690` | `g_midiPlaybackActive` | 4 |
| `0x44E694` | `g_midiMciDeviceId` | 4 |
| `0x44E698` | `g_midiLoopFlag` | 4 |
| `0x44E67C` | `g_cdPlaybackActive` | 4 |
| `0x44E680` | `g_cdPlaybackLoopFlag` | 4 |
| `0x44E684` | `g_cdPlaybackError` | 4 |
| `0x44E68C` | `g_cdAudioInitialized` | 4 |
| `0x4451F8` | `g_cdRequestedTrack` | 4 |
| `0x4451FC` | `g_cdDriveLetter` | 1 |

### MCI command codes (reference)

| Code | Constant | Used by |
|---|---|---|
| `0x803` | `MCI_OPEN` | StartMidiPlayback, PlayCDAudioTrack |
| `0x804` | `MCI_CLOSE` | StopSoundPlayback, StopCDAudio |
| `0x806` | `MCI_PLAY` | StartMidiPlayback, PlayCDAudioTrack |
| `0x807` | `MCI_SEEK` | StartMidiPlayback (restart already-open sequencer) |
| `0x808` | `MCI_STOP` | StopSoundPlayback, StopCDAudio |
| `0x80D` | `MCI_SET` | PlayCDAudioTrack (set time format = TMSF) |
| `0x2200` | `MCI_OPEN_TYPE \| MCI_OPEN_ELEMENT` | Open flags |
| `0xD` | `MCI_FROM \| MCI_TO \| MCI_NOTIFY` | Play range with completion notify |
| `0x400` | `MCI_SET_TIME_FORMAT` | Time-format flag |
| `0x100` | `MCI_TO_START` | Seek to start |
| `0x2` | `SND_PURGE` (winmm) | StopAndFreeWavPlayback |

---

## See Also

- `docs/editor/ida_progress.md` — global project/slot structs, renamed functions, big-picture loader algorithm
- `docs/editor/system_settings.md` — the 1033-byte stage/demo trailer blocks written/read around the test-play export
- `docs/editor/game_ini_reference.md` — INI config that drives launch modes and paths
- `docs/editor/2DFM Codeblocks.md` — PLAY_SOUND (opcode 3) format targeted by the CRUD ref-fixup code
