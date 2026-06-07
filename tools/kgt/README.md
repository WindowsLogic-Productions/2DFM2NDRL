# kgt — 1:1 byte-for-byte round-trip parser for FM2K `.kgt` files

Python tool that parses FM2K project files, dumps them to JSON for
inspection / diffing / regression detection, and packs JSON back to the
original byte layout — **every byte accounted for, byte-exact**.

## Format coverage

Supports the FM2K family:
- `2DKGT2K\0` (modern: Wonderful World, ReSHUFFLE, vanpri, most)
- `2DKGT2G\0` (G-variant: AOB et al)

FM95 family (`KGTGAME\0`, `2DKGT95\0`) is **gracefully skipped** —
different fixed-offset layout, separate parser needed.

**Verified across 146 files (809 MiB) from `C:\games\2dfm`, `C:\dev\fm95`,
`C:\Users\teo\Downloads\_nicotine`, `D:\games\fm2k`:**
- 114 / 114 FM2K files: round-trip byte-exact
- 32 / 32 FM95 files: gracefully skipped
- 0 hard failures

## Quick start

```bash
# Summary
python3 kgt.py info "path/to/game.kgt"

# Verify round-trip
python3 kgt.py verify "path/to/game.kgt"

# Full JSON
python3 kgt.py parse "path/to/game.kgt" -o out.json

# Rebuild from JSON
python3 kgt.py pack out.json rebuilt.kgt

# Byte-level coverage map (every section labeled)
python3 kgt.py coverage "path/to/game.kgt"

# Extract assets — sprite frames as 8-bit BMP, sounds as WAV/MIDI,
# palettes as 16×16 swatch BMPs. Decompresses RLE-packed frames
# automatically. Frames without a private palette use shared palette 0
# (override with --palette 0..7).
python3 kgt.py extract "path/to/game.kgt" out_dir/
```

The `extract` command produces three sub-directories:
- `frames/0042_640x480.bmp` — one BMP per non-empty sprite-frame slot, named with `<index>_<width>x<height>.bmp`. 8-bit indexed Windows-3.x format with the embedded palette baked in (private palette if the frame has one, otherwise the shared palette you select with `--palette`).
- `palettes/0.bmp` — eight 16×16 swatch BMPs, one per shared palette.
- `sounds/0007_ファイト.wav` — WAV/MIDI/raw payload per sound entry, named with `<index>_<sound_name>.<ext>`. CP932/SJIS/GBK names preserved.

The RLE decompressor is ported from `Unity2dfmRuntime/Assets/Scripts/2dfmFile/DecompressUtil.cs` (4-opcode format: zero-fill / verbatim / byte-fill / back-reference, with 6-bit lengths and two-stage extension).

## Coverage on WonderfulWorld_ver_0946.kgt (4.48 MiB)

```
OFFSET         END       BYTES  SECTION
0x00000000  0x00000010      16  file_signature (sig+version)
0x00000010  0x00000110     256  project_name
0x00000110  0x00000114       4  scripts.count
0x00000114  0x00001509    5109  scripts[131]
…  
0x0046f7d4  0x0046f7d8       4  freeze_time_marker (always 2)
0x0046f7d8  0x0046f7d9       1  recover_time_gap
0x0046f7d9  0x0046f7da       1  block_hitstop_frames
0x0046f7da  0x0046f7db       1  hit_hitstop_frames
0x0046f7db  0x0046f7dc       1  collision_effect_type
0x0046f7dc  0x004729dc   12800  stage_infos[50] (g_stage_file_buffer)
…
0x0047a6e8  0x0047a7f0     264  score_digit_sprites_array (132 × u16)
0x0047a7f0  0x0047a80c      28  char_sel_config (14 × 2B)
0x0047a80c  0x0047a83e      50  player_selectable_infos[50]
0x0047a83e  0x0047abf0     946  reserved_tail_946 (all-zero pad)

file total:  4697072 bytes  (4.48 MiB)
accounted:   4697072  (100.0000%)
```

## Field naming policy

Where the 010 Editor template, the C++ `2dfmFileReader.cpp` axmol reader,
and the IDA disassembly of `WonderfulWorld_ver_0946.exe` disagreed about
the semantics of a byte, **IDA names win** — that's what the actual game
runtime reads. Specifically:

| 010 / C++ name | Our name | Why |
|----------------|----------|-----|
| `atkFreezeTime` (RecoverTimeConfig.attackRecoverTime) | `block_hitstop_frames` | IDA: `g_block_hitstop_frames`, used by `hit_detection_system` |
| `dfsFreezeTime` (RecoverTimeConfig.defenceRecoverTime) | `hit_hitstop_frames` | IDA: `g_hit_hitstop_frames` |
| `cclFreezeTime` (RecoverTimeConfig.clashRecoverTime) | `collision_effect_type` | IDA: `g_collision_effect_type`, used by `ProcessHitboxCollisions` |
| `unknownGapBytes[264]` ("位置数据") | `score_digit_sprites_array` (132 × u16) | IDA: `g_score_digit_sprites_array`, used by `DisplayScoreNumbers` |
| `unknown996Bytes[996]` | split: `player_selectable_infos[50]` + `reserved_tail_946` | C++ reader names first 50; remaining 946 always-zero pad |
| `generalSettings` (int32) | `general_settings` + `general_settings_bits` | Added bitfield decode (ProjectBaseConfig union) |

## What's still labeled "unknown"

After our naming pass, only **3 bytes per file** carry "unknown" labels:
- `unknown_demo_id_1` and `unknown_demo_id_2` (2 bytes) — last 2 bytes of
  `GameDemoConfig`. C++ also calls them `unknownTag1/2`. No IDA xrefs.
- `recover_time_gap` (1 byte) — first byte of `RecoverTimeConfig`. No IDA
  xrefs in the WW runtime; LoadStageFile/WinMain/settings_dialog_proc do
  reference the *block* at this address but via the stageFileBuffer name,
  not this byte.

These 3 bytes are kept verbatim for round-trip. **Everything else is
named with a real semantic purpose**.

## JSON output schema

```jsonc
{
  "file_signature": "32444b4754324b000000000001000000",  // hex (sig + 8B version flags)
  "project_name": "...",                                  // hex with trailing NUL+garbage
  "project_name_str": "WonderfulWorld_ver_0946",          // decoded (cp932/gbk/latin-1)

  "scripts": [{ "name": "<hex>", "name_str": "...", "script_index": 0, ... }],
  "script_items": [...],
  "sprite_frames": [{ "width": 84, "height": 41, ..., "frame_content_b64": "..." }],
  "palettes": [...],   // always 8
  "sounds": [...],

  "player_name_gap": 0,
  "player_infos": [...],  // 50 × {raw: hex, name_str: ...}
  "reactions": [...],     // 200

  "freeze_time_marker": 2,           // always 2 in our corpus
  "recover_time_gap": 0,             // RecoverTimeConfig.gap (no IDA xrefs)
  "block_hitstop_frames": 4,         // g_block_hitstop_frames
  "hit_hitstop_frames": 4,           // g_hit_hitstop_frames
  "collision_effect_type": 20,       // g_collision_effect_type

  "stage_infos": [...],  // 50
  "demo_infos": [...],   // 100

  "title_demo_id": 1,
  "char_sel_for_1p_demo_id": 2,
  ...
  "unknown_demo_id_1": 3,            // GameDemoConfig.unknownTag1 — vestigial
  "unknown_demo_id_2": 0,            //   "                .unknownTag2 — vestigial

  "general_settings": 76,
  "general_settings_bits": {         // decoded ProjectBaseConfig
    "encrypt_game": false,
    "allow_clash": false,
    "enable_story_mode": true,
    "enable_1v1_mode": true,
    "enable_team_mode": false,
    "show_hp_after_bar": false,
    "press_to_start": true,
    "raw": 76
  },

  "throw_reactions": [...],  // 200

  "score_digit_sprites_array": "<hex>",        // 264B raw
  "score_digit_sprites_array_u16": [0,1,2,...],// 132 × u16 (semantic view)

  "char_sel_start_pos_x": 296, ... // 14 × i16/u16 fields

  "player_selectable_infos": [3,3,3,...,0,0],  // 50 bytes (CSS eligibility per slot)
  "reserved_tail_946": "<hex>"                 // 946 bytes (always zero in observed corpus)
}
```

## Use case (Xem's pattern: catching editor corruption)

```bash
# Snapshot before editing
python3 kgt.py parse work.kgt -o before.json

# …edit in the editor (Ctrl+S, add hit junction, etc.)…

# Diff in canonical form
python3 kgt.py parse work.kgt -o after.json
diff <(jq -S . before.json) <(jq -S . after.json) | head -200
```

JSON is sorted-key-friendly so `diff <(jq -S ...)` gives clean field-level
deltas. If a hex `_raw` blob diverges but the matching `_str` stays the
same, that's exactly the kind of editor-corruption signature you're
hunting.

## Adding `.player` / `.demo` / `.stage` support

The 010 templates exist:
- `Unity2dfmRuntime/Docs/2dfm-player.bt` (44 lines)
- `Unity2dfmRuntime/Docs/2dfm-stage.bt`  (45 lines)
- `Unity2dfmRuntime/Docs/2dfm-demo.bt`   (51 lines)

Factor `Script` / `ScriptItem` / `SpriteFrame` / `Palette` / `Sound` out
of `kgt.py` — they're the shared "common resource" records. Then write
thin wrappers for the per-file-type config tails.

## Adding FM95 support

FM95 uses a 0x78D48-byte fixed-size game-system block with hard-coded
offsets (no count-prefixed sections). See `FM2K_KgtParser.cpp` lines
200-228 for the offsets:

```
g_kgt_data              file offset 0
g_player_file_name_array          0x110
g_stage_file_name_array           0x3FA8
g_demo_file_name_array            0x71A8
```

Detect via `KGTGAME\0` / `2DKGT95\0` signature, branch into a separate
`KgtFM95.parse()` path.
```
