# Porting KGT2nd_EDITOR.exe → KGT2nd_GAME.exe

The editor `compiles` projects into the game runtime — they share a common engine core
(script interpreter, runtime objects, file format, renderer). Everything in the overlap
can be ported from the editor IDA DB (finished 2026-04-23) into the game IDA DB cheaply.

## What was exported from the editor IDA

| File | Entries | Purpose |
|---|---|---|
| `editor_funcs.json` | 495 | User-named functions: `{addr, name, size, signature}` |
| `editor_func_comments.json` | 72 | Human-authored top-of-function narratives (high signal) |
| `editor_inline_comments.json` | 1461 across 167 functions | Inline instruction-level comments (filtered ≥15 chars to skip Hex-Rays auto-hints) |
| `editor_func_fingerprints.json` | 336 | `{strings, immediates}` per function — for binary-agnostic matching |
| `editor_globals.json` | 898 | All user-named globals: `{addr, name, size, xrefs, type}` |
| `editor_engine_core_decomp.json` | 31 | Decompiled text of the engine-core functions most likely to exist in the game binary |
| `editor_types.json` | 155 | Local type library (includes our 44 Kgt/Parameter structs + IDA built-ins) |

Total on disk: ~580KB JSON. These are meant to be consumed by the porting script, not read by hand.

## Porting scripts

| Script | Purpose |
|---|---|
| `port_structs_to_game.py` | Minimal: declares 51 types in the target IDA, verifies sizes |
| `port_to_game_master.py` | Full pipeline: declare types + fingerprint-match functions + apply renames + port comments |

## Usage

In the target IDA (e.g. KGT2nd_GAME.exe or WonderfulWorld_ver_0946.exe):

```python
# File → Script File... → port_to_game_master.py
# OR paste contents into the Output window Python prompt.

load_dumps()
declare_all_types()

# Dry run first — review proposed matches
matches = match_all_functions(min_score=5)

# When happy, apply (will skip already-named functions)
apply_matches(matches, min_score=10, dry_run=False)

# For globals — use the histogram to hand-match
match_globals_by_size()
```

## What will port cleanly

| Domain | Will port | Why |
|---|---|---|
| **File-format structs** (KgtFileHeader, KgtProjectSlot, KgtGameSystemData, etc.) | ✅ Always | File format is identical — game reads same files the editor writes |
| **Script-item opcode structs** (KgtShowPic, KgtMoveCmd, etc.) | ✅ Always | Same reason |
| **Runtime structs** (KgtRuntimeObject, KgtPlayerRuntimeSlot, KgtAfterimageEntry) | ✅ Likely | Script interpreter is presumably shared source between editor's animation-preview and game's test-play |
| **Engine-core functions** (ExecuteAnimationScript, ProcessObjectPhysics, ProcessObjectCollisions, ReadCommonResourcePart, BlitPalettedSprite) | ✅ By fingerprint | Shared source |
| **Sprite/palette pipeline** | ✅ By fingerprint | Same |
| **Sound playback** (DispatchSoundPlayback, WAV/MIDI/CD lanes) | ⚠️ Partial | Game probably has richer audio; some overlap |
| **Sound CRUD** (AddSoundEntry, DeleteSoundEntry etc.) | ❌ Editor-only | Game doesn't edit sound entries |
| **WndProcs, menus, dialogs, editor state** | ❌ Editor-only | Not in game |
| **INI parsing, game.ini reference** | ❌ Editor-only | Game has its own config if any |

## Match quality expectations

Fingerprint matcher uses `score = string_overlap * 5 + immediate_overlap`. Empirical confidence by score:

- **≥ 20**: essentially certain (multiple unique strings shared)
- **10-19**: high confidence, review first
- **5-9**: candidate — verify by decompiling
- **< 5**: low — hand-match or use structural fingerprints

Functions unlikely to match well:
- Small utility functions that share boilerplate (e.g. `ZeroMemoryBlock`, sprintf wrappers)
- Functions where the editor uses a specific string but the game uses a different one
- Hex-Rays auto-decomped stubs / `nullsub_*`

## Beyond functions — what else to hunt

After porting by fingerprint, look for the game's versions of these by **structural** pattern:

1. **The object pool** (editor: `g_objectPool@0x775900`, stride 382, 1024 entries = 391KB region)
   - Search for a contiguous ~391KB region in .data that's touched by a function doing `base + 382*N` pattern.
2. **The per-player slots** (editor: `&unk_4551E0`, stride 47851, at least 8 entries)
   - Search for a region that's aligned to 47851 bytes and referenced with `47851 * i` multiplication.
3. **The afterimage pool** (editor: `g_afterimagePool@0x6075E0`, stride 404, 100 entries)
   - Region ~40KB, stride 404.
4. **The game-system data block** (editor: `g_kgtGameSystemData@0x765594`, 66108 bytes)
   - Stores game-wide config loaded from `.kgt` file.

Once you find these, apply the Kgt* types and all the offset-based accesses will auto-resolve.

## The "+0xDD asymmetry" TODO (from `runtime_entity.md`)

Editor's `ProcessObjectCollisions` reads attacker hitbox array at `+0xD9` and defender hurtbox array at `+0xDD` (4-byte skew). These arrays overlap in memory — either:
- (a) it's one 21-slot array, attacker reads [0..19], defender reads [1..20]; or
- (b) there's a special attacker-only 4-byte field just before the shared 20-slot box array

In animation preview (editor) the collision code runs but no real combat occurs, so both box arrays are always NULL. This can only be resolved against live in-game memory — **run Probe 4 in the game binary**.

## The `.uro_` TODO (resolved 2026-04-23 in editor)

Empty vestigial PE section, `VirtualSize=1`, `SizeOfRawData=0`, zero xrefs, zero relocations. Likely from `#pragma section("uro", ...)` declaration that was never populated. Not pursuing further unless it shows up populated in game binary.

## Questions not answerable from editor binary alone

1. **Input handling** — game binary exclusively (editor has no test-play input subsystem).
2. **Hitstop / pause timing** — game exclusively; editor's "pause" is a different system (see `engine_bugs_mapped.md` §12).
3. **Physics collision resolution** (beyond what animation preview exercises).
4. **Sound playback in-combat** — editor has the pipeline stubs; game uses them for real.
5. **Netplay, AI, save states** — game only.
6. **The exact `KgtCpuEntry` / `KgtStoryEntry` / `KgtAfterimageEntry` layouts** — fields TBD; payload blobs in the struct library.
