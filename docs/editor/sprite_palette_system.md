# KGT2nd_EDITOR — Sprite / Picture / Palette Subsystem

> Audience: reverse-engineers porting the 2DFM/FM2 engine. This document maps the editor binary's sprite and palette data paths to the Hong Kong dev's `KgtPicture` / `KgtPalette` model in `2dfm/`. Pair with `docs/editor/ida_progress.md` (baseline structs, globals) and `2dfm/2dfm_binary_analysis.md` (file-format side).

## 1. On-disk pixel format (recap)

A `.kgt` project stores sprite frames inside the `KgtProjectSlot.pictureHeaders[8192]` array followed by a raw pixel blob for each one. Each entry is a 20-byte `KgtPictureSlotEntry`:

| Offset | In-memory name | On disk | Meaning |
|---|---|---|---|
| 0 | `void *data` | `unknownFlag1` (4 bytes, ignored) | After load: `GlobalAlloc()`ed pointer to pixel data |
| 4 | `int width` | width | in pixels |
| 8 | `int height` | height | in pixels |
| 12 | `int hasPrivatePalette` | 1 = first 1024 bytes of `data` is a `KgtPalette` |
| 16 | `int size` | compressed size, 0 = uncompressed |

The image is **1 byte per pixel** (palette index into one of the 8 shared palettes, or into the private palette if present). `width*height` bytes, row-major, no padding. If `hasPrivatePalette != 0`, the blob is preceded by a 1024-byte `KgtPalette` (256 × BGRA).

Project-level palettes live right after the picture-header array as `KgtPaletteWithPad sharedPalettes[8]` — 1024 bytes of `KgtColorBgra[256]` + 32 bytes of padding = 1056 bytes per slot.

### `KgtColorBgra` as seen from the renderer

```c
union KgtColorBgra {
    struct { byte blue; byte green; byte red; byte alpha; } channel;
    uint32_t value;
};
```

The editor treats `alpha` as an **active-slot flag** (1 = usable colour, 0 = unused / transparent). It is **never** an 8-bit alpha channel at runtime — the blitter treats palette index 0 as the transparent colour regardless.

## 2. Project-slot offsets (IDA decoded)

`g_projectSlot` is at `0x62ED80`. Inside a slot, picture and palette data live at:

| Struct field | Offset | Byte-count | Example in decompilation |
|---|---|---|---|
| `pictureHeaders[0].data` | 1088784 | 4 | `*(g_mainHwnd + 1088784 + 20*i)` |
| `pictureHeaders[0].width` | 1088788 | 4 | `*(ptr + 1088788)` |
| `pictureHeaders[0].height` | 1088792 | 4 | `*(ptr + 1088792)` |
| `pictureHeaders[0].hasPrivatePalette` | 1088796 | 4 | `*(ptr + 1088796)` |
| `pictureHeaders[0].size` | 1088800 | 4 | `*(ptr + 1088800)` |
| `sharedPalettes[0].colors[0].blue` | 1252624 | 1 | `*(g_mainHwnd + 4*i + 1252624)` |
| `sharedPalettes[0].colors[0].green` | 1252625 | 1 | `*(g_mainHwnd + 4*i + 1252625)` |
| `sharedPalettes[0].colors[0].red` | 1252626 | 1 | `*(g_mainHwnd + 4*i + 1252626)` |
| `sharedPalettes[0].colors[0].alpha` | 1252627 | 1 | `*(g_mainHwnd + 4*i + 1252627)` |
| Between consecutive palettes (incl. padding) | +1056 | | `1056 * g_selectedPaletteIdx` |

`g_mainHwnd` at `0x5F1320` is just "the base of the project-slot copy the editor keeps as its working set". It's confusingly named HWND but is treated as a `KgtProjectSlot *` in almost every sprite/palette function — the real main-window HWND is stored elsewhere.

## 3. Rendering pipeline

### 3.1 Framebuffer

The editor renders everything into a 16-bit (RGB555) software framebuffer. Key globals:

| Addr | Name | Purpose |
|---|---|---|
| `0x7D5670` | `ppvBits` | pointer to the framebuffer pixels (16bpp) |
| `0x602178` | `g_framebufferWidth` | width in pixels |
| `0x60217C` | `g_framebufferHeight` | height in pixels |
| `0x5F1684` | `g_clearColorIndex` (dword) | 5-bit "background" RGB used by `FillFramebufferRect` |
| `0x5F1688` | `g_gridColorRgb555` (dword) | 15-bit RGB555 grid line colour |
| `0x5F1694..` | `word_5F1694[8]` | 8 preset RGB555 colours used by the editor overlays |
| `0x5F1B6A..0x5F1B86` | `g_viewOrigin*`  | 4 per-editor-tab (x,y) origins — see §5 |
| `0x5F1B9E` | `g_previewZoomHalfScale` | 0 = 1:1, 1 = half-size StretchBlt preview |

The final present is a `BitBlt(hdc, ..., ::hdc, 0,0, SRCCOPY)` or a 2:1 `StretchBlt` when half-zoom is selected. `::hdc` is the global 16bpp backing DC used by the sprite-preview windows.

### 3.2 `BlitPalettedSprite` @ `0x4061B0` — the core blitter

Signature (after retype):

```c
__int16 __cdecl BlitPalettedSprite(
    const void   *pixels,      // 8bpp source pixel plane (palette indices)
    unsigned int *palette,     // 256 × 32-bit palette (see §3.3)
    int           dstX, dstY,  // destination in framebuffer coords
    int           srcW, srcH,  // sprite size
    int           srcX, srcY,  // offset inside the source plane
    int           flags,       // blend + flip + stride flags
    int           rOffset,     // per-channel offsets applied to palette
    int           gOffset,     //   before lookup — used for palette
    int           bOffset);    //   shifting / flashing / additive colour
```

#### How a palette index becomes a pixel

For each pixel `pixels[y*stride + x]`:

1. Read `idx = pixels[...]` (1 byte).
2. Look up `palette[idx]` — a 32-bit word whose byte layout is **b | g<<8 | r<<16 | alpha<<24**. The blitter does the work `>>3, >>11, >>19` to extract three 5-bit components `R5,G5,B5` and masks with `0x1F`. Note that this is equivalent to reading the bytes at offsets `+2, +1, +0` respectively and shifting right 3 — i.e. 8-bit-per-channel -> 5-bit-per-channel.
3. Add `rOffset / gOffset / bOffset` to each component and clamp to `[0..31]` (these offsets enable per-frame palette cycling/flashing — e.g. hit flashes, damage tint).
4. Pack into RGB555: `v60[idx] = (R5 << 10) | (G5 << 5) | B5`. If the resulting pixel would be exactly 0 (all-black after offsets), force it to 1 so it doesn't collide with the transparent sentinel.
5. Palette index 0 always yields 0 in `v60[0]` — that's the transparent colour.

The 256-entry `v60[]` LUT is built once at the start of the call (the preamble loop around `0x4062B0`). The inner blit loops then do `v60[src_pixel]` lookups only.

#### Blend modes (`flags & 7`)

| Mode | Meaning | Formula |
|---|---|---|
| 0 | Opaque (chroma-keyed on idx=0) | `if (v) *dst = v` |
| 1 | 50/50 alpha blend | `((dst>>1)&0x3DEF) + ((v>>1)&0x3DEF)` — classic 15-bit sum-of-halves |
| 2 | Additive saturating | `(dst & 0x7BDF) + (v & 0x7BDF)` with per-channel overflow subtract |
| 3 | Subtractive saturating | Per-channel `max(dst-v, 0)` on R/G/B fields |
| 4 | Programmable alpha | Uses `rOffset / gOffset / bOffset` as 0..31 blend weights for the three channels independently |
| 7 | Raw copy (no chroma-key) | Always `*dst = v` |

#### Flip / stride flags

| Bit | Meaning |
|---|---|
| `0x20000000` | Use a scratch stride from `dword_7D7A38` (`g_bmpEditorWidth`) — used when drawing the bmp-editor's live canvas |
| `0x40000000` | Vertical flip (reverse the row-pointer step) |
| `0x80000000` (sign) | Horizontal flip (decrement column step) |

Clipping: if `dstX + srcW > g_framebufferWidth`, `srcW` is clipped; same for height. Negative `dstX / dstY` shift the source origin.

### 3.3 Palette-pointer conventions

The blitter expects a `uint32_t palette[256]` where **each entry is the raw `KgtColorBgra::value`** (little-endian: `byte[0]=blue, byte[1]=green, byte[2]=red, byte[3]=alpha`). Callers pass one of:

- `&g_projectSlot.sharedPalettes[selIdx].colors[0]` — i.e. `g_mainHwnd + 1056*g_selectedPaletteIdx + 1252624`.
- For a sprite with a private palette: the first 1024 bytes of the sprite blob (which are exactly a `KgtPalette`).

The `extractPixelData()` path in `2dfm/KgtPicture.cpp` is the same idea, but there the runtime ports decode directly to a `ColorBgra*` buffer; in the editor, the blit happens on-the-fly against the 16bpp framebuffer so no per-frame ARGB buffer is materialised.

### 3.4 `RenderSpriteData` @ `0x4172F0`

Thin wrapper used by the main sprite-preview paint routine. Given a `KgtProjectSlot *` and a picture index, it:

1. Locks the `KgtPictureSlotEntry.data` handle via `GlobalLock`.
2. Updates the status bar to show "Private Pal" vs "Common Pal".
3. If `pictureHeaders[i].size != 0` (compressed): `GlobalAlloc` a scratch buffer, call `DecompressSpriteData(entry, scratch, locked_src, size)` (RLE decoder — not yet reversed in detail), then blit from the scratch.
4. If uncompressed and `hasPrivatePalette`: the palette pointer is the first 1024 bytes of the data; the pixel pointer is `data + 1024`.
5. Otherwise: the palette pointer is the active shared palette and the pixel pointer is `data`.
6. Calls `BlitPalettedSprite(pixels, palette, dstX, dstY, w, h, 0, 0, blitFlags, 0, 0, 0)`.
7. Status bar shows `size WxH Comp(uncompressed>compressed N%)` or `size WxH (bytes)`.

### 3.5 `SpritePreviewPanelWndProc` @ `0x4108E0`

The small "throw-reaction preview" panel. Double-clicks the currently-selected throw reaction's sprite, renders it into the 16bpp back-buffer by calling `BlitPalettedSprite` directly (not `RenderSpriteData`). Handles middle-mouse drag to pan the drawing origin `dword_6075D0/6075D4` and left-click drag to adjust a per-reaction offset inside `g_characterSlots[cur][throwReactions[i].origin]`.

### 3.6 `SpritePreviewWndProc` @ `0x417500`

The big 7.9 KB game-preview/frame-edit canvas. Paint handler breaks down by `g_currentEditorMode`:

| Mode | Purpose | Per-mode view origin |
|---|---|---|
| 0 | Character-sprite preview (Shiftable frame origin shown with crosshair) | `dword_5F1B72 / 5F1B76` for normal, `dword_5F1B7A / 5F1B7E` if "absolute pos" flag |
| 1 | Hit-reaction / hitbox editor (red/green rects, central alignment crosshair) | `dword_5F1B6A / 5F1B6E` |
| 2 | Stage / ringout preview (gravity-tied background) | `dword_5F1B82 / 5F1B86` |
| 3 | Demo preview | `dword_5F1B7A / 5F1B7E` |

Key behaviours:

- **Hitbox / pushbox overlay** (mode 1): walks backwards through the script items looking for opcode 0x18 (hurt box) and 0x19 (hit box); builds up to 20 box records in `dword_454BE0[]`, then draws 2-pixel-thick rectangles using `word_5F1694[colorIdx & 7]`.
- **Grid overlay** (mode 0): a two-line crosshair (`FillFramebufferRect` with 2-pixel-thick strokes) at the current cell's origin when the `& 2` or `& 8` flag is set in the script's opcode byte.
- **Half-zoom**: if `g_previewZoomHalfScale` is non-zero, `StretchBlt(hdc, 0,0, W/2,H/2, ::hdc, 0,0, W,H, SRCCOPY)` downsamples the back-buffer to the window.
- **Mouse interaction**: `WM_LBUTTONDOWN` starts dragging one of five "anchor pointers" (`dword_450254` indirects into `dword_5F1B6A..`) depending on `g_currentEditorMode`. The WM_MOUSEMOVE case clamps movement per-mode (e.g. gravity preview uses stage bounds derived from `(movePower + 100) << 6 / 10` formula).
- **Additional cell overlays**: for opcode 0x14 ("marker" or similar) a second crosshair is drawn; for opcode 0x07 (mirror/child-spawn) a secondary sprite is composited via `RenderSpriteData` on top using the same offset rules.

## 4. Palette editor

### 4.1 `PaletteEditorWndProc` @ `0x42DB30`

Window layout:

- Toolbar (different button set in mode 1 vs modes 0/2/3 — character editor gets extra palette-copy / palette-load buttons, stage/demo see fewer).
- 16×16 colour grid below the toolbar. Each cell shows `sharedPalettes[g_selectedPaletteIdx].colors[i]` as a `CreateSolidBrush(r<<16 | g<<8 | b)` fill. If `colors[i].alpha == 0` (inactive slot) a white dot is drawn so the user can see the "unused" entries.
- Cursor highlight: a white L-shaped bracket at `(g_paletteEditorCursorX, g_paletteEditorCursorY)`.
- Status bar: `"pal-cellIdx R:r G:g B:b"`.

Mouse:

- `WM_MOUSEMOVE` → recompute `g_paletteEditorCursorX/Y` from `(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)-26)` / cell-stride; repaint if changed.
- `WM_LBUTTONDBLCLK` (Msg 515) → call `ChooseColorA` on the hovered cell. If the user picks a colour that already exists elsewhere in the same palette, prompt ("This color already exists — use the existing slot?"). The new RGB is force-quantised to the top 5 bits (masked `& 0xF8`) before write.

Menu / toolbar commands (WM_COMMAND, `Msg == 273`):

| ID | Action |
|---|---|
| `0xB3B1..0xB3B8` | Select shared palette 0..7 (toolbar radio); updates `g_selectedPaletteIdx`. |
| `0xB3BB` | `SortPaletteDarkToLight` (@ `0x42D230`) |
| `0xB3BC` | `SortPaletteByChannel` (@ `0x42D530`) |
| `0xB3C4` | `ReducePaletteColors` (@ `0x42D780`) |

All three ops always run across **all 8 shared palettes simultaneously** so that a given sprite's pixel indices remain valid no matter which palette the user picks to render it with — they only reorder / merge entries in a *parallel* fashion.

### 4.2 Sorting ops

- **`SortPaletteDarkToLight` @ `0x42D230`**: sort key = `(r>>3)+(g>>3)+(b>>3)`; active slots get a `+1` tie-break bias, inactive-all-zero slots get `-1` so they settle at the high end. Implemented as a naive O(n²) selection sort that swaps **every one of the 8 palettes in lockstep** using `SwapByte` over 8×1056-byte strides. On completion, builds `g_paletteRemapTable[256]` and calls `ShowPaletteRemapDialog`.
- **`SortPaletteByChannel` @ `0x42D530`**: sort key = dominant channel. Algorithm:
  1. Build identity map `v17[i] = i`.
  2. For decreasing `i = 31..0`: call `FindAndSwapPaletteColor(i, i, i, &cursor, v17)` — places "greys" (R==G==B) first, ordered by brightness.
  3. Three nested loops for each channel dominance: pass 1 places colours where `R == max`, pass 2 where `G == max`, pass 3 where `B == max`.
  4. Populate `g_paletteRemapTable` from `v17` and call `ShowPaletteRemapDialog`.
- **`FindAndSwapPaletteColor` @ `0x42D3E0`**: scan from `*cursor` upward in the currently-selected palette for the first **active** slot whose 5-bit `(R,G,B)` matches `(r5,g5,b5)`. If found: swap it with `*cursor` (via 4×`SwapByte` per palette × 8 palettes — the inner loop steps by `1056 / 4 = 264 dwords` = 1056 bytes per palette), increment `*cursor`, update `backMap`.

### 4.3 Colour reduction (`ReducePaletteColors` @ `0x42D780`)

1. Count active slots (alpha byte != 0) in the selected palette.
2. Pop up `ColorCountInputDlg` (id `DialogNameInputDlg`) asking for `1 .. activeCount-1`. User input → `g_paletteReduceTargetCount`.
3. Until `activeCount == target`:
   - For every pair `(a, b)` of active slots where `b > a`, compute similarity = `sumOf5bit/10 + squaredDistance8bit(a,b) per channel` (cheap approximation — the 8-bit squared distance dominates). Track the pair with minimum cost.
   - Average their 8-bit RGB (`(A + B + 7) / 2 & 0xF8`, so it stays within the 5-bit-quantised lattice) and store into slot `a` **across all 8 palettes**.
   - Zero out slot `b` in all 8 palettes (alpha byte cleared — slot marked inactive).
   - `g_paletteRemapTable[b] = a`; propagate — any previous entries that mapped to `b` are re-routed to `a`.
4. After convergence: call `ShowPaletteRemapDialog` to rewrite every sprite's pixel bytes.

### 4.4 `ShowPaletteRemapDialog` / `PaletteRemapDlgProc`

Iterates all 8192 `pictureHeaders` entries (`stride = 20`). For each sprite with non-null data and non-zero width×height:

- If `size != 0` (compressed): `DecompressSpriteData` into `lpString` (a global scratch), rewrite each pixel as `lpString[i] = g_paletteRemapTable[lpString[i]]`, `GlobalFree` the old data, `GlobalAlloc` fresh uncompressed storage, clear `size`, copy.
- If uncompressed: `GlobalLock`, in-place remap through the 256-entry table, `GlobalUnlock`.

Private-palette sprites still remap their pixel bytes (the 1024-byte private palette at the front is skipped because the `width*height` length variable covers only the pixel area).

The progressbar at `0x451674` is advanced one tick per picture.

## 5. BMP editor (`BmpEditor`)

### 5.1 Frames and globals

A separate top-level editor (tab `dword_451600`) for painting / importing raw 8bpp bitmaps.

| Addr | Name | Role |
|---|---|---|
| `0x7D7600` | `g_bmpEditorBMI` (`BITMAPINFO` + 256 RGBQUADs) | The canvas' DIB descriptor |
| `0x7D7A28` | `g_bmpEditorMemDC` | `CreateCompatibleDC`'d memory DC |
| `0x7D7A2C` | `g_bmpEditorDIBSection` | HBITMAP of the `CreateDIBSection` result |
| `0x7D7A30` | `g_bmpEditorOldDIBObj` | Old HGDIOBJ stashed from the `SelectObject` |
| `0x7D7A34` | `g_bmpEditorPixels` | Writable 8bpp pixel plane |
| `0x7D7A38` | `g_bmpEditorWidth` | |
| `0x7D7A3C` | `g_bmpEditorHeight` | |
| `0x7D7A40` | `g_bmpEditorColorCount` | Active palette size (≤ 256) |
| `0x7D51A0..0x7D51AC` | selection rect (x0,y0,x1,y1) | |
| `0x7D51E0 / 0x7D51E4` | canvas size | |
| `0x451628 / 0x45162C` | view scroll (x,y) | |

### 5.2 `BmpEditorMainWndProc` @ `0x42B9B0`

Creates a rebar + toolbar (icons 0xA41B..0xA41F = import, open, paste, crop, fill; 0xA420..0xA422 = three mode toggles). Three toolbar state bits:

| Global | Toolbar id | Meaning |
|---|---|---|
| `dword_5F19D4 & 1` | `0xA420` | "Lock pixel-edit" mode — mutually exclusive with `0xA421` |
| `dword_5F19CC` | `0xA421` | "Pan mode" — mutually exclusive with `0xA420` |
| `dword_5F19D0` | `0xA422` | "Auto-crop on selection release" |

`HandleBmpEditorHotkey` @ `0x42B8E0` maps `R/I/O/P/A/F` keys to menu commands `0xA411, 0xA41B..0xA41F` (reload, import, open, paste, apply, fill). These are dispatched via `HandleMenuCommand`.

### 5.3 `BmpCanvasWndProc` @ `0x42BF40`

The painting surface. Mode variable `dword_5F16B8`:

| Mode | Cursor | On LBUTTONDOWN |
|---|---|---|
| 0 | crosshair (rect select) | Track `(dword_7D51A0..AC)` selection rectangle. |
| 1 | 4-way arrow | Floating 320×240 marquee. |
| 2 | 4-way arrow | Floating 640×480 marquee. |
| 3 | 4-way arrow | Floating user-sized marquee (`dword_5F16BC x dword_5F16C0`). |

Painting (WM_PAINT, `Msg == 15`):

1. `CreateRenderSurfaceFromWindow(hWnd)` — binds `::hdc` (the 16bpp backing DC) to the canvas rect.
2. `FillFramebufferRect(0,0,W,H, 0, g_clearColorIndex)` — clear.
3. `BlitPalettedSprite(g_bmpEditorPixels, g_bmpEditorBMI.bmiColors, offsetX, offsetY, imageW - scrollX, imageH - scrollY, scrollX, scrollY, 0x20000007, 0, 0, 0)` — renders the bmp using the DIB's own RGBQUAD palette. Flag `0x20000007` = "use the scratch stride + raw copy" (mode 7, no chroma-key) — the bmp canvas has no transparent colour.
4. Draw a 4-pixel-thick dashed selection outline via `PatBlt` with a stippled rop depending on the `byte_451660` animation counter (so the marquee crawls).

The right-click + middle-drag codepaths pan the view; right-click drag also extends the selection (if mode 0) or moves the marquee.

### 5.4 BMP import — `LoadBmpFileToSpriteDIB` @ `0x406C10`

Flow:

1. `CreateFileA(filename, READ)` → `GlobalAlloc` whole-file buffer → `ReadFile`.
2. `CleanupDIBSection(g_bmpEditorBMI)` — tear down any previous import.
3. `ZeroMemoryBlock(g_bmpEditorBMI, 0x444)` — clear BITMAPINFO + 256-RGBQUAD array.
4. Use BMP header `biClrUsed` (offset +46) or derive palette size from `biBitCount` (+28): 1bpp→2, 4bpp→16, 8bpp→256, 24bpp→0 (synthesised later).
5. Validate: only 1/4/8/24 bpp are accepted; other formats produce `ShowStatusMessage("the BMP file has strange format")`.
6. Copy `width / height` from `biWidth` / `biHeight`. Stride padding: 4-byte for ≥4bpp, 8-byte for 1bpp.
7. Fill in the output DIB header (biBitCount=8, biPlanes=1, biCompression=BI_RGB, biHeight = -height for top-down DIB).
8. Copy the BMP palette into `g_bmpEditorBMI.bmiColors[]` as B,G,R,alpha=1. (24bpp path synthesizes a 3-3-2 fixed palette instead — see below.)
9. `CreateCompatibleDC` + `CreateDIBSection(... DIB_RGB_COLORS ...)` producing a writable 8bpp surface. The pixels-pointer is returned into `g_bmpEditorPixels`.
10. Decode pixels:
    - 1 bpp: unpack the bit pattern; each set bit becomes palette index 1, clear bit 0. Rows are flipped (`i + w*(h - row - 1)`).
    - 4 bpp: two pixels per source byte; hi-nibble first.
    - 8 bpp: direct copy.
    - 24 bpp: quantize each RGB triple to 3-3-2 bits: `idx = (R & 0xE0) + ((G & 0xE0) >> 3) + ((B & 0xE0) >> 6)`. Synthesises a matching 256-entry palette (`R = idx>>5 * 32, G = ((idx>>3) & 7) * 32, B = (idx & 3) * 64`).
11. `RemapPaletteIndices(colorCount)` de-duplicates the palette (important for sloppily-saved 24bpp BMPs and for merging duplicate entries from 8bpp BMPs).

### 5.5 `RemapPaletteIndices` @ `0x4068D0`

For each of the `colorCount` palette entries, walk the already-deduplicated list and find a match by exact RGB comparison. If not found, append the new RGB at position `uniqueCount++`. Store the destination index in `remap[src]`. Slots `uniqueCount..255` are zeroed. Final pass: rewrite every pixel of `g_bmpEditorPixels` with `pixel = remap[pixel]` — now indices `0..uniqueCount-1` are live and the rest of the palette is unused.

## 6. Other related windows

| Addr | Name | Role |
|---|---|---|
| `0x40BD10` | `CommonImageListWndProc` | *(Misnamed — actually shows throw-reaction names)* Scrollable list of the 200 `KgtThrowReaction` entries. Handles up/down drag-reorder (swaps via `SwapCommonImages`). |
| `0x40C550` | `CommonImageContainerWndProc` | Thin parent that just hosts the list above. |
| `0x410010` | `CommonImageListPanelWndProc` | Same data as above but inside a different tab. |
| `0x411A60` | `CellButtonWndProc` | A single cell/frame-index button used in the character-scripts grid. Draws two tile bitmaps from `hdcSrc` (one for the "action type" and one for the "index type") + the bound `g_characterSlots[cur][82*y+31]` text. Right-click opens the hit-junction popup. |

## 7. Where 2dfm/KgtPicture.cpp matches the editor

| `KgtPicture::setFrom2dfmPicture` step | Editor equivalent |
|---|---|
| `width/height/hasPrivatePalette = header.*` | `ReadCommonResourcePart@0x428be0` (documented in `ida_progress.md`) |
| `decompress(content, ...)` if `size != 0` | `DecompressSpriteData` inside `RenderSpriteData@0x4172f0` (not yet fully reversed) |
| `privatePalette = createPalette(content, true)` if `hasPrivatePalette` | The first 1024 bytes of `pictureHeaders[i].data` are used directly — the editor never materializes a separate `KgtPalette` object in this case. |
| `setSharedPalettes(palettes)` | The editor keeps them in-place in `g_projectSlot.sharedPalettes[]`; selection is just the index `g_selectedPaletteIdx`. |
| `extractPixelData` — builds ARGB per-pixel buffer | The editor skips this: `BlitPalettedSprite` renders 8bpp→16bpp on-the-fly into the framebuffer. |

## 8. IDA addresses referenced in this document

| Addr | Function / data | Purpose |
|---|---|---|
| `0x4061B0` | `BlitPalettedSprite` | Core 8bpp → 16bpp blit with blend modes + palette-shift offsets |
| `0x4068D0` | `RemapPaletteIndices` | Dedup BMP palette after import |
| `0x406A50` | `CleanupDIBSection` | Tear down DIB memory DC / HBITMAP |
| `0x406A90` | `InitializeRenderContext` | Zero the 16bpp DC + bmp-editor DC at startup |
| `0x406C10` | `LoadBmpFileToSpriteDIB` | BMP file → 8bpp internal DIB |
| `0x4108E0` | `SpritePreviewPanelWndProc` | Throw-reaction preview panel |
| `0x410010` | `CommonImageListPanelWndProc` | Throw-reaction list panel |
| `0x40BD10` | `CommonImageListWndProc` | Throw-reaction list (alt location) |
| `0x40C550` | `CommonImageContainerWndProc` | Parent container for the list above |
| `0x411A60` | `CellButtonWndProc` | Per-cell button in script grid |
| `0x4172F0` | `RenderSpriteData` | Sprite decompress + `BlitPalettedSprite` wrapper |
| `0x417500` | `SpritePreviewWndProc` | 7.9 KB main game-preview canvas |
| `0x42B5E0` | `ApplyBmpEditMode3` | → `CaptureImageFrameFromSelection(3)` |
| `0x42B5F0` | `ApplyBmpEditMode0` | → `CaptureImageFrameFromSelection(0)` |
| `0x42B8E0` | `HandleBmpEditorHotkey` | R/I/O/P/A/F key dispatch |
| `0x42B9B0` | `BmpEditorMainWndProc` | Bmp-editor outer window + toolbar |
| `0x42BF40` | `BmpCanvasWndProc` | Bmp-editor painting surface |
| `0x42CFE0` | `PaletteRemapDlgProc` | Progressbar dialog for palette remap |
| `0x42D1C0` | `ShowPaletteRemapDialog` | Launch A101 dialog → remap + refresh |
| `0x42D230` | `SortPaletteDarkToLight` | Brightness sort across 8 palettes |
| `0x42D3E0` | `FindAndSwapPaletteColor` | Per-slot swap helper |
| `0x42D530` | `SortPaletteByChannel` | Dominant-channel sort across 8 palettes |
| `0x42D630` | `ColorCountInputDlgProc` | "Enter new color count" dialog |
| `0x42D780` | `ReducePaletteColors` | Pairwise-merge colour reduction |
| `0x42DB30` | `PaletteEditorWndProc` | 16×16 palette grid + toolbar |
| `0x435850` | `ChooseColorForPalette` | UI-color ChooseColorA wrapper |

### Globals

| Addr | Name | Struct |
|---|---|---|
| `0x5F16C8` | `g_selectedPaletteIdx` | 0..7 |
| `0x451688` | `g_paletteEditorCursorX` | column in 16×16 grid |
| `0x45168C` | `g_paletteEditorCursorY` | row in 16×16 grid |
| `0x4543C0` | `g_paletteRemapTable[256]` | sorting / reduce remap |
| `0x4547C0` | `g_paletteReduceTargetCount` | ColorCountInputDlg result |
| `0x7D7600` | `g_bmpEditorBMI` | BITMAPINFO of bmp-editor DIB |
| `0x7D7A28` | `g_bmpEditorMemDC` | HDC |
| `0x7D7A2C` | `g_bmpEditorDIBSection` | HBITMAP |
| `0x7D7A30` | `g_bmpEditorOldDIBObj` | old HGDIOBJ |
| `0x7D7A34` | `g_bmpEditorPixels` | 8bpp pixel plane |
| `0x7D7A38` | `g_bmpEditorWidth` | int |
| `0x7D7A3C` | `g_bmpEditorHeight` | int |
| `0x7D7A40` | `g_bmpEditorColorCount` | active palette size |
| `0x602178` | `g_framebufferWidth` | int |
| `0x60217C` | `g_framebufferHeight` | int |
| `0x5F1B9E` | `g_previewZoomHalfScale` | 0/1 |
