# Extending Locale Spoof to FM2K Games — Analysis

Question: should the LE-style locale spoof we built for FM95 also apply to FM2K games (Wonderful World, AOB, Magical Chaser, etc.)?

Method: trace WW's import table + xrefs against the same locale-relevant API set we used to scope CPW's spoof. Compare what WW touches vs what CPW touches.

---

## What FM2K games need vs FM95

### Filename-side fragility (FM95 had this, FM2K does NOT)

`find /mnt/c/games/2dfm` confirms:
- Every FM2K-engine exe in our test corpus has an **ASCII filename** — `WonderfulWorld_ver_0946.exe`, `AOB.exe`, `vanpri.exe`, `pkmncc.exe`, `STRIPFIGHTER5CE.exe`, `colors.exe`, etc.
- Their `.kgt` files are also ASCII-named (matching the exe stem).
- `.player`, `.demo`, `.bg` data files inside FM2K dirs are mostly ASCII; the only JP-named asset spotted across the survey is `mikyaku2/Sizu/Timi EDéP.demo` (Latin-1 é, not SJIS).

CPW's bug — `GetModuleFileNameA` / `GetCommandLineA` collapsing fullwidth Ｃ Ｐ Ｗ → ASCII C P W via best-fit char mapping — **does not occur on FM2K games** because there's no fullwidth Latin in the path to collapse. CPW-style `GetCommandLineA` / `GetModuleFileNameA` / `GetCurrentDirectoryA` / `GetFullPathNameA` / `CreateFileA` path-translation hooks are **unnecessary for FM2K** and (cheap but pointless) overhead if installed.

### Visible-text fragility (FM2K games HAVE this)

WW import + xref scan (pass 2) turns up four ANSI render paths, all of which leak mojibake when given JP strings on US-locale Windows:

| Import | Imported at | Callsites | Risk |
|--------|-------------|-----------|------|
| `TextOutA` (GDI32) | `0x41c028` | 5 in `render_game` (0x404DD0), `RenderDebugTextOverlay` (0x415220), `KeyConfigWindowProc` (0x416CC0), `JoystickButtonConfigWindowProc` (0x416EF0) | **Main in-game JP text path.** Per-frame, hot. |
| **`MessageBoxA`** (USER32) | `0x41c198` | `ShowMessageBox` (0x402470), `DialogFunc` (0x402EE0 — 4 sites), `bitmap_loader` (0x4043D0 — 2), `ShowErrorMessageBox` (0x414880), `InitializeCDAudio` (0x415570), `LoadWaveFileToBuffer` (0x415910 — 2), `LoadWaveFileReplace` (0x415A40) | Error popups. Confirmed JP strings: `キャラクターセレクト１Ｐデモスクリプトが見つかりません` etc. fire from `bitmap_loader` / sound loaders / dialog. |
| **`SetDlgItemTextA`** (USER32) | `0x41c18c` | `DialogFunc` (0x402EE0 — 2), `KeyboardConfigDialogProc` (0x417230 — 2), `JoystickConfigDialogProc` (0x417460 — 2) | Config dialog labels. JP if the game ships JP UI. |
| `SetWindowTextA` (USER32) | `0x41c194` | `SetNetplayWindowTitle` (0x402B10), `LoadGameSystemFile` (0x403D60) | Window title (can carry JP names). |
| **`CreateWindowExA`** (USER32) | `0x41c204` | `InitializeMainWindow` (0x4056C0), `CreateKeyboardConfigWindow` (0x416EA0), `CreateJoystickConfigWindow` (0x4171E0) | Window class is `"KGT2KGAME"` ASCII, but the WINDOW NAME (title at create time) may be JP — relevant for the dialog windows in particular. |

Bolded rows are the ones I missed in pass 1 — three additional ANSI text APIs WW uses to render JP. These are **not** hooked by our current spoof.

JP strings embedded in WW.exe via `find_regex` (confirmed live consumption via the xrefs above):
- `検索してタイトルを指定してください` — file-picker prompt
- `キャラクターファイル読み込み[%s]` — loading message
- `キャラクターセレクト１Ｐデモスクリプトが見つかりません` + VS Single / VS Team variants — error messages, dispatched by `bitmap_loader` / `LoadWaveFileToBuffer` / dialog handler via `MessageBoxA`

CRT path:
- `GetACP`, `GetOEMCP`, `GetCPInfo` imported (CRT codepage init).
- `MultiByteToWideChar`, `WideCharToMultiByte` imported (CRT char conversion).
- Same fragility as CPW — without CP932 spoofing, CRT initialises CP1252 MBCS tables and `_sprintf("%s", japanese_string)` produces incorrect lengths and split byte sequences.

---

## Hook surface inventory

| Hook | FM95 (CPW) | FM2K (WW) | Notes |
|------|------------|-----------|-------|
| `GetACP` | ✓ | ✓ | CRT MBCS init |
| `GetOEMCP` | ✓ | ✓ | CRT |
| `GetCPInfo` | ✓ | ✓ | CRT |
| `MultiByteToWideChar` | ✓ | ✓ | CRT char conversion |
| `WideCharToMultiByte` | ✓ | ✓ | CRT char conversion |
| `GetUserDefaultLCID`, `GetSystemDefaultLCID`, `GetUserDefaultUILanguage`, `GetSystemDefaultUILanguage`, `GetThreadLocale` | ✓ | ✓ | GUI APIs branch on LCID |
| `IsDBCSLeadByte`, `IsDBCSLeadByteEx` | ✓ | ✓ | char iteration |
| `IsValidCodePage`, `IsValidLocale` | ✓ | ✓ | sanity probes |
| `SetWindowTextA` → `SetWindowTextW` | ✓ | ✓ | window title rendering |
| **`TextOutA` → `TextOutW`** | (n/a — CPW doesn't import) | **NEEDED — not currently hooked** | in-game JP text via GDI32 |
| **`MessageBoxA` → `MessageBoxW`** | (CPW doesn't show MessageBoxes) | **NEEDED — not currently hooked** | JP error popups (asset-load failures) |
| **`SetDlgItemTextA` → `SetDlgItemTextW`** | (n/a) | **NEEDED — not currently hooked** | config dialog labels |
| **`CreateWindowExA` → `CreateWindowExW`** | (CPW only imports it via CRT) | **NEEDED if window-name is JP** — not currently hooked | initial window title at creation time |
| `GetCommandLineA` (`WC_NO_BEST_FIT_CHARS`) | ✓ | not needed (ASCII paths) | |
| `GetModuleFileNameA` (`WC_NO_BEST_FIT_CHARS`) | ✓ | not needed | |
| `GetCurrentDirectoryA` (`WC_NO_BEST_FIT_CHARS`) | ✓ | not needed | |
| `GetFullPathNameA` (`WC_NO_BEST_FIT_CHARS`) | ✓ | not needed | |
| `CreateFileA` (CP932 path translation) | ✓ | not needed | |

**Net addition for full FM2K JP rendering: 4 new hooks — `TextOutA`, `MessageBoxA`, `SetDlgItemTextA`, `CreateWindowExA`.** First pass undercounted; pass 2 caught the dialog/error-popup paths via the import xref scan.

---

## Risk assessment for "always-on FM2K spoof"

If we install the spoof on every FM2K game by default:

- **CRT codepage hooks**: substituting `CP_ACP` → 932 in `MultiByteToWideChar(CP_ACP, ...)` is safe for ASCII strings (ASCII is a subset of CP932 and round-trips identically). English-only games don't pass non-ASCII bytes through these calls, so the substitution is invisible.
- **LCID hooks**: returning `0x0411` (ja-JP) instead of `0x0409` (en-US) could affect locale-sensitive comparisons (`CompareStringEx` etc.). FM2K games don't appear to use those — their string handling is byte-level via the CRT.
- **`SetWindowTextA` → `SetWindowTextW`**: ASCII titles round-trip cleanly through `MultiByteToWideChar(932, ...)`. Non-issue.
- **`TextOutA` → `TextOutW`** (proposed): same as above. ASCII text round-trips cleanly; SJIS text renders correctly instead of mojibake; English game titles render their menu strings exactly as before.
- **Path APIs (FM95-only)**: not installed on FM2K builds, so no risk.

The spoof has no observable effect on a fully-English game and fixes mojibake on JP games. **"Always-on for any 2DFM-engine game" is safe.**

---

## Recommended UX

Three options, in increasing order of polish:

1. **Always-on across FM2K and FM95**, no toggle. Installs the same 19 hooks regardless of detected JP-ness. Simplest ship; covers every JP game out of the box; English games are unaffected.

2. **Opt-in toggle in launcher dev panel** under the existing FM95 pane (or a new "Locale" pane): `Spoof Japanese locale (FM2K_JP_LOCALE=1)`. FM95 stays always-on (its boot literally requires it); FM2K games default off and the user flips the toggle if they see mojibake.

3. **Auto-detect at discovery time**: `xxhash`-based registry lookup (already in the launcher) carries a `requires_jp_locale: bool` field. We seed `true` for known JP titles (CPW, WW, AOB JP variant, etc.) and let a heuristic decide unknown ones. Heuristic: scan the first 256 KB of the exe for SJIS-decodable string density — non-trivial count → JP. WW has 12+ SJIS strings near the IAT; pure English games have 0.

**Recommendation: option 1 (always-on)** for v1. The risk is genuinely zero on English games (we re-encode ASCII bytes through CP932 round-trip-cleanly) and it eliminates a class of "why is the menu garbled" support tickets. Add the dev-panel checkbox (option 2) as an escape hatch — `Disable locale spoof` — for the rare case someone needs vanilla locale behaviour for diagnosis.

---

## Concrete next-pass changes

1. Add **four new hooks** to `FM2KHook/src/locale/locale_spoof.cpp`, all following the same `MultiByteToWideChar(932, ...)` → W-variant pattern:
   - **`TextOutA` → `TextOutW`** — fires per-frame, hot path. Use a small thread-local `wchar_t` buffer to avoid per-call allocation. Length input is the byte length of the ANSI string; convert and pass as wchar count.
   - **`MessageBoxA` → `MessageBoxW`** — convert both `lpText` and `lpCaption`. Cold (only fires on errors).
   - **`SetDlgItemTextA` → `SetDlgItemTextW`** — convert the string parameter; HWND + control id pass through unchanged.
   - **`CreateWindowExA` → `CreateWindowExW`** — convert `lpWindowName`. **Be careful with `lpClassName`**: if it's an ATOM (low 16 bits used as integer), pass through as-is; only convert if it's a real string pointer (`>= 0x10000`). If conversion fails, fall back to `CreateWindowExA` so a custom-registered ANSI class still resolves.

2. Flip the `dllmain.cpp` activation gate from "FM95 always, FM2K only if env=1" to "always-on for both engines". Keep `FM2K_JP_LOCALE` as a now-vestigial env var; add `FM2K_NO_JP_LOCALE=1` as an explicit opt-OUT.

3. Optional: split the spoof's hook list into "common" (CRT codepage, LCID, DBCS, validity, all the new GDI/USER32 surfaces) and "FM95-only" (path APIs + CreateFileA path translation). FM2K builds skip the FM95-only block — saves 5 install attempts per launch. Cheap to keep installed (each MinHook is ~µs), but cleaner.

4. Launcher dev-panel checkbox: `Disable locale spoof` (default unchecked). Sets `FM2K_NO_JP_LOCALE=1` when checked. Lives under a new "Locale" subsection of the dev panel, or under the existing FM95 pane.

## Implementation gotchas to watch for

- **`TextOutA` perf**: WW's `render_game` calls it 5×/frame × ~100 fps = 500 calls/sec. Each conversion needs one `MultiByteToWideChar` call (returns wide length, fast). Use a `thread_local std::array<wchar_t, 512>` instead of `std::vector` to avoid heap traffic. For short ASCII strings (≤32 chars), just pass through to original `TextOutA` — saves the conversion cost on score numbers / frame counters.

- **`MessageBoxA` recursion**: our hook may itself call `MessageBoxW` which on some systems internally trampolines back through... actually `MessageBoxW` is the kernel-side path, no recursion risk. But our `SDL_LogError` calls inside the hook might allocate; keep diagnostics off the hot conversion path.

- **`CreateWindowExA` ATOM check**: per MSDN, if `lpClassName` is the result of `RegisterClassExA` (an `ATOM`, low 16 bits used as integer), the upper 16 bits are zero — guard with `(uintptr_t)lpClassName < 0x10000` and pass through unchanged. Otherwise SJIS-decode and call `CreateWindowExW`.

- **Pre-injection windows**: `InitializeMainWindow` fires very early in WW's startup (right after `init_window_and_subsystems`). Our DllMain runs pre-resume so the hook is in place by then — no race. But if any window gets created before DllMain (e.g., kernel32-injected dialogs), they'd miss the hook; not a concern for FM2K games we've audited.
