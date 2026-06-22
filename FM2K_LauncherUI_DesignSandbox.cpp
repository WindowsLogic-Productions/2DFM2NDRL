// FM2K_LauncherUI_DesignSandbox.cpp — see header.
//
// Smoke test for porting claude.ai/design `ColdLaunch` (01 cold boot) from
// the NERV-themed React/anime.js prototype into ImGui + ImAnim. The
// prototype timing comes straight from launcher.jsx:8 — 7 stages with
// cumulative thresholds [400, 1000, 1700, 2300, 3100, 3800, 5000] ms.
// We approximate the chrome (NvFrame / NvHeader / NvTelemetry) at low
// fidelity for now; a faithful pixel port follows once the toolchain is
// proven.
#include "FM2K_LauncherUI_DesignSandbox.h"

#include "imgui.h"
#include "imgui_internal.h"  // ImHashStr — same dep ImAnim relies on
#include "im_anim.h"

#include <array>
#include <cstdio>

namespace fm2k::sandbox {

namespace {

// ─── persistent UI state ──────────────────────────────────────────────────
struct ColdBootState {
    bool   open         = false;
    double anim_start_s = 0.0;   // ImGui::GetTime() at last (re)start
    bool   armed        = false; // first-frame init
};
ColdBootState g_cold_boot;

struct StyleGuideState {
    bool   open        = false;
    double phase_start = 0.0;    // anchor for the easing-lab timeline
    bool   armed       = false;
    bool   auto_loop   = true;   // restart easing demo every loop_dur seconds
    float  loop_dur    = 2.0f;
};
StyleGuideState g_style;

// ─── AppShell mock state ─────────────────────────────────────────────────
// END-TO-END NAVIGABLE port of app.jsx's AppShell — 640×448 grid with
// LeftRail / TopBar / RightRail / status bar, internal view routing
// across 9 views. All data mock for now (taken verbatim from the
// prototype); real-state wiring happens later after the chrome lands.
enum AppView {
    kView_Lobby = 0,
    kView_Browse,
    kView_Replays,
    kView_Rankings,
    kView_Stats,
    kView_Events,
    kView_Profile,
    kView_Cfg,
    kView_Map,
    kView_Count
};

struct AppShellState {
    bool   open         = false;
    AppView view        = kView_Lobby;
    int    current_game = 0;        // index into kGames
    int    user_hover   = -1;       // for right-rail row hover anim
};
AppShellState g_appshell;

// ─── Mock data tables (verbatim from app.jsx ─ keeps the design honest)
struct GameEntry { const char* id; const char* code; const char* short_name;
                   ImU32 tint_a; ImU32 tint_b; };
constexpr GameEntry kGames[] = {
    { "kof98", "KOF98",  "KING OF FIGHTERS '98",  IM_COL32(0x5a,0x2a,0x85,0xff), IM_COL32(0xff,0x30,0x30,0xff) },
    { "3s",    "SF3-3S", "SF III \xc2\xb7 3rd STRIKE", IM_COL32(0x00,0x40,0xa0,0xff), IM_COL32(0x4a,0xda,0xcc,0xff) },
    { "kof02", "KOF02",  "KOF 2002",              IM_COL32(0x00,0x00,0x00,0xff), IM_COL32(0xff,0x5a,0x3c,0xff) },
    { "k02p",  "KOF02P", "KOF 2002 PLUS",         IM_COL32(0x3d,0x1c,0x5e,0xff), IM_COL32(0xff,0xb9,0x38,0xff) },
};

struct UserEntry { const char* name; const char* region; int ping; bool in_match; };
constexpr UserEntry kUsers[] = {
    { "B2K (MVR)",       "MX",  22, false },
    { "GDK-TWK-KUMBIA",  "AR", 145, true  },
    { "ElRukoKofero",    "MX",  28, false },
    { "neftali.selene",  "MX",  78, false },
    { "DeathKaizer",     "BR", 142, false },
    { "elbebe",          "ES",  18, false },
    { "ALK_SaDen",       "ES",  64, false },
    { "Helikorn",        "DE",  88, false },
    { "~DanStroke~",     "FR",  54, false },
    { "[AFL]ChinitoJR",  "MX", 110, false },
    { "MVR-FC98.2002",   "MX",  32, false },
    { "SNEIDER3434",     "AR", 156, false },
    { "Jhonatan332",     "MX",  92, false },
    { "Soy el Bonus",    "ES",  14, false },
    { "Dmsilverio",      "BR", 168, false },
    { "purypury",        "DE",  76, false },
    { "lorraynecba",     "BR", 138, true  },
    { "Dnb^",            "ES",  22, false },
    { "semcombozueiro",  "BR", 145, false },
    { "perrolento2",     "MX", 102, false },
};

struct ActiveMatch { const char* a; const char* b; const char* charA; const char* charB;
                     int scoreA, scoreB, firstTo; const char* room; int ping; int spectators; };
constexpr ActiveMatch kActiveMatches[] = {
    { "elbebe",         "DeathKaizer",  "TERRY",   "IORI",    1, 0, 3,  "#6F2A",  18, 14 },
    { "Helikorn",       "purypury",     "KYO",     "K'",      2, 2, 5,  "#A14B",  78, 38 },
    { "GDK-TWK-KUMBIA", "SNEIDER3434",  "MAI",     "ATHENA",  0, 1, 3,  "#B208", 142,  4 },
    { "lorraynecba",    "Dmsilverio",   "RYO",     "TAKUMA",  4, 5, 7,  "#D44C",  56, 62 },
};

// 2-color region "flag" — pulled from app.jsx's FlagDot map.
struct RegionFlag { const char* code; ImU32 a; ImU32 b; };
constexpr RegionFlag kFlags[] = {
    { "JP", IM_COL32(0xff,0xff,0xff,0xff), IM_COL32(0xff,0x30,0x30,0xff) },
    { "US", IM_COL32(0x00,0x40,0xa0,0xff), IM_COL32(0xff,0x30,0x30,0xff) },
    { "FR", IM_COL32(0x00,0x40,0xa0,0xff), IM_COL32(0xff,0x30,0x30,0xff) },
    { "DE", IM_COL32(0x00,0x00,0x00,0xff), IM_COL32(0xff,0xb9,0x38,0xff) },
    { "KR", IM_COL32(0xff,0xff,0xff,0xff), IM_COL32(0x00,0x40,0xa0,0xff) },
    { "BR", IM_COL32(0x0d,0x8a,0x3a,0xff), IM_COL32(0xff,0xd0,0x00,0xff) },
    { "MX", IM_COL32(0x0d,0x8a,0x3a,0xff), IM_COL32(0xff,0x30,0x30,0xff) },
    { "ES", IM_COL32(0xff,0xd0,0x00,0xff), IM_COL32(0xff,0x30,0x30,0xff) },
    { "AR", IM_COL32(0x7e,0xc0,0xee,0xff), IM_COL32(0xff,0xff,0xff,0xff) },
};

// Lookup region colors by code. Falls back to neutral grays.
void LookupFlag(const char* code, ImU32* a, ImU32* b) {
    for (const auto& f : kFlags) {
        if (code[0] == f.code[0] && code[1] == f.code[1]) {
            *a = f.a; *b = f.b; return;
        }
    }
    *a = IM_COL32(0x3d,0x3d,0x3d,0xff);
    *b = IM_COL32(0x88,0x88,0x88,0xff);
}

// Color mix helper (linear, alpha-naïve — fine for chrome accents).
ImU32 MixCol(ImU32 ca, ImU32 cb, float t) {
    ImVec4 a = ImGui::ColorConvertU32ToFloat4(ca);
    ImVec4 b = ImGui::ColorConvertU32ToFloat4(cb);
    ImVec4 r(a.x + (b.x - a.x) * t,
             a.y + (b.y - a.y) * t,
             a.z + (b.z - a.z) * t,
             a.w + (b.w - a.w) * t);
    return ImGui::ColorConvertFloat4ToU32(r);
}

// ─── design font handles ──────────────────────────────────────────────────
// Populated by LoadDesignFonts(); stay valid for program lifetime once
// the atlas is built. Any of these may be nullptr if the corresponding
// TTF wasn't found on disk — push helpers below null-guard.
ImFont* g_font_body     = nullptr;  // VT323 — boot log lines, body
ImFont* g_font_label    = nullptr;  // Silkscreen — labels, k/v telemetry
ImFont* g_font_display  = nullptr;  // DotGothic16 — big "READY." numerals
ImFont* g_font_micro    = nullptr;  // Press Start 2P — eyebrow microtype

void PushFontIf(ImFont* f) { if (f) ImGui::PushFont(f); }
void PopFontIf (ImFont* f) { if (f) ImGui::PopFont();   }

// ─── design palette (NERV) — pulled from app.jsx PALETTES.nerv ────────────
// Hex lifted verbatim. Kept const so we can A/B against ICE/AMBER later
// just by pointing at a different palette table.
struct NervPalette {
    ImU32 bg, bg2, ink, ink2, dim, faint, line, hr, acc, phos, amber, red;
};
constexpr NervPalette kNerv = {
    IM_COL32(0x0a, 0x0a, 0x0a, 0xff),  // bg
    IM_COL32(0x12, 0x12, 0x12, 0xff),  // bg2
    IM_COL32(0xff, 0xf8, 0xe7, 0xff),  // ink
    IM_COL32(0xcf, 0xc8, 0xb8, 0xff),  // ink2
    IM_COL32(0x7a, 0x74, 0x68, 0xff),  // dim
    IM_COL32(0x4a, 0x46, 0x40, 0xff),  // faint
    IM_COL32(0x2a, 0x2a, 0x2a, 0xff),  // line
    IM_COL32(0xff, 0xff, 0xff, 0x10),  // hr (alpha-faint white)
    IM_COL32(0xff, 0x6a, 0x00, 0xff),  // acc (orange)
    IM_COL32(0x00, 0xd9, 0x6b, 0xff),  // phos (green OK)
    IM_COL32(0xff, 0xb0, 0x00, 0xff),  // amber
    IM_COL32(0xff, 0x30, 0x30, 0xff),  // red
};

// 32-color VGA swatch table, also from launcher.jsx:29.
constexpr std::array<ImU32, 32> kVgaPalette = {
    IM_COL32(0x0a,0x06,0x12,0xff), IM_COL32(0x11,0x08,0x20,0xff),
    IM_COL32(0x1d,0x0d,0x2e,0xff), IM_COL32(0x2a,0x12,0x40,0xff),
    IM_COL32(0x3d,0x1c,0x5e,0xff), IM_COL32(0x5a,0x2a,0x85,0xff),
    IM_COL32(0x88,0x42,0xc4,0xff), IM_COL32(0xc0,0x68,0xf0,0xff),
    IM_COL32(0x00,0x18,0x30,0xff), IM_COL32(0x00,0x28,0x50,0xff),
    IM_COL32(0x00,0x40,0x7a,0xff), IM_COL32(0x00,0x64,0xb8,0xff),
    IM_COL32(0x00,0x90,0xff,0xff), IM_COL32(0x4a,0xda,0xcc,0xff),
    IM_COL32(0x80,0xff,0xe8,0xff), IM_COL32(0xd8,0xff,0xff,0xff),
    IM_COL32(0x20,0x08,0x00,0xff), IM_COL32(0x50,0x18,0x00,0xff),
    IM_COL32(0x90,0x30,0x00,0xff), IM_COL32(0xd0,0x48,0x00,0xff),
    IM_COL32(0xff,0x5a,0x3c,0xff), IM_COL32(0xff,0x8a,0x3c,0xff),
    IM_COL32(0xff,0xb9,0x38,0xff), IM_COL32(0xff,0xe0,0x80,0xff),
    IM_COL32(0xff,0xf8,0xe7,0xff), IM_COL32(0x00,0x10,0x08,0xff),
    IM_COL32(0x00,0x38,0x20,0xff), IM_COL32(0x00,0x68,0x3a,0xff),
    IM_COL32(0x00,0xa0,0x50,0xff), IM_COL32(0x20,0xd8,0x70,0xff),
    IM_COL32(0x9b,0xff,0x5a,0xff), IM_COL32(0xf0,0xa8,0xff,0xff),
};

// Cumulative ms thresholds for the seven boot stages.
constexpr std::array<float, 7> kStageMs = {
    400.0f, 1000.0f, 1700.0f, 2300.0f, 3100.0f, 3800.0f, 5000.0f
};

int CurrentStage(float elapsed_ms) {
    int s = 0;
    for (size_t i = 0; i < kStageMs.size(); ++i) {
        if (elapsed_ms >= kStageMs[i]) s = (int)i + 1;
    }
    return s;
}

// Per-line fade-in alpha tween — id is the line's hashed name so the
// ImAnim pool keys are stable across frames.
float LineAlpha(const char* line_id, bool reached, float dt) {
    iam_ease_desc ez{ iam_ease_out_cubic, 0, 0, 0, 0 };
    return iam_tween_float(
        ImHashStr(line_id),
        ImHashStr("alpha"),
        reached ? 1.0f : 0.0f,
        0.32f,
        ez,
        iam_policy_crossfade,
        dt,
        0.0f);
}

// ── small drawing helpers ─────────────────────────────────────────────
void TextWithAlpha(ImU32 col, float alpha, const char* fmt, ...) {
    if (alpha <= 0.001f) return;
    ImVec4 c = ImGui::ColorConvertU32ToFloat4(col);
    c.w *= alpha;
    ImGui::PushStyleColor(ImGuiCol_Text, c);
    va_list args;
    va_start(args, fmt);
    ImGui::TextV(fmt, args);
    va_end(args);
    ImGui::PopStyleColor();
}

void HRule(ImU32 col, float thickness = 1.0f) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    dl->AddLine(ImVec2(p.x, p.y + 4), ImVec2(p.x + w, p.y + 4), col, thickness);
    ImGui::Dummy(ImVec2(w, 9));
}

// NvHeader: thin top strip with id · title · sub on the left, telemetry
// k/v pairs on the right. Pixel fonts via DrawList::AddText use whatever
// font is currently pushed on the ImGui font stack, so we PushFont
// around the chunks that need it and rely on AddText reading the
// active font from ImGui::GetFont().
void DrawHeader(int stage) {
    const float h = 64.0f;
    ImVec2 origin = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Left side — id (Press Start 2P micro), title (VT323 large), sub
    // (Silkscreen). Push the font we want, AddText, pop. AddText with
    // explicit font pointer keeps the per-section size correct.
    if (g_font_micro) {
        dl->AddText(g_font_micro, g_font_micro->LegacySize,
                    ImVec2(origin.x + 12, origin.y + 10),
                    kNerv.dim, "00.LOAD");
    }
    if (g_font_body) {
        dl->AddText(g_font_body, 19.0f,
                    ImVec2(origin.x + 12, origin.y + 22),
                    kNerv.ink, "Cold Boot");
    }
    if (g_font_label) {
        dl->AddText(g_font_label, g_font_label->LegacySize,
                    ImVec2(origin.x + 12, origin.y + 46),
                    kNerv.ink2, "FM2K_ROLLBACK \xe2\x80\xa2 v0.2.8 \xe2\x80\xa2 32-BIT");
    }

    // Right-aligned telemetry, k=Silkscreen-dim / v=Silkscreen-acc-or-ink.
    char stage_buf[16], mem_buf[24], ts_buf[16];
    std::snprintf(stage_buf, sizeof(stage_buf), "%d/7", stage);
    std::snprintf(mem_buf,   sizeof(mem_buf),   "%dM / 256M", 24 + stage * 8);
    std::snprintf(ts_buf,    sizeof(ts_buf),    "00:00:0%d", stage);
    struct Row { const char* k; const char* v; bool acc; } rows[] = {
        { "STAGE", stage_buf, true },
        { "MEM",   mem_buf,   false },
        { "TS",    ts_buf,    false },
    };
    if (g_font_label) {
        ImFont* f  = g_font_label;
        float   fs = f->LegacySize;
        float   y  = origin.y + 12;
        for (const auto& r : rows) {
            ImU32 vcol = r.acc ? kNerv.acc : kNerv.ink;
            ImVec2 ksz = f->CalcTextSizeA(fs, FLT_MAX, 0.0f, r.k);
            ImVec2 vsz = f->CalcTextSizeA(fs, FLT_MAX, 0.0f, r.v);
            float right_x = origin.x + w - 12.0f;
            dl->AddText(f, fs, ImVec2(right_x - vsz.x,            y), vcol,      r.v);
            dl->AddText(f, fs, ImVec2(right_x - vsz.x - 6 - ksz.x, y), kNerv.dim, r.k);
            y += fs + 4.0f;
        }
    }

    // Underline
    dl->AddLine(ImVec2(origin.x, origin.y + h - 1),
                ImVec2(origin.x + w, origin.y + h - 1),
                kNerv.line, 1.0f);

    ImGui::Dummy(ImVec2(w, h));
}

// ── the actual cold-boot panel ────────────────────────────────────────
void RenderColdBoot() {
    if (!g_cold_boot.open) return;

    ImGui::SetNextWindowSize(ImVec2(640.0f, 448.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(640.0f, 448.0f),
                                        ImVec2(640.0f, FLT_MAX));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, kNerv.bg);
    ImGui::PushStyleColor(ImGuiCol_TitleBg, kNerv.bg2);
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive, kNerv.bg2);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));

    bool show = ImGui::Begin("Cold Boot \xe2\x80\xa2 Sandbox",
                             &g_cold_boot.open,
                             ImGuiWindowFlags_NoCollapse);
    ImGui::PopStyleVar();

    if (!show) {
        ImGui::End();
        ImGui::PopStyleColor(3);
        return;
    }

    // (Re)start anim when window first opens or user clicks reset.
    if (!g_cold_boot.armed) {
        g_cold_boot.armed = true;
        g_cold_boot.anim_start_s = ImGui::GetTime();
    }

    const float dt        = ImGui::GetIO().DeltaTime;
    const float elapsedMs = (float)((ImGui::GetTime() - g_cold_boot.anim_start_s) * 1000.0);
    const int   stage     = CurrentStage(elapsedMs);

    DrawHeader(stage);

    ImGui::Dummy(ImVec2(0, 6));
    ImGui::Indent(22.0f);

    // Body text uses VT323 — the cold-boot crawl is the entire reason
    // we vendored a CRT mono pixel font. Push for the body block,
    // pop before READY/footer (which want different fonts).
    PushFontIf(g_font_body);

    // Stage 1 — boot loader banner
    {
        float a = LineAlpha("cb.line.boot", stage >= 1, dt);
        if (a > 0.001f) {
            TextWithAlpha(kNerv.faint, a, "[boot]");
            ImGui::SameLine(0, 6);
            TextWithAlpha(kNerv.ink,   a, "FM2K_ROLLBACK loader v0.2.8");
            ImGui::SameLine(0, 6);
            TextWithAlpha(kNerv.dim,   a, "\xe2\x80\x94 (c) 2dfm collective");
        }
    }
    // Stage 2 — verify
    {
        float a = LineAlpha("cb.line.verify", stage >= 2, dt);
        if (a > 0.001f) {
            TextWithAlpha(kNerv.faint, a, "[verify]");
            ImGui::SameLine(0, 6);
            TextWithAlpha(kNerv.ink, a, "boot.signature");
            ImGui::SameLine(0, 6);
            TextWithAlpha(kNerv.phos, a, "OK");
            ImGui::SameLine(0, 6);
            TextWithAlpha(kNerv.dim, a, "0xDEADBEEF");
        }
    }
    // Stage 3+4 — palette load with per-swatch staggered fade
    {
        float ahdr = LineAlpha("cb.line.palette", stage >= 3, dt);
        if (ahdr > 0.001f) {
            TextWithAlpha(kNerv.faint, ahdr, "[palette]");
            ImGui::SameLine(0, 6);
            TextWithAlpha(kNerv.acc, ahdr, "loading 32-color VGA table\xe2\x80\xa6");

            // Swatch row — each cell tweens its alpha with a 30ms stagger.
            ImGui::Dummy(ImVec2(0, 2));
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImVec2 p = ImGui::GetCursorScreenPos();
            const float cw = 14.0f, ch = 10.0f, gap = 1.0f;
            for (size_t i = 0; i < kVgaPalette.size(); ++i) {
                // Per-cell stage trigger: cell i visible once we're past
                // stage 3 AND a 30ms-per-cell stagger has elapsed.
                const float cellMs = kStageMs[2] + (float)i * 30.0f;
                bool reached = elapsedMs >= cellMs;
                char id[32];
                std::snprintf(id, sizeof(id), "cb.swatch.%02zu", i);
                float ac = iam_tween_float(
                    ImHashStr(id), ImHashStr("a"),
                    reached ? 1.0f : 0.0f,
                    0.06f,
                    iam_ease_desc{iam_ease_out_quad, 0,0,0,0},
                    iam_policy_crossfade, dt, 0.0f);
                ImVec4 col = ImGui::ColorConvertU32ToFloat4(kVgaPalette[i]);
                col.w *= ac;
                ImU32 c32 = ImGui::ColorConvertFloat4ToU32(col);
                float x = p.x + (cw + gap) * (float)i;
                dl->AddRectFilled(ImVec2(x, p.y),
                                  ImVec2(x + cw, p.y + ch),
                                  c32);
            }
            ImGui::Dummy(ImVec2(0, ch + 4));
        }
    }
    // Stage 4 — fonts
    {
        float a = LineAlpha("cb.line.fonts", stage >= 4, dt);
        if (a > 0.001f) {
            TextWithAlpha(kNerv.faint, a, "[fonts]");
            ImGui::SameLine(0, 6);
            TextWithAlpha(kNerv.ink, a, "dm-serif.woff2 \xe2\x80\xa2 jetbrains-mono.woff2 \xe2\x80\xa2 inter.woff2");
            ImGui::SameLine(0, 6);
            TextWithAlpha(kNerv.phos, a, "OK");
        }
    }
    // Stage 5 — scan
    {
        float a = LineAlpha("cb.line.scan", stage >= 5, dt);
        if (a > 0.001f) {
            TextWithAlpha(kNerv.faint, a, "[scan]");
            ImGui::SameLine(0, 6);
            TextWithAlpha(kNerv.ink, a, "./games/*");
            ImGui::SameLine(0, 6);
            TextWithAlpha(kNerv.acc, a, "\xe2\x86\x92 5 titles");

            const char* games[] = {
                "akatsuki_blitzkampf.exe",
                "glove_on_fight.exe",
                "master_of_fighter_2k.exe",
                "rumble_fighter.exe",
                "wonderful_world.exe",
            };
            ImGui::Indent(36.0f);
            for (const char* g : games) {
                TextWithAlpha(kNerv.dim, a, "+ %s", g);
            }
            ImGui::Unindent(36.0f);
        }
    }
    // Stage 6 — net
    {
        float a = LineAlpha("cb.line.net", stage >= 6, dt);
        if (a > 0.001f) {
            TextWithAlpha(kNerv.faint, a, "[net]");
            ImGui::SameLine(0, 6);
            TextWithAlpha(kNerv.ink, a, "stun:relay-tokyo:3478 \xe2\x80\xa2");
            ImGui::SameLine(0, 6);
            TextWithAlpha(kNerv.phos, a, "14ms");
        }
    }
    // Body block ends — pop VT323 before swapping in the display font.
    PopFontIf(g_font_body);

    // Stage 7 — READY display (DotGothic16) + eyebrow (Press Start 2P)
    {
        float a = LineAlpha("cb.line.ready", stage >= 7, dt);
        if (a > 0.001f) {
            ImGui::Dummy(ImVec2(0, 4));
            HRule(kNerv.hr);

            PushFontIf(g_font_display);
            TextWithAlpha(kNerv.ink, a, "READY");
            ImGui::SameLine(0, 0);
            TextWithAlpha(kNerv.acc, a, ".");
            PopFontIf(g_font_display);

            ImGui::Dummy(ImVec2(0, 8));
            PushFontIf(g_font_micro);
            TextWithAlpha(kNerv.ink2, a, "PRESS [\xe2\x86\xb5] TO ENTER SYSTEM");
            PopFontIf(g_font_micro);
        }
    }

    ImGui::Unindent(22.0f);

    // Replay / status footer — pinned, easy to find for testing.
    ImGui::Dummy(ImVec2(0, 8));
    ImGui::Separator();
    ImGui::TextDisabled("stage %d/7 \xe2\x80\xa2 t=%.0fms", stage, elapsedMs);
    ImGui::SameLine();
    if (ImGui::SmallButton("Replay")) {
        g_cold_boot.anim_start_s = ImGui::GetTime();
    }

    ImGui::End();
    ImGui::PopStyleColor(3);
}

// ─── STYLE GUIDE PANEL ───────────────────────────────────────────────────
// Three sections that lock down the design tokens we're porting:
//   01 TYPE RAMP   — each pixel font at multiple sizes, side by side
//   02 COLOR TOKENS — NERV palette swatches with hex + token name
//   03 EASING LAB   — every ImAnim easing animated in parallel for
//                     comparison (this is the user-asked "test easing")
//
// Skipping the design's "chrome library" and "screen-state matrix"
// sections for now — secondary for getting tokens locked.

struct EaseEntry {
    const char*       name;
    int               type;     // iam_ease_type
    float             p0, p1, p2, p3;
};

constexpr EaseEntry kEasings[] = {
    { "linear",          iam_ease_linear,           0,0,0,0 },
    { "in_quad",         iam_ease_in_quad,          0,0,0,0 },
    { "out_quad",        iam_ease_out_quad,         0,0,0,0 },
    { "in_out_quad",     iam_ease_in_out_quad,      0,0,0,0 },
    { "in_cubic",        iam_ease_in_cubic,         0,0,0,0 },
    { "out_cubic",       iam_ease_out_cubic,        0,0,0,0 },
    { "in_out_cubic",    iam_ease_in_out_cubic,     0,0,0,0 },
    { "out_quart",       iam_ease_out_quart,        0,0,0,0 },
    { "out_quint",       iam_ease_out_quint,        0,0,0,0 },
    { "out_sine",        iam_ease_out_sine,         0,0,0,0 },
    { "out_expo",        iam_ease_out_expo,         0,0,0,0 },
    { "out_circ",        iam_ease_out_circ,         0,0,0,0 },
    { "out_back",        iam_ease_out_back,         1.7f,0,0,0 },   // p0 = overshoot
    { "out_elastic",     iam_ease_out_elastic,      1.0f, 0.3f, 0,0 },// p0=amp p1=period
    { "out_bounce",      iam_ease_out_bounce,       0,0,0,0 },
    { "steps(8)",        iam_ease_steps,            8.0f, 0,0,0 },
    { "bezier·overshoot",iam_ease_cubic_bezier,     0.68f, -0.55f, 0.27f, 1.55f },
    { "spring",          iam_ease_spring,           1.0f, 100.0f, 10.0f, 0 }, // mass/stiff/damp
};

void DrawSwatchCard(ImU32 col, const char* token, const char* hex, const char* label) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    const float W = 96.0f, H = 64.0f;
    // Color block with subtle scanline overlay
    dl->AddRectFilled(p, ImVec2(p.x + W, p.y + 36), col);
    for (int y = 1; y < 36; y += 3) {
        dl->AddLine(ImVec2(p.x, p.y + y),
                    ImVec2(p.x + W, p.y + y),
                    IM_COL32(0, 0, 0, 46));
    }
    // 1-px border in faint line color
    dl->AddRect(p, ImVec2(p.x + W, p.y + H), kNerv.line);
    // Label area below
    if (g_font_micro) {
        dl->AddText(g_font_micro, g_font_micro->LegacySize,
                    ImVec2(p.x + 6, p.y + 39),
                    kNerv.acc, label);
    }
    if (g_font_label) {
        dl->AddText(g_font_label, g_font_label->LegacySize,
                    ImVec2(p.x + 6, p.y + 49),
                    kNerv.ink, token);
    }
    if (g_font_label) {
        dl->AddText(g_font_label, 9.0f,
                    ImVec2(p.x + 6, p.y + 56),
                    kNerv.faint, hex);
    }
    ImGui::Dummy(ImVec2(W, H));
}

void RenderStyleGuide() {
    if (!g_style.open) return;

    ImGui::SetNextWindowSize(ImVec2(820.0f, 640.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(640.0f, 320.0f),
                                        ImVec2(FLT_MAX, FLT_MAX));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, kNerv.bg);
    ImGui::PushStyleColor(ImGuiCol_TitleBg, kNerv.bg2);
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive, kNerv.bg2);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, kNerv.bg2);

    bool show = ImGui::Begin("Style Guide \xe2\x80\xa2 NERV \xe2\x80\xa2 Sandbox",
                             &g_style.open,
                             ImGuiWindowFlags_NoCollapse);
    if (!show) {
        ImGui::End();
        ImGui::PopStyleColor(4);
        return;
    }

    if (!g_style.armed) {
        g_style.armed = true;
        g_style.phase_start = ImGui::GetTime();
    }

    const float dt = ImGui::GetIO().DeltaTime;

    // Title bar
    PushFontIf(g_font_label);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(kNerv.acc));
    ImGui::TextUnformatted("\xc2\xa7 NERV \xe2\x80\xa2 DESIGN SYSTEM \xe2\x80\xa2 v0.1");
    ImGui::PopStyleColor();
    PopFontIf(g_font_label);

    PushFontIf(g_font_body);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(kNerv.dim));
    ImGui::TextUnformatted("// foundations \xe2\x80\xa2 chrome \xe2\x80\xa2 motion \xe2\x80\xa2 made for FM2K rollback");
    ImGui::PopStyleColor();
    PopFontIf(g_font_body);

    ImGui::Dummy(ImVec2(0, 8));

    // ─── 01 TYPE RAMP ────────────────────────────────────────────────
    if (ImGui::CollapsingHeader("\xc2\xa7 01  TYPE RAMP",
                                ImGuiTreeNodeFlags_DefaultOpen)) {
        struct Ramp { const char* label; ImFont* font; float size; const char* sample; };
        Ramp ramps[] = {
            { "DISPLAY \xe2\x80\xa2 DotGothic16 \xe2\x80\xa2 28px",  g_font_display, 28.0f, "READY." },
            { "DISPLAY \xe2\x80\xa2 DotGothic16 \xe2\x80\xa2 20px",  g_font_display, 20.0f, "PATTERN: ORANGE" },
            { "BODY    \xe2\x80\xa2 VT323       \xe2\x80\xa2 17px",  g_font_body,    17.0f, "[boot] FM2K_ROLLBACK loader v0.2.8" },
            { "BODY    \xe2\x80\xa2 VT323       \xe2\x80\xa2 14px",  g_font_body,    14.0f, "stun:relay-tokyo:3478 \xe2\x80\xa2 14ms" },
            { "LABEL   \xe2\x80\xa2 Silkscreen  \xe2\x80\xa2 12px",  g_font_label,   12.0f, "STAGE  3/7   MEM  48M / 256M" },
            { "MICRO   \xe2\x80\xa2 PressStart2P \xe2\x80\xa2 9px",  g_font_micro,    9.0f, "PRESS [\xe2\x86\xb5] TO ENTER SYSTEM" },
        };
        ImDrawList* dl = ImGui::GetWindowDrawList();
        for (const auto& r : ramps) {
            ImVec2 p = ImGui::GetCursorScreenPos();
            // Tag (Silkscreen, faint)
            if (g_font_label) {
                dl->AddText(g_font_label, g_font_label->LegacySize,
                            ImVec2(p.x, p.y + 6),
                            kNerv.faint, r.label);
            }
            // Sample
            if (r.font) {
                dl->AddText(r.font, r.size,
                            ImVec2(p.x + 280, p.y),
                            kNerv.ink, r.sample);
            } else {
                dl->AddText(ImVec2(p.x + 280, p.y), kNerv.dim,
                            "[font missing]");
            }
            ImGui::Dummy(ImVec2(0, r.size + 10.0f));
        }
        ImGui::Dummy(ImVec2(0, 4));
    }

    // ─── 02 COLOR TOKENS ─────────────────────────────────────────────
    if (ImGui::CollapsingHeader("\xc2\xa7 02  COLOR TOKENS",
                                ImGuiTreeNodeFlags_DefaultOpen)) {
        struct Swatch { ImU32 col; const char* token; const char* hex; const char* label; };
        Swatch swatches[] = {
            { kNerv.bg,    "--nv-bg",    "#0a0a0a", "BASE"  },
            { kNerv.bg2,   "--nv-bg2",   "#121212", "PLATE" },
            { kNerv.line,  "--nv-line",  "#2a2a2a", "RULE"  },
            { kNerv.faint, "--nv-faint", "#4a4640", "FAINT" },
            { kNerv.dim,   "--nv-dim",   "#7a7468", "DIM"   },
            { kNerv.ink2,  "--nv-ink2",  "#cfc8b8", "INK-2" },
            { kNerv.ink,   "--nv-ink",   "#fff8e7", "INK"   },
            { kNerv.acc,   "--nv-acc",   "#ff6a00", "PRIME" },
            { kNerv.amber, "--nv-amber", "#ffb000", "ALERT" },
            { kNerv.phos,  "--nv-phos",  "#00d96b", "SYNC"  },
            { kNerv.red,   "--nv-red",   "#ff3030", "ERR"   },
        };
        const int per_row = 7;
        for (size_t i = 0; i < IM_ARRAYSIZE(swatches); ++i) {
            if (i > 0 && (i % per_row) != 0) ImGui::SameLine(0, 8);
            const auto& s = swatches[i];
            DrawSwatchCard(s.col, s.token, s.hex, s.label);
        }
        ImGui::Dummy(ImVec2(0, 4));
    }

    // ─── 03 EASING LAB ───────────────────────────────────────────────
    if (ImGui::CollapsingHeader("\xc2\xa7 03  EASING LAB \xe2\x80\xa2 ImAnim",
                                ImGuiTreeNodeFlags_DefaultOpen)) {
        // Loop control: tween rides 0→1, then 1→0, repeating. Each
        // full loop is loop_dur*2 seconds. The "phase" is which half
        // we're in; flipping the target on each half drives the tween.
        const double now = ImGui::GetTime();
        const double cycle = (double)g_style.loop_dur * 2.0;
        double t_in_cycle = std::fmod(now - g_style.phase_start, cycle);
        if (t_in_cycle < 0) t_in_cycle += cycle;
        const bool forward = t_in_cycle < (double)g_style.loop_dur;
        const float target = forward ? 1.0f : 0.0f;

        // Controls
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(kNerv.dim));
        ImGui::Text("phase: %s  t=%.2fs / %.1fs",
                    forward ? "FWD" : "REV",
                    forward ? (float)t_in_cycle : (float)(t_in_cycle - g_style.loop_dur),
                    g_style.loop_dur);
        ImGui::PopStyleColor();
        ImGui::SameLine();
        if (ImGui::SmallButton("Restart")) {
            g_style.phase_start = now;
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120);
        ImGui::SliderFloat("loop", &g_style.loop_dur, 0.4f, 4.0f, "%.2fs");

        ImGui::Dummy(ImVec2(0, 6));

        // Layout per row: [ name (140px) ][ track (rest) ]
        const float track_h     = 16.0f;
        const float marker_w    = 12.0f;
        const float track_inset = 150.0f;
        const float row_h       = track_h + 6.0f;

        ImDrawList* dl = ImGui::GetWindowDrawList();

        for (size_t i = 0; i < IM_ARRAYSIZE(kEasings); ++i) {
            const EaseEntry& e = kEasings[i];

            ImVec2 row_origin = ImGui::GetCursorScreenPos();
            float  avail_w    = ImGui::GetContentRegionAvail().x;
            float  track_w    = avail_w - track_inset - 8.0f;
            if (track_w < 80.0f) track_w = 80.0f;

            // Name in Silkscreen
            if (g_font_label) {
                dl->AddText(g_font_label, g_font_label->LegacySize,
                            ImVec2(row_origin.x, row_origin.y + 2),
                            kNerv.ink2, e.name);
            }

            // Track background
            ImVec2 t0 = ImVec2(row_origin.x + track_inset, row_origin.y);
            ImVec2 t1 = ImVec2(t0.x + track_w, t0.y + track_h);
            dl->AddRectFilled(t0, t1, kNerv.bg2);
            dl->AddRect(t0, t1, kNerv.line);

            // Tween: target alternates 0/1; channel id is the easing
            // index so each row has its own pool entry.
            iam_ease_desc ez{ e.type, e.p0, e.p1, e.p2, e.p3 };
            char id_buf[32];
            std::snprintf(id_buf, sizeof(id_buf), "sg.ease.%zu", i);
            float v = iam_tween_float(
                ImHashStr(id_buf),
                ImHashStr("x"),
                target,
                g_style.loop_dur,
                ez,
                iam_policy_crossfade,
                dt,
                0.0f);

            // Marker. Clamp the rendered position so back/elastic/bounce
            // overshoot/undershoot stay visible without spilling out.
            float clamped = v;
            if (clamped < -0.15f) clamped = -0.15f;
            if (clamped >  1.15f) clamped =  1.15f;
            float mx = t0.x + clamped * (track_w - marker_w);
            ImU32 mcol = (i % 3 == 0) ? kNerv.acc :
                         (i % 3 == 1) ? kNerv.phos : kNerv.amber;
            dl->AddRectFilled(ImVec2(mx, t0.y + 2),
                              ImVec2(mx + marker_w, t1.y - 2),
                              mcol);

            // Numeric readout
            char num[16];
            std::snprintf(num, sizeof(num), "%.2f", v);
            if (g_font_label) {
                dl->AddText(g_font_label, g_font_label->LegacySize,
                            ImVec2(t1.x + 6, row_origin.y + 2),
                            kNerv.dim, num);
            }

            ImGui::Dummy(ImVec2(0, row_h));
        }

        ImGui::Dummy(ImVec2(0, 4));

        // Bonus: ImAnim oscillator + shake demo so all the primitives
        // get smoke-tested in one place.
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(kNerv.faint));
        ImGui::TextUnformatted("// oscillators \xe2\x80\xa2 shake");
        ImGui::PopStyleColor();

        const char* osc_names[] = { "sine", "triangle", "sawtooth", "square" };
        const int   osc_types[] = { iam_wave_sine, iam_wave_triangle,
                                    iam_wave_sawtooth, iam_wave_square };
        for (int i = 0; i < 4; ++i) {
            ImVec2 p = ImGui::GetCursorScreenPos();
            float  avail_w = ImGui::GetContentRegionAvail().x;
            float  track_w = avail_w - track_inset - 8.0f;
            if (track_w < 80.0f) track_w = 80.0f;

            if (g_font_label) {
                dl->AddText(g_font_label, g_font_label->LegacySize,
                            ImVec2(p.x, p.y + 2),
                            kNerv.ink2, osc_names[i]);
            }
            ImVec2 t0 = ImVec2(p.x + track_inset, p.y);
            ImVec2 t1 = ImVec2(t0.x + track_w, t0.y + track_h);
            dl->AddRectFilled(t0, t1, kNerv.bg2);
            dl->AddRect(t0, t1, kNerv.line);

            char id_buf[32];
            std::snprintf(id_buf, sizeof(id_buf), "sg.osc.%d", i);
            float v = iam_oscillate(ImHashStr(id_buf),
                                    1.0f,         // amplitude (-1..+1)
                                    0.6f,         // frequency Hz
                                    osc_types[i],
                                    0.0f, dt);
            float pos01 = (v + 1.0f) * 0.5f;
            float mx = t0.x + pos01 * (track_w - marker_w);
            dl->AddRectFilled(ImVec2(mx, t0.y + 2),
                              ImVec2(mx + marker_w, t1.y - 2),
                              kNerv.phos);
            ImGui::Dummy(ImVec2(0, row_h));
        }

        // Shake row — big "SHAKE" word that vibrates when the target
        // alternates. Triggers on each phase flip.
        {
            static bool s_was_forward = forward;
            ImGuiID shake_id = ImHashStr("sg.shake.row");
            if (forward != s_was_forward) {
                iam_trigger_shake(shake_id);
                s_was_forward = forward;
            }
            ImVec2 p = ImGui::GetCursorScreenPos();
            float shake_x = iam_shake(shake_id, 6.0f, 14.0f, 0.4f, dt);
            if (g_font_label) {
                dl->AddText(g_font_label, g_font_label->LegacySize,
                            ImVec2(p.x, p.y + 2),
                            kNerv.ink2, "shake");
            }
            if (g_font_display) {
                dl->AddText(g_font_display, 22.0f,
                            ImVec2(p.x + track_inset + shake_x, p.y - 4),
                            kNerv.red, "PATTERN LOST.");
            }
            ImGui::Dummy(ImVec2(0, 28));
        }
    }

    ImGui::End();
    ImGui::PopStyleColor(4);
}

// ─── APPSHELL ─ END-TO-END NAVIGABLE ─────────────────────────────────────
// Layout (640×448, before any user resize):
//   ┌──┬──────────────────────────────────────┬──────────────┐ 30 px topbar
//   │  ├──────────────────────────────────────┼──────────────┤
//   │  │                                      │              │
//   │R │              MAIN VIEW               │  RIGHT RAIL  │
//   │  │                                      │              │
//   │  ├──────────────────────────────────────┴──────────────┤ 18 px status
//   └──┴────────────────────────────────────────────────────-┘
//   48px        right rail = 154px
//
// We compute child rect origins from the window's content min, then draw
// each region with a child + ImDrawList combo. Interactive bits use
// InvisibleButton + manual hover styling so we can match the design's
// flat, minimal selection states.

constexpr float kAS_W         = 640.0f;
constexpr float kAS_H         = 448.0f;
constexpr float kAS_RailW     = 48.0f;
constexpr float kAS_RightW    = 154.0f;
constexpr float kAS_TopH      = 30.0f;
constexpr float kAS_StatusH   = 18.0f;

const char* AppViewKey(AppView v) {
    switch (v) {
        case kView_Lobby:    return "LOBBY";
        case kView_Browse:   return "BROWSE";
        case kView_Replays:  return "REPLAYS";
        case kView_Rankings: return "RANK";
        case kView_Stats:    return "STATS";
        case kView_Events:   return "EVTS";
        case kView_Profile:  return "PROFILE";
        case kView_Cfg:      return "CONFIG";
        case kView_Map:      return "CTRL MAP";
        default:             return "?";
    }
}

void DrawTextF(ImDrawList* dl, ImFont* f, float size, ImVec2 pos, ImU32 col, const char* s) {
    if (f) dl->AddText(f, size, pos, col, s);
    else   dl->AddText(pos, col, s);
}

// 12×8 region flag chip — two horizontal stripes.
void DrawFlagDot(ImDrawList* dl, ImVec2 p, const char* region) {
    ImU32 a, b; LookupFlag(region, &a, &b);
    dl->AddRectFilled(ImVec2(p.x, p.y),       ImVec2(p.x + 12, p.y + 4), a);
    dl->AddRectFilled(ImVec2(p.x, p.y + 4),   ImVec2(p.x + 12, p.y + 8), b);
    dl->AddRect(ImVec2(p.x - 0.5f, p.y - 0.5f),
                ImVec2(p.x + 12, p.y + 8),
                IM_COL32(0,0,0,160));
}

// Tiny ping wave canvas — colored sine that gets jittery at high ping.
// Driven by ImGui::GetTime() so it animates without us tracking dt per
// row. Color: green<30, amber<80, red otherwise.
void DrawPingWave(ImDrawList* dl, ImVec2 p, int ping, float w = 28.0f, float h = 8.0f) {
    float t = (float)ImGui::GetTime();
    float amp  = std::min(h * 0.4f, 0.6f + ping * 0.04f);
    float freq = 0.6f + std::min(0.8f, ping * 0.02f);
    float jit  = std::min(0.6f, ping * 0.012f);
    ImU32 col  = (ping < 30) ? IM_COL32(0x5f,0xff,0x95,0xff)
               : (ping < 80) ? IM_COL32(0xff,0xb9,0x38,0xff)
               :                IM_COL32(0xff,0x5a,0x3c,0xff);
    ImVec2 prev(p.x, p.y + h * 0.5f);
    for (int x = 1; x <= (int)w; ++x) {
        float phase = t * freq * 4.0f + x * 0.5f;
        float j = std::sin(phase * 7.0f + (float)ping) * jit;
        float y = h * 0.5f + std::sin(phase) * amp + j;
        ImVec2 cur(p.x + (float)x, p.y + y);
        dl->AddLine(prev, cur, col, 1.0f);
        prev = cur;
    }
}

// ── LEFT RAIL ────────────────────────────────────────────────────────
void DrawLeftRail(ImVec2 origin) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 br(origin.x + kAS_RailW, origin.y + kAS_H);

    // Background + right border
    dl->AddRectFilled(origin, br, IM_COL32(8, 4, 14, 153));
    dl->AddLine(ImVec2(br.x, origin.y), br, kNerv.line, 1.0f);

    // Logo dot — pulsing ring + "F" centered
    {
        ImVec2 c(origin.x + kAS_RailW * 0.5f, origin.y + 22);
        float t = (float)ImGui::GetTime();
        float pulse = 0.5f + 0.5f * std::sin(t * (IM_PI * 2.0f / 2.4f));
        ImU32 ring_col = MixCol(kNerv.acc, kNerv.bg2, pulse);
        dl->AddCircle(c, 14.0f, ring_col, 24, 1.5f);
        if (g_font_display) {
            ImVec2 sz = g_font_display->CalcTextSizeA(18.0f, FLT_MAX, 0.0f, "F");
            DrawTextF(dl, g_font_display, 18.0f,
                      ImVec2(c.x - sz.x * 0.5f, c.y - sz.y * 0.5f),
                      kNerv.acc, "F");
        }
    }

    // Recent games (4 chips, 36×27 each)
    float y = origin.y + 50;
    for (int i = 0; i < (int)IM_ARRAYSIZE(kGames); ++i) {
        ImVec2 cp(origin.x + 6, y);
        ImVec2 cb(cp.x + 36, cp.y + 27);

        // Linear-gradient tint (135deg approx → diagonal corner mix).
        // ImDrawList has no native gradient, so split the chip into 4
        // triangles to fake 135° linear-gradient between two stops.
        const auto& g = kGames[i];
        ImU32 mid = MixCol(g.tint_a, g.tint_b, 0.5f);
        dl->AddRectFilledMultiColor(cp, cb,
                                    g.tint_a,  // top-left
                                    mid,       // top-right
                                    g.tint_b,  // bottom-right
                                    mid);      // bottom-left

        bool active = (i == g_appshell.current_game);
        dl->AddRect(cp, cb, active ? kNerv.acc : kNerv.line);
        if (active) {
            dl->AddRectFilled(ImVec2(cp.x - 2, cp.y + 4),
                              ImVec2(cp.x,     cb.y - 4),
                              kNerv.acc);
        }

        if (g_font_label) {
            ImVec2 sz = g_font_label->CalcTextSizeA(7.0f, FLT_MAX, 0.0f, g.code);
            DrawTextF(dl, g_font_label, 7.0f,
                      ImVec2(cp.x + (36 - sz.x) * 0.5f,
                             cp.y + (27 - sz.y) * 0.5f),
                      IM_COL32(255,255,255,255), g.code);
        }

        // Click handler
        ImGui::SetCursorScreenPos(cp);
        char btn_id[32]; std::snprintf(btn_id, sizeof(btn_id), "##rail.game.%d", i);
        if (ImGui::InvisibleButton(btn_id, ImVec2(36, 27))) {
            g_appshell.current_game = i;
        }
        y += 30;
    }

    // Nav icons (browse / rank / events / map / cfg).
    struct Nav { const char* glyph; const char* label; AppView view; };
    Nav navs[] = {
        { "\xe2\x97\x8e", "browse", kView_Browse   },  // ◎
        { "\xe2\x9c\xa6", "rank",   kView_Rankings },  // ✦
        { "\xe2\x96\xa3", "evts",   kView_Events   },  // ▣
    };
    Nav navs_bottom[] = {
        { "\xe2\x97\xab", "ctrl",   kView_Map },        // ◫
        { "\xe2\x9a\x99", "cfg",    kView_Cfg },        // ⚙
    };

    auto draw_nav = [&](const Nav& n, float ny) {
        bool active = g_appshell.view == n.view;
        ImVec2 cp(origin.x + 8, ny);
        if (active) {
            dl->AddRectFilled(ImVec2(cp.x - 2, cp.y),
                              ImVec2(cp.x,     cp.y + 24),
                              kNerv.acc);
        }
        ImU32 col = active ? kNerv.acc : kNerv.faint;
        if (g_font_body) {
            DrawTextF(dl, g_font_body, 14.0f,
                      ImVec2(cp.x + 12, cp.y + 0),
                      col, n.glyph);
        }
        if (g_font_micro) {
            DrawTextF(dl, g_font_micro, 7.0f,
                      ImVec2(cp.x + 4, cp.y + 16),
                      col, n.label);
        }
        ImGui::SetCursorScreenPos(cp);
        char btn_id[32]; std::snprintf(btn_id, sizeof(btn_id), "##rail.nav.%d", (int)n.view);
        if (ImGui::InvisibleButton(btn_id, ImVec2(32, 24))) {
            g_appshell.view = n.view;
        }
    };

    y += 6;
    for (const auto& n : navs) { draw_nav(n, y); y += 26; }

    // Bottom-anchored: avatar + bottom nav.
    float by = origin.y + kAS_H - 36;  // avatar
    {
        ImVec2 av(origin.x + 8, by);
        dl->AddRectFilledMultiColor(av, ImVec2(av.x + 32, av.y + 32),
                                    IM_COL32(0x5a,0x2a,0x85,0xff),
                                    IM_COL32(0xff,0x5a,0x3c,0xff),
                                    IM_COL32(0xff,0x5a,0x3c,0xff),
                                    IM_COL32(0x5a,0x2a,0x85,0xff));
        dl->AddRect(av, ImVec2(av.x + 32, av.y + 32), kNerv.acc);
        if (g_font_display) {
            ImVec2 sz = g_font_display->CalcTextSizeA(14.0f, FLT_MAX, 0.0f, "S");
            DrawTextF(dl, g_font_display, 14.0f,
                      ImVec2(av.x + (32 - sz.x) * 0.5f,
                             av.y + (32 - sz.y) * 0.5f),
                      IM_COL32(255,255,255,255), "S");
        }
        // online dot
        dl->AddCircleFilled(ImVec2(av.x + 30, av.y + 30), 4.0f, kNerv.phos);
        dl->AddCircle      (ImVec2(av.x + 30, av.y + 30), 4.0f, IM_COL32(0,0,0,255));
    }

    // Nav icons above avatar
    float by2 = by - 28 * (int)IM_ARRAYSIZE(navs_bottom);
    for (const auto& n : navs_bottom) { draw_nav(n, by2); by2 += 28; }
}

// ── TOP BAR ──────────────────────────────────────────────────────────
void DrawTopBar(ImVec2 origin) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float w = kAS_W - kAS_RailW;
    ImVec2 br(origin.x + w, origin.y + kAS_TopH);
    dl->AddRectFilled(origin, br, IM_COL32(0, 0, 0, 102));
    dl->AddLine(ImVec2(origin.x, br.y), ImVec2(br.x, br.y), kNerv.line, 1.0f);

    const auto& g = kGames[g_appshell.current_game];
    float x = origin.x + 8;
    float y_eyebrow = origin.y + 6;
    float y_body    = origin.y + 16;

    // Game eyebrow + short name
    if (g_font_micro) {
        DrawTextF(dl, g_font_micro, 9.0f,
                  ImVec2(x, y_eyebrow), kNerv.acc, g.code);
    }
    if (g_font_body) {
        DrawTextF(dl, g_font_body, 13.0f,
                  ImVec2(x + 50, y_eyebrow + 2), kNerv.ink, g.short_name);
    }
    // Divider
    dl->AddLine(ImVec2(x + 168, origin.y + 8),
                ImVec2(x + 168, origin.y + kAS_TopH - 8),
                kNerv.line, 1.0f);

    // Tabs
    struct Tab { AppView v; const char* k; };
    Tab tabs[] = {
        { kView_Lobby,    "LOBBY" },
        { kView_Replays,  "REPLAYS" },
        { kView_Rankings, "RANK" },
        { kView_Stats,    "STATS" },
        { kView_Events,   "EVTS" },
        { kView_Profile,  "PROFILE" },
    };
    float tx = x + 176;
    for (const auto& t : tabs) {
        ImVec2 sz = g_font_label
            ? g_font_label->CalcTextSizeA(9.0f, FLT_MAX, 0.0f, t.k)
            : ImGui::CalcTextSize(t.k);
        ImVec2 tp(tx, origin.y + 9);
        bool active = g_appshell.view == t.v;
        ImU32 col = active ? kNerv.acc : kNerv.ink2;
        DrawTextF(dl, g_font_label, 9.0f, tp, col, t.k);
        if (active) {
            dl->AddLine(ImVec2(tp.x - 2, br.y - 1),
                        ImVec2(tp.x + sz.x + 2, br.y - 1),
                        kNerv.acc, 2.0f);
        }
        // Click target
        ImGui::SetCursorScreenPos(ImVec2(tp.x - 4, origin.y + 6));
        char btn_id[32]; std::snprintf(btn_id, sizeof(btn_id), "##tab.%d", (int)t.v);
        ImGui::InvisibleButton(btn_id, ImVec2(sz.x + 8, kAS_TopH - 12));
        if (ImGui::IsItemClicked()) g_appshell.view = t.v;
        tx += sz.x + 14;
    }

    // Right-side action buttons (◐ TEST / ▤ TRAIN / ▶ BOOT-accent)
    struct ABtn { const char* label; bool accent; };
    ABtn abtns[] = {
        { "\xe2\x97\x90 TEST",  false },
        { "\xe2\x96\xa4 TRAIN", false },
        { "\xe2\x96\xb6 BOOT",  true  },
    };
    float ax = br.x - 8;
    for (int i = (int)IM_ARRAYSIZE(abtns) - 1; i >= 0; --i) {
        const auto& b = abtns[i];
        ImVec2 sz = g_font_label
            ? g_font_label->CalcTextSizeA(9.0f, FLT_MAX, 0.0f, b.label)
            : ImGui::CalcTextSize(b.label);
        ax -= sz.x + 10;
        ImVec2 bp(ax, origin.y + 8);
        ImVec2 b1(bp.x + sz.x + 6, bp.y + 14);
        ImU32 border = b.accent ? kNerv.acc : kNerv.line;
        ImU32 fg     = b.accent ? kNerv.acc : kNerv.ink2;
        if (b.accent) {
            dl->AddRectFilled(bp, b1, IM_COL32(0xff,0x5a,0x3c,0x14));
        }
        dl->AddRect(bp, b1, border);
        DrawTextF(dl, g_font_label, 9.0f,
                  ImVec2(bp.x + 3, bp.y + 1), fg, b.label);
        ax -= 4;
    }
}

// ── RIGHT RAIL ───────────────────────────────────────────────────────
void DrawRightRail(ImVec2 origin) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float h = kAS_H - kAS_TopH - kAS_StatusH;
    ImVec2 br(origin.x + kAS_RightW, origin.y + h);
    dl->AddRectFilled(origin, br, IM_COL32(8, 4, 14, 102));
    dl->AddLine(origin, ImVec2(origin.x, br.y), kNerv.line, 1.0f);

    // Header: ◈ IN MATCH · count
    {
        ImVec2 hb(br.x, origin.y + 16);
        dl->AddRectFilled(origin, hb, IM_COL32(0xff,0x5a,0x3c,0x10));
        dl->AddLine(ImVec2(origin.x, hb.y), ImVec2(br.x, hb.y), kNerv.line, 1.0f);
        if (g_font_micro) {
            DrawTextF(dl, g_font_micro, 8.0f,
                      ImVec2(origin.x + 6, origin.y + 4),
                      kNerv.acc, "\xe2\x97\x88 IN MATCH");
        }
        char num[8];
        std::snprintf(num, sizeof(num), "%d", (int)IM_ARRAYSIZE(kActiveMatches));
        if (g_font_body) {
            ImVec2 sz = g_font_body->CalcTextSizeA(11.0f, FLT_MAX, 0.0f, num);
            DrawTextF(dl, g_font_body, 11.0f,
                      ImVec2(br.x - sz.x - 6, origin.y + 2),
                      kNerv.acc, num);
        }
    }

    // Active matches list
    float y = origin.y + 18;
    for (const auto& m : kActiveMatches) {
        ImVec2 rp(origin.x + 4, y);
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%s vs %s", m.a, m.b);
        DrawTextF(dl, g_font_label, 9.0f, rp, kNerv.ink2, buf);
        std::snprintf(buf, sizeof(buf), "%d-%d  %s  %dms",
                      m.scoreA, m.scoreB, m.room, m.ping);
        DrawTextF(dl, g_font_label, 9.0f,
                  ImVec2(rp.x, rp.y + 10), kNerv.dim, buf);
        y += 22;
    }

    // Looking-to-play header
    {
        ImVec2 hp(origin.x, y + 4);
        ImVec2 hb(br.x, hp.y + 14);
        dl->AddLine(ImVec2(hp.x, hp.y), ImVec2(br.x, hp.y), kNerv.line, 1.0f);
        if (g_font_micro) {
            DrawTextF(dl, g_font_micro, 8.0f,
                      ImVec2(hp.x + 6, hp.y + 3),
                      kNerv.dim, "LOOKING TO PLAY");
        }
        char num[8];
        std::snprintf(num, sizeof(num), "%d", (int)IM_ARRAYSIZE(kUsers));
        if (g_font_body) {
            ImVec2 sz = g_font_body->CalcTextSizeA(11.0f, FLT_MAX, 0.0f, num);
            DrawTextF(dl, g_font_body, 11.0f,
                      ImVec2(br.x - sz.x - 6, hp.y + 1),
                      kNerv.acc, num);
        }
        dl->AddLine(ImVec2(hp.x, hb.y), ImVec2(br.x, hb.y), kNerv.line, 1.0f);
        y = hb.y + 1;
    }

    // User rows. Hover style: subtle background tint + 2-px left bar.
    int hovered = -1;
    for (int i = 0; i < (int)IM_ARRAYSIZE(kUsers); ++i) {
        const auto& u = kUsers[i];
        ImVec2 rp(origin.x, y);
        ImVec2 r1(br.x, y + 14);
        // hover hit area
        ImGui::SetCursorScreenPos(rp);
        char btn_id[32]; std::snprintf(btn_id, sizeof(btn_id), "##usr.%d", i);
        ImGui::InvisibleButton(btn_id, ImVec2(kAS_RightW, 14));
        bool hov = ImGui::IsItemHovered();
        if (hov) hovered = i;

        if (hov) {
            dl->AddRectFilled(rp, r1, IM_COL32(0xff,0x5a,0x3c,0x14));
            dl->AddRectFilled(rp, ImVec2(rp.x + 2, r1.y), kNerv.acc);
        }
        DrawFlagDot(dl, ImVec2(rp.x + 6, rp.y + 3), u.region);
        ImU32 name_col = u.in_match ? kNerv.faint : kNerv.ink;
        DrawTextF(dl, g_font_label, 9.0f,
                  ImVec2(rp.x + 22, rp.y + 2),
                  name_col, u.name);
        DrawPingWave(dl, ImVec2(br.x - 60, rp.y + 3), u.ping);
        // status glyph
        const char* sg = u.in_match ? "\xe2\x97\x88" : "\xe2\x97\x8f";
        ImU32       sc = u.in_match ? kNerv.acc      : kNerv.phos;
        DrawTextF(dl, g_font_label, 9.0f,
                  ImVec2(br.x - 14, rp.y + 2), sc, sg);
        y += 14;
        if (y > br.y - 14) break;  // overflow — design has scroll, we clip
    }
    g_appshell.user_hover = hovered;
}

// ── STATUS BAR ───────────────────────────────────────────────────────
void DrawStatusBar(ImVec2 origin) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float w = kAS_W - kAS_RailW;
    ImVec2 br(origin.x + w, origin.y + kAS_StatusH);
    dl->AddRectFilled(origin, br, IM_COL32(0,0,0,127));
    dl->AddLine(origin, ImVec2(br.x, origin.y), kNerv.line, 1.0f);

    if (!g_font_label) return;
    float fs = 9.0f;
    float y = origin.y + 4;
    float x = origin.x + 6;
    DrawTextF(dl, g_font_label, fs, ImVec2(x, y), kNerv.acc,  "\xe2\x97\x8f");
    DrawTextF(dl, g_font_label, fs, ImVec2(x + 10, y), kNerv.faint,
              "ONLINE \xc2\xb7 STUN.RELAY-TOKYO");
    DrawTextF(dl, g_font_label, fs, ImVec2(x + 168, y), kNerv.ink2, "14ms");
    DrawTextF(dl, g_font_label, fs, ImVec2(x + 200, y), kNerv.phos, "NAT \xc2\xb7 OPEN");
    DrawTextF(dl, g_font_label, fs, ImVec2(x + 260, y), kNerv.faint, "SLOTS 14/16");
    const char* build = "FM2K_ROLLBACK \xc2\xb7 0.2.8 \xc2\xb7 build 0xA01F";
    ImVec2 sz = g_font_label->CalcTextSizeA(fs, FLT_MAX, 0.0f, build);
    DrawTextF(dl, g_font_label, fs, ImVec2(br.x - sz.x - 6, y), kNerv.faint, build);
}

// ── VIEW BODIES ──────────────────────────────────────────────────────
// Each renders inside the main content rect (origin + size). Default
// view is Lobby — fleshed out. The rest are stubs that show the view
// name as a placeholder; flesh out as needed.

void DrawLobbyView(ImVec2 origin, ImVec2 size) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const auto& g = kGames[g_appshell.current_game];

    // Hero plate at top — game art block + key info
    ImVec2 hero = origin;
    ImVec2 hero1(hero.x + size.x, hero.y + 92);
    ImU32 mid = MixCol(g.tint_a, g.tint_b, 0.5f);
    dl->AddRectFilledMultiColor(hero, hero1,
                                g.tint_a, mid, g.tint_b, mid);
    // dark vignette
    for (int i = 0; i < 92; ++i) {
        ImU32 c = IM_COL32(0, 0, 0, (int)(80.0f * (1.0f - (float)i / 92.0f)));
        dl->AddLine(ImVec2(hero.x, hero1.y - i),
                    ImVec2(hero1.x, hero1.y - i), c);
    }
    if (g_font_display) {
        DrawTextF(dl, g_font_display, 26.0f,
                  ImVec2(hero.x + 14, hero.y + 8), kNerv.ink, g.code);
    }
    if (g_font_label) {
        DrawTextF(dl, g_font_label, 10.0f,
                  ImVec2(hero.x + 14, hero.y + 40), kNerv.ink2, g.short_name);
    }
    if (g_font_micro) {
        DrawTextF(dl, g_font_micro, 8.0f,
                  ImVec2(hero.x + 14, hero.y + 56), kNerv.acc,
                  "\xe2\x97\x88 LOBBY \xc2\xb7 ROLLBACK \xc2\xb7 5\xe2\x80\x935 PEERS");
    }
    if (g_font_label) {
        DrawTextF(dl, g_font_label, 9.0f,
                  ImVec2(hero.x + 14, hero.y + 70), kNerv.dim,
                  "PROTOCOL FM2K-RB v0.2.8 \xc2\xb7 GEKKONET INPUT-DELAY-ROLLBACK");
    }

    // Resource links row
    ImVec2 res(hero.x + 14, hero1.y + 8);
    struct Res { const char* k; const char* u; };
    Res rs[] = {
        { "WIKI",      "fm2krb.wiki"      },
        { "DISCORD",   "discord.gg/2dfm"  },
        { "GUIDE",     "kof98rb.tips"     },
        { "FRAMEDATA", "framedata.2dfm"   },
    };
    for (const auto& r : rs) {
        ImVec2 sz = g_font_label
            ? g_font_label->CalcTextSizeA(9.0f, FLT_MAX, 0.0f, r.k)
            : ImGui::CalcTextSize(r.k);
        DrawTextF(dl, g_font_label, 9.0f, res, kNerv.acc, r.k);
        DrawTextF(dl, g_font_label, 9.0f,
                  ImVec2(res.x + sz.x + 6, res.y), kNerv.dim, r.u);
        res.x += sz.x + 6 + 90;
    }

    // Chat panel (mock lines)
    ImVec2 chat(origin.x + 14, hero1.y + 28);
    ImVec2 chat1(origin.x + size.x - 14, origin.y + size.y - 12);
    dl->AddRect(chat, chat1, kNerv.line);
    if (g_font_micro) {
        DrawTextF(dl, g_font_micro, 8.0f,
                  ImVec2(chat.x + 6, chat.y + 4), kNerv.dim,
                  "GLOBAL CHAT \xc2\xb7 #lobby");
    }
    struct Line { const char* ts; const char* who; const char* text; bool sys; bool me; };
    Line lines[] = {
        { "02:38", "DONG_JACK_G_MAN", "looking for a kof98 set anyone?",      false, false },
        { "02:39", "B2K (MVR)",       "down lemme finish this game",          false, false },
        { "02:40", "system",          "ElRukoKofero joined channel",          true,  false },
        { "02:41", "elbebe",          "new patch dropped, frame data updated", false, false },
        { "02:41", "DeathKaizer",     "yo elbebe FT5?",                       false, false },
        { "02:42", "elbebe",          "claro vamonos",                        false, false },
        { "02:43", "system",          "elbebe vs DeathKaizer \xc2\xb7 #6F2A \xc2\xb7 18ms", true, false },
        { "02:44", "sloth#7421",      "anyone wanna run a set on east stage", false, true  },
        { "02:45", "ALK_SaDen",       "I'll join in 5",                       false, false },
        { "02:47", "system",          "EVENT REMINDER \xc2\xb7 BRASILEIRO 30 starts in 13d", true, false },
    };
    float ly = chat.y + 18;
    for (const auto& l : lines) {
        if (ly > chat1.y - 14) break;
        ImU32 ts_col  = kNerv.faint;
        ImU32 who_col = l.sys ? kNerv.dim
                              : (l.me ? kNerv.acc : kNerv.ink2);
        ImU32 txt_col = l.sys ? kNerv.dim : kNerv.ink;
        DrawTextF(dl, g_font_label, 9.0f, ImVec2(chat.x + 6, ly), ts_col, l.ts);
        char who_buf[48];
        std::snprintf(who_buf, sizeof(who_buf), "%s%s",
                      l.sys ? "" : "<", l.who);
        DrawTextF(dl, g_font_label, 9.0f, ImVec2(chat.x + 40, ly), who_col, who_buf);
        DrawTextF(dl, g_font_body, 12.0f,
                  ImVec2(chat.x + 130, ly - 2), txt_col, l.text);
        ly += 12;
    }
}

void DrawStubView(ImVec2 origin, ImVec2 size, const char* title, const char* note) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (g_font_micro) {
        DrawTextF(dl, g_font_micro, 9.0f,
                  ImVec2(origin.x + 14, origin.y + 14), kNerv.dim,
                  "// stub view");
    }
    if (g_font_display) {
        DrawTextF(dl, g_font_display, 26.0f,
                  ImVec2(origin.x + 14, origin.y + 28), kNerv.ink, title);
    }
    if (g_font_body) {
        DrawTextF(dl, g_font_body, 14.0f,
                  ImVec2(origin.x + 14, origin.y + 70), kNerv.ink2, note);
    }
    // Cross-hatch placeholder fill so empty views are visually distinct.
    for (float x = origin.x; x < origin.x + size.x; x += 20) {
        dl->AddLine(ImVec2(x, origin.y + 110),
                    ImVec2(x + 14, origin.y + 124),
                    IM_COL32(255,255,255,12));
    }
    (void)dl;
}

void DrawMainContent(ImVec2 origin, ImVec2 size) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    // Region clip so view bodies can't bleed into adjacent panels.
    dl->PushClipRect(origin,
                     ImVec2(origin.x + size.x, origin.y + size.y),
                     true);

    switch (g_appshell.view) {
        case kView_Lobby:    DrawLobbyView(origin, size); break;
        case kView_Browse:   DrawStubView(origin, size, "BROWSE",
                                          "All games, sortable. Selecting one swaps the lobby."); break;
        case kView_Replays:  DrawStubView(origin, size, "REPLAYS",
                                          "Pattern browser \xc2\xb7 frame scrubber. TODO."); break;
        case kView_Rankings: DrawStubView(origin, size, "RANK",
                                          "ELO ladder for current game. TODO."); break;
        case kView_Stats:    DrawStubView(origin, size, "STATS",
                                          "Per-character W/L, recent matches. TODO."); break;
        case kView_Events:   DrawStubView(origin, size, "EVENTS",
                                          "Tournament bracket / scheduling. TODO."); break;
        case kView_Profile:  DrawStubView(origin, size, "PROFILE",
                                          "sloth#7421 \xc2\xb7 1842 ELO \xc2\xb7 412/318 W-L. TODO."); break;
        case kView_Cfg:      DrawStubView(origin, size, "CONFIG",
                                          "Input \xc2\xb7 audio \xc2\xb7 video \xc2\xb7 net. TODO."); break;
        case kView_Map:      DrawStubView(origin, size, "CTRL MAP",
                                          "Controller binder. TODO."); break;
        default:             break;
    }

    dl->PopClipRect();
}

// ── MAIN PANEL ──────────────────────────────────────────────────────
void RenderAppShell() {
    if (!g_appshell.open) return;

    ImGui::SetNextWindowSize(ImVec2(kAS_W, kAS_H), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(kAS_W, kAS_H),
                                        ImVec2(kAS_W, FLT_MAX));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, kNerv.bg);
    ImGui::PushStyleColor(ImGuiCol_TitleBg, kNerv.bg2);
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive, kNerv.bg2);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));

    bool show = ImGui::Begin("AppShell \xe2\x80\xa2 END-TO-END NAVIGABLE \xe2\x80\xa2 Sandbox",
                             &g_appshell.open,
                             ImGuiWindowFlags_NoCollapse);
    ImGui::PopStyleVar();
    if (!show) {
        ImGui::End();
        ImGui::PopStyleColor(3);
        return;
    }

    ImVec2 win_origin = ImGui::GetCursorScreenPos();

    // Layout (fixed at design dims regardless of window resize for now —
    // any extra height shows a gap; horizontal min-locks at 640).
    DrawLeftRail (ImVec2(win_origin.x,                                 win_origin.y));
    DrawTopBar   (ImVec2(win_origin.x + kAS_RailW,                     win_origin.y));
    DrawRightRail(ImVec2(win_origin.x + kAS_W - kAS_RightW,             win_origin.y + kAS_TopH));
    DrawStatusBar(ImVec2(win_origin.x + kAS_RailW,                     win_origin.y + kAS_H - kAS_StatusH));
    DrawMainContent(
        ImVec2(win_origin.x + kAS_RailW,                               win_origin.y + kAS_TopH),
        ImVec2(kAS_W - kAS_RailW - kAS_RightW,
               kAS_H - kAS_TopH - kAS_StatusH));

    // Reserve space so ImGui's auto-cursor doesn't get confused.
    ImGui::Dummy(ImVec2(kAS_W, kAS_H));

    ImGui::End();
    ImGui::PopStyleColor(3);
}

}  // namespace

void LoadDesignFonts() {
    ImGuiIO& io = ImGui::GetIO();

    // Path-search the same way the icon load in FM2K_RollbackClient.cpp
    // does — handles running both from <repo>/build/ and from a deployed
    // location next to assets/.
    static const char* kFontDirs[] = {
        "assets/fonts/",
        "../assets/fonts/",
        "../../assets/fonts/",
    };

    auto load = [&](const char* fname,
                    float       size,
                    const ImWchar* range) -> ImFont* {
        ImFontConfig cfg;
        cfg.OversampleH = 1;
        cfg.OversampleV = 1;
        cfg.PixelSnapH  = true;       // pixel fonts: never subpixel-shift
        cfg.MergeMode   = false;
        char buf[512];
        for (const char* dir : kFontDirs) {
            std::snprintf(buf, sizeof(buf), "%s%s", dir, fname);
            ImFont* f = io.Fonts->AddFontFromFileTTF(buf, size, &cfg, range);
            if (f) return f;
        }
        return nullptr;  // sandbox falls back to default font
    };

    // VT323 / Silkscreen / Press Start 2P are Latin-only — default range
    // covers ASCII + Latin-1 which is all we need for boot log lines.
    g_font_body    = load("VT323-Regular.ttf",       17.0f, nullptr);
    g_font_label   = load("Silkscreen-Regular.ttf",  12.0f, nullptr);
    g_font_micro   = load("PressStart2P-Regular.ttf", 9.0f, nullptr);
    // DotGothic16 — Japanese pixel font. Restrict to ASCII + a slim
    // Hiragana/Katakana + a handful of CJK ideographs from the design's
    // header labels (起動シーケンス etc.). Loading the full kanji set
    // would balloon the atlas (DotGothic16-Regular.ttf is 2 MB on
    // disk; the corresponding glyph pack in the atlas is large enough
    // to matter at 28 px). Default-range Latin first; merging the JP
    // range is left as a follow-up since the existing MS Gothic merge
    // already covers JP coverage at the system-font level.
    g_font_display = load("DotGothic16-Regular.ttf", 28.0f, nullptr);
}

void RenderViewMenuItems() {
    if (ImGui::MenuItem("Design Sandbox \xe2\x80\xa2 Cold Boot",
                        nullptr,
                        g_cold_boot.open)) {
        g_cold_boot.open = !g_cold_boot.open;
        if (g_cold_boot.open) {
            g_cold_boot.armed = false;  // reset anim on re-open
        }
    }
    if (ImGui::MenuItem("Design Sandbox \xe2\x80\xa2 Style Guide",
                        nullptr,
                        g_style.open)) {
        g_style.open = !g_style.open;
        if (g_style.open) {
            g_style.armed = false;
        }
    }
    if (ImGui::MenuItem("Design Sandbox \xe2\x80\xa2 AppShell (E2E)",
                        nullptr,
                        g_appshell.open)) {
        g_appshell.open = !g_appshell.open;
    }
}

void RenderPanels() {
    RenderColdBoot();
    RenderStyleGuide();
    RenderAppShell();
}

}  // namespace fm2k::sandbox
