# Wave D Rename Report — `WonderfulWorld_ver_0946.exe` `.data 0x4F0000..0x544000`

Final tail-of-`.data` cleanup for the game binary. Complementary to
`docs/editor/ida_progress.md` (editor baseline), `docs/editor/wave_d_renames.md`
(editor Wave D — same CRT patterns apply to the game binary), and
`docs/editor/editor_globals.json`.

## Scope & Tally

Range contains `g_app_name@0x541F7C` (47 xrefs). Despite its prominence,
the surrounding region is **almost entirely MSVC 6.0 CRT internal state**,
not game state or Windows API handles. All DirectDraw / DirectPlay /
DirectSound handles for this binary live further up at `0x4246E0..0x424774`
(`g_ddraw_primary_surface`, `g_dplay_interface`, etc. — already named).

| Category | Before | After |
|---|---|---|
| Auto-named (`dword_`, `byte_`, `word_`, `unk_`) in range | 23 | **0** |
| Renamed to CRT standard names | — | 17 |
| Renamed to game char-slot field names | — | 6 |
| User-named globals in range already | 8 | 31 (+23) |

Every auto-named global in `0x4F0000..0x544000` is now resolved.

## Cluster 1 — MSVC small-block heap (`0x541FA0..0x541FB4`, 5 renames)

The classic `_sbh_*` scalar state block used by MSVC 6.0's Small-Block
Heap. Identified by decompiling `___sbh_heap_init@0x419F20`,
`___sbh_free_block@0x419FB0`, `___sbh_alloc_block@0x41A2F0`, and
`___sbh_alloc_new_region`.

| Addr | xrefs | Old | New | Evidence |
|---|---|---|---|---|
| `0x541FA0` | 3 | `dword_541FA0` | `__sbh_sizeHeapList` | `___sbh_heap_init` sets to `16` (initial heap list capacity) |
| `0x541FA4` | 5 | `dword_541FA4` | `__sbh_indGroupDefer` | Index of deferred group (0..63), written by free/alloc paths |
| `0x541FA8` | 4 | `dword_541FA8` | `__sbh_pHeapNext` | Rolling scan pointer through heap-region list, set by `___sbh_heap_init` to the first region |
| `0x541FAC` | 13 | `dword_541FAC` | `__sbh_pHeapDefer` | Region pointer pending `VirtualFree`; `free_block` checks it and defers release |
| `0x541FB0` | 8 | `dword_541FB0` | `__sbh_cntHeapList` | Count of regions in the list; used as end sentinel in scans (`lpMem + 20 * cnt`) |

## Cluster 2 — MBCS / locale state (`0x541FB8..0x5420E1`, 4 renames)

MSVC's multibyte-character-set state. Identified in `__setmbcp@0x4196B0`,
`setSBCS@0x4198A0`, `setSBUpLow@0x4198E0`.

| Addr | xrefs | Old | New | Evidence |
|---|---|---|---|---|
| `0x541FB8` | 8 | `dword_541FB8` | `__mbcodepage` | `_setmbcp` first checks/writes the active code page here; `setSBCS` resets to 0 |
| `0x541FCC` | 4 | `dword_541FCC` | `__mbctype_isMbcs` | Set to 1 when `CPInfo.MaxCharSize > 1`, 0 when SBCS |
| `0x541FE0` | 4 | `byte_541FE0` | `__mbcasemap` | 256-byte case-map array populated from `LCMapStringA` results (to-upper / to-lower) |
| `0x5420E1` | 12 | `byte_5420E1` | `__mbctype` | The `__mbctype[257]` classification array (MBCS lead-byte/letter flags). All writes OR-in bits `4`, `8`, `0x10`, `0x20` — the canonical `_MS`/`_MP`/`_UP`/`_LO` constants. |

The `0x541FC0..0x541FC8` `unk_` aliases (codepage info triplet) were
already IDA-`unk_`-labeled and are **outside the 23-auto scope**; they
are effectively the `__mbulinfo` cpinfo record.

## Cluster 3 — stdio state (`0x5421E8, 0x543220`, 2 renames)

| Addr | xrefs | Old | New | Evidence |
|---|---|---|---|---|
| `0x5421E8` | 7 | `dword_5421E8` | `__piob` | `___initstdio` calls `calloc(Count, 4)` and stores the `FILE*` table here; `_fcloseall`/`_flsall` iterate it. Classic `__piob` |
| `0x543220` | 9 | `dword_543220` | `__pioinfo` | Array of pointers to 256-byte `_ioinfo` blocks. `__ioinit` uses `dword_543220[k >> 5] + 8 * (k & 0x1F)` — the textbook MSVC indexing for `__pioinfo[k]` |

## Cluster 4 — startup / exit state (`0x543324..0x543338`, 6 renames)

Startup-vector, at-exit table, and command-line pointer. Identified in
`start@0x417AD0`, `__wincmdln`, `__setargv@0x4188A0`,
`__setenvp@0x418820`, `_cinit@0x4184C0`, `_doexit@0x418510`.

| Addr | xrefs | Old | New | Evidence |
|---|---|---|---|---|
| `0x543324` | 1 | `dword_543324` | `__env_initialized` | `__setenvp` sets to 1 after environment pointer array is built |
| `0x543328` | 5 | `dword_543328` | `__mbctype_initialized` | Checked by `__setargv`/`__setenvp` before calling `__initmbctable`; set by `__initmbctable` |
| `0x54332C` | 1 | `dword_54332C` | `__onexitend` | End ptr of the atexit fn table; `_doexit` walks `v4 = __onexitend - 4` down to `__onexitbegin` |
| `0x543330` | 2 | `dword_543330` | `__onexitbegin` | Base of atexit fn table (walked by `_doexit`) |
| `0x543334` | 1 | `dword_543334` | `__matherr_init_fn` | `_cinit` calls it indirectly if non-zero — MSVC's `_matherr_init` / `_initterm_e` hook |
| `0x543338` | 3 | `dword_543338` | `__acmdln` | `start` writes `GetCommandLineA()` result here; `__wincmdln` and `__setargv` read it |

## Cluster 5 — per-character-slot round-action flag (`0x4FBE05..0x541F40`, 6 renames)

**Not CRT, not Windows API — game state.** The six `dword_` addresses
were parallel assignments in `vs_round_function@0x409250` at offset
`0x409371..0x40938F`:

```asm
; at 0x409371..
mov dword_4EDDC6, eax  ; slot 0 (already named, outside range)
mov dword_4FBE05, eax  ; slot 1  (+0xE03F)
mov dword_509E44, eax  ; slot 2  (+0xE03F)
mov dword_517E83, eax  ; slot 3  (+0xE03F)
mov dword_525EC2, eax  ; slot 4  (+0xE03F)
mov dword_533F01, eax  ; slot 5  (+0xE03F)
mov dword_541F40, eax  ; slot 6  (+0xE03F)  ← last one, just before g_app_name
```

Stride `0xE03F` = 57407 bytes per character-slot block — same block size
as `g_character_data_base@0x4D1D80` and `g_char_slot_data@0x435474`
regions. At `0x409250..0x40929A` the same field is looped over via
`for (eax = &dword_4EDDC6; eax < 0x54FF7F; eax += 0xE03F)`, confirming
7 parallel character slots.

| Addr | xrefs | Old | New |
|---|---|---|---|
| `0x4FBE05` | 1 | `dword_4FBE05` | `g_charSlot1_round_action_flag` |
| `0x509E44` | 1 | `dword_509E44` | `g_charSlot2_round_action_flag` |
| `0x517E83` | 1 | `dword_517E83` | `g_charSlot3_round_action_flag` |
| `0x525EC2` | 1 | `dword_525EC2` | `g_charSlot4_round_action_flag` |
| `0x533F01` | 1 | `dword_533F01` | `g_charSlot5_round_action_flag` |
| `0x541F40` | 1 | `dword_541F40` | `g_charSlot6_round_action_flag` |

Slot 0 stays as `dword_4EDDC6` (outside this wave's range — listed here
only to anchor the pattern). Follow-up: when a `KgtCharacterSlot` struct
is declared, these 7 fields collapse into a single field access at
`char_slot[N].round_action_flag` (offset unknown; its address within
the 57 KB slot places it ~1% from slot end, so likely a tail/resetflag
field).

## Surprises

1. **The "prominent anchor" `g_app_name` at 0x541F7C is an island inside
   CRT state.** Its 47 xrefs suggested a window-class/app-init region,
   but the surrounding 23 autos are 17 CRT internals + 6 per-slot
   character flags. The window-class / DirectDraw / DirectPlay handles
   this region hinted at are actually at `0x4246E0..0x424774` — far
   below in `.data`. Wave D's anchor was misleading.

2. **No Windows API handles, no HWNDs, no DirectDraw surfaces in this
   range.** Everything Windows-API-like in WonderfulWorld is in the
   `0x424xxx..0x447xxx` region (already named in prior waves).

3. **MSVC 6.0 CRT layout is identical between `KGT2nd_EDITOR.exe`
   (editor) and `WonderfulWorld_ver_0946.exe` (game).** The editor's
   Wave D doc (`docs/editor/wave_d_renames.md` §7) names the same
   subsystem globals in the editor (`0x7D7C4C..0x7D8FD4`). Same MSVC
   version, same static library — expected. A future follow-up could
   extract these 17 CRT names into a single `crt_globals.md` shared by
   both binaries.

4. **The `g_charSlot{1..6}_round_action_flag` addresses straddle the
   end of `.data`** — `0x541F40` is *8 bytes before* `g_app_name@0x541F7C`.
   The 7 character slots are packed with very little slack. This also
   confirms that `g_char_data_array_end@0x541F78` is the correct end
   sentinel for the array.

## Follow-ups

1. **Declare `KgtCharacterSlot` struct** (57,407 = 0xE03F bytes) and
   retype `g_char_slot_data` / the 7-slot array starting at slot 0. All
   6 auto-addresses renamed in this wave would then resolve as struct
   field access `g_charSlots[N].round_action_flag`.
2. **Factor out CRT globals into `docs/game/crt_globals.md`**. This wave
   named 17 CRT internals that also exist in the editor binary at
   different addresses. A single shared reference would save future
   cross-referencing effort.
3. **Verify `__matherr_init_fn@0x543334`** by checking the `_cinit`
   indirect call target at runtime — IDA has no xrefs to it beyond
   `_cinit`, so it's likely `NULL` and the indirect call is a no-op in
   this build. If so, rename to `__initterm_e_fn` or similar.
4. **`__onexitbegin/__onexitend` table population**: the single xref
   to each (besides `_doexit`) is from code that doesn't install any
   atexit handlers in this binary — FM2K likely doesn't register any
   runtime cleanup. Confirm by watching for `_onexit` calls.

## See also

- `docs/editor/ida_progress.md` — global IDA state baseline
- `docs/editor/wave_d_renames.md` §7 — CRT internals in the editor binary
- `docs/editor/wave_a_renames.md` §3 — similar CRT cluster at
  `0x44DAC4 / 0x44DCD4` (`__ctype` locale tables)
