// frontend/Theme.h — design tokens, font handles, and small drawing
// helpers shared by every shell view. Production version of what lived
// in FM2K_LauncherUI_DesignSandbox.cpp.
//
// The palette here is the NERV direction (Eva orange/purple/phosphor on
// near-black). Switching palettes (terminal/ice/vapor/amber/blood) is a
// follow-up beyond M1; the structure of this header anticipates it
// (replace kNerv with a runtime-selected pointer when we get there).
#pragma once

#include "imgui.h"

namespace fm2k::shell {

// ── Palette ──────────────────────────────────────────────────────────
// 12-color design palette. Code references `kNerv.<field>` everywhere;
// kNerv is the ACTIVE palette mutated at runtime. Themes live in
// Theme.cpp's kThemes[] table. Cycle/select via SetTheme(idx).
//
// Role guide for new code:
//   bg     canvas background
//   bg2    raised surfaces (panels, headers, rail)
//   ink    primary text
//   ink2   secondary text (less emphasis)
//   dim    tertiary text (eyebrows, helper)
//   faint  near-disabled text + faint glyphs
//   line   borders + dividers
//   hr     faint translucent divider (alpha-low)
//   acc    accent / active / brand
//   phos   positive / live / online indicator
//   amber  warning / pending
//   red    danger / error / mute
struct NervPalette {
    ImU32 bg, bg2, ink, ink2, dim, faint, line, hr, acc, phos, amber, red;
};
extern NervPalette kNerv;

// Available palettes. SetTheme(idx) swaps kNerv to that palette and
// persists `theme_index` to settings.ini. CycleTheme() advances + wraps.
enum class Theme {
    // Originals
    Nerv = 0,        // Eva orange / purple / phosphor (legacy default)
    Terminal,        // CRT green-on-black
    Amber,           // VT220 amber-on-black
    Ice,             // cyan / blue-grey
    Blood,           // red / dark
    Mono,            // greyscale, no chromatic accent

    // Neon / retro
    Synthwave,       // magenta + cyan on deep purple
    Vaporwave,       // pastel pink + cyan
    Matrix,          // bright green Matrix-style
    Cyberpunk,       // yellow + magenta neon
    Plasma,          // purple/magenta
    Neon,            // hot pink + cyan

    // Nature / mood
    Forest,          // deep greens
    Ocean,           // teal + deep blue
    Sunset,          // warm gold / orange
    Sakura,          // soft pink + cream
    Copper,          // bronze + warm
    Emerald,         // emerald + teal
    Crimson,         // dark red + gold
    Navy,            // navy + light blue

    // Popular dev palettes (close approximations)
    Dracula,         // purple + pink (Dracula-ish)
    Gruvbox,         // retro warm yellow/orange
    Nord,            // cool muted blue/slate
    TokyoNight,      // dark + neon blue/purple
    Catppuccin,      // mocha — soft dark pastel
    RosePine,        // rose / gold / muted
    SolarizedDark,   // solarized dark accents

    // Light
    PaperWhite,      // light mode — dark text on white
    SolarizedLight,  // solarized light scheme

    kCount
};
const char* ThemeName(Theme t);
void        SetTheme(Theme t);
void        CycleTheme();
Theme       CurrentTheme();

// ── AppShell layout constants — base design pixels ──────────────────
// All numeric layout constants are authored at this canonical scale.
// At runtime we render at NATIVE window pixels; layout multiplies
// every constant by `g_ui_scale` (= window_w / kBaseDesignW) so the
// design tracks the window without SDL upscaling. Result: ImGui's
// dynamic font baking rasterizes glyphs at native pixel size, no
// blur from re-sampling.
//
// 640×450 is the canonical design canvas — every spatial literal in
// the views was authored against this. Pinning kBaseDesignW at 640
// (not 1280) means the default 1280×900 window renders at ui_scale
// = 2.0 — every layout literal + every font auto-scales 2× without
// any per-call wrapping. The minimum 640×450 window renders at 1×.
// 1920×1350 renders at 3×, and so on.
constexpr float kBaseDesignW = 640.0f;
constexpr float kBaseDesignH = 450.0f;

// Layout constants — call sites multiply by g_ui_scale. Do NOT bake
// the scale here so the constants stay legible.
constexpr float kCanvasMinW = 640.0f;
constexpr float kCanvasMinH = 450.0f;
constexpr float kTitlebarH  = 32.0f;
constexpr float kRailW      = 48.0f;
constexpr float kRightW     = 154.0f;
constexpr float kTopH       = 30.0f;
constexpr float kStatusH    = 18.0f;

// Per-frame UI scale. Set in fm2k::shell::Render() entry from the
// current ImGui DisplaySize. DrawTextF reads this to scale glyph
// rasterization size; layout code uses S(x) = x * g_ui_scale to
// scale every magic pixel.
extern float g_ui_scale;
inline float S(float x) { return x * g_ui_scale; }
inline int   Si(int x)  { return (int)(x * g_ui_scale); }

// Logical-space x where the titlebar's tab strip actually ends. Set
// each frame by DrawShellTitlebar; the SDL hit-test in
// FM2K_RollbackClient.cpp reads it (via the extern declaration there)
// to make the empty space PAST the last tab DRAGGABLE while keeping
// the tabs themselves NORMAL (clickable). Without this, the
// hit-test had to either over-cover (NORMAL up to 560 logical, so
// drag-on-empty-titlebar broke) or under-cover (NORMAL up to 360,
// so far-right tabs in Hub mode weren't clickable).
extern float g_titlebar_tabs_end_logical_x;

// ── Font handles ─────────────────────────────────────────────────────
// Populated by LoadFonts() (called from LauncherUI::Initialize before
// the SDL renderer backend init). nullptr-safe — callers should
// PushFontIf / PopFontIf, not raw PushFont.
//
// Two parallel sets:
//   * Primary "Eva-pass" handles — what nerv.css uses by default for the
//     AppShell / SetupController / LogoIntro / Hub. DM Serif Display
//     (italic display), JetBrains Mono (body + labels + micro).
//   * Pixel handles — the `nv-bitmap` art direction used ONLY by the
//     BootCrawl route per design intent. VT323 / Silkscreen /
//     DotGothic16 / Press Start 2P.
extern ImFont* g_font_display;  // DM Serif Display Italic — large numerals, headings
extern ImFont* g_font_body;     // JetBrains Mono — body text, log lines
extern ImFont* g_font_label;    // JetBrains Mono — labels, telemetry kv (used at smaller sizes)
extern ImFont* g_font_micro;    // JetBrains Mono — eyebrow microtype (smallest)

extern ImFont* g_font_pixel_display;  // DotGothic16 — bitmap-mode display (BootCrawl READY)
extern ImFont* g_font_pixel_body;     // VT323 — bitmap-mode body
extern ImFont* g_font_pixel_label;    // Silkscreen — bitmap-mode labels
extern ImFont* g_font_pixel_micro;    // Press Start 2P — bitmap-mode microtype

// ── Font size multipliers (dev tune) ─────────────────────────────────
// DrawTextF multiplies its `size` arg by the scale matching the font
// class passed in. Default 1.0 — the per-call sizes were dialed for
// 1× — but the dev panel can crank them when the launcher's running
// at high DPI / large monitor and the eyebrow micro reads as too
// fine. Persisted to settings.ini under tune_font_*.
extern float g_font_display_scale;
extern float g_font_body_scale;
extern float g_font_label_scale;
extern float g_font_micro_scale;

void LoadFonts();
void PushFontIf(ImFont* f);
void PopFontIf (ImFont* f);

// ── Drawing helpers ──────────────────────────────────────────────────
// Color mix in linear-ish space. Alpha-naïve — fine for chrome accents,
// don't use for compositing semi-transparent overlays.
ImU32 MixCol(ImU32 a, ImU32 b, float t);

// Two-stripe region "flag" chip, 12×8. Falls back to neutral grays for
// unknown region codes.
void DrawFlagDot(ImDrawList* dl, ImVec2 p, const char* region);

// Tiny ping-wave canvas — colored sine that gets jittery + redder at
// high ping. Driven by ImGui::GetTime() so animation is automatic.
void DrawPingWave(ImDrawList* dl, ImVec2 p, int ping,
                  float w = 28.0f, float h = 8.0f);

// Convenience: dl->AddText with optional font, falling back to current
// font when f is null. Saves a null-check at every call site.
void DrawTextF(ImDrawList* dl, ImFont* f, float size,
               ImVec2 pos, ImU32 col, const char* s);

}  // namespace fm2k::shell
