// frontend/AppShell.cpp — production AppShell. Layout + chrome
// (LeftRail / TopBar / RightRoster / StatusBar) + the Hub landing
// view, all bound to real LauncherUI data. No mock tables.
//
// M1 scope: structure + real data, no first-run wizard, no
// transitions, no motion. Boot straight to Hub.
//
// Layout (viewport-filling, never below 640×448):
//   ┌──┬──────────────────────────────────────┬──────────────┐ kTopH
//   │  ├──────────────────────────────────────┼──────────────┤
//   │  │                                      │              │
//   │R │              MAIN VIEW               │  RIGHT RAIL  │
//   │  │                                      │              │
//   │  ├──────────────────────────────────────┴──────────────┤ kStatusH
//   └──┴────────────────────────────────────────────────────-┘
//   kRailW                                       kRightW
//
// All Draw* helpers take an origin + size pair (so the layout math
// happens once at the top of Render) and use ImDrawList directly —
// faster + tighter visual control than nested BeginChild children.
#include "AppShell.h"
#include "ShellState.h"
#include "Theme.h"
#include "Settings.h"
#include "IconsFontAwesome6.h"

#include "FM2K_Integration.h"
#include "FM2K_HubClient.h"
#include "FM2K_DiscordAuth.h"
#include "FM2KHook/src/ui/input_binder.h"

#include "imgui.h"
#include "imgui_internal.h"   // ImHashStr (kept for future imanim ids)
#include "im_anim.h"          // iam_tween_float, iam_ease_desc, etc.

#include <SDL3/SDL.h>          // SDL_LogInfo for route-transition tracing

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

namespace fm2k::shell {

namespace {

// ─── shared state ────────────────────────────────────────────────────
// EnergyField particle backdrop — used by SplashV2 per design intent
// (app.jsx EnergyField). Persistent across frames; seeded lazily on
// first render. Particles drift, fade, respawn on death/edge-cross.
// Capacity is the hard cap; the active count is `g_tune.field_count`,
// so the dev panel can crank live. Bumping this constant requires a
// rebuild but the slider clamps to it.
struct EnergyParticle {
    float x, y;
    float vx, vy;
    float life;       // 0..1; -t/dt fades to 0 then respawns
    float seed;       // per-particle phase offset for sin drift
    float radius;     // px; randomized per particle for depth
};
constexpr int kEnergyParticleCap = 400;
EnergyParticle g_energy[kEnergyParticleCap];
bool           g_energy_seeded   = false;
int            g_energy_seed_cnt = 0;     // count we last seeded for; reseed on change

// ─── runtime-tunable knobs (M4 / dev panel) ──────────────────────────
// Single-source-of-truth for splash visuals. Persisted to settings.ini
// under tune_* keys via LoadTune/SaveTune. The dev panel (Ctrl+`)
// edits this struct live; values take effect on the next frame. Keep
// fields here when you find yourself eyeballing a magic number twice.
struct Tune {
    // EnergyField (snow/starfield backdrop)
    int   field_count        = 140;
    float field_intensity    = 0.55f;
    float field_speed_min    =   6.0f;
    float field_speed_max    =  24.0f;
    float field_wobble_amp   =  14.0f;
    float field_size_min     =   1.0f;
    float field_size_max     =   2.4f;
    float field_life_decay   =   0.18f;

    // Lissajous corner gizmo
    float lissa_size         = 200.0f;
    int   lissa_a            = 3;
    int   lissa_b            = 2;
    float lissa_speed        = 0.7f;
    int   lissa_trail        = 120;
    float lissa_margin_x     = 22.0f;
    float lissa_margin_y     = 12.0f;
    float lissa_dot_radius   = 2.5f;
    float lissa_line_width   = 1.5f;

    // Splash text sizing
    float splash_title_pt    = 78.0f;

    // Panel state — NOT persisted, just keeps visibility across frames.
    bool  panel_open         = false;
};
Tune g_tune;
bool g_tune_loaded = false;

void LoadTune() {
    if (g_tune_loaded) return;
    const std::string path = SettingsPath();
    if (path.empty()) { g_tune_loaded = true; return; }
    g_tune.field_count       = ReadInt  (path, "tune_field_count",       g_tune.field_count);
    g_tune.field_intensity   = ReadFloat(path, "tune_field_intensity",   g_tune.field_intensity);
    g_tune.field_speed_min   = ReadFloat(path, "tune_field_speed_min",   g_tune.field_speed_min);
    g_tune.field_speed_max   = ReadFloat(path, "tune_field_speed_max",   g_tune.field_speed_max);
    g_tune.field_wobble_amp  = ReadFloat(path, "tune_field_wobble_amp",  g_tune.field_wobble_amp);
    g_tune.field_size_min    = ReadFloat(path, "tune_field_size_min",    g_tune.field_size_min);
    g_tune.field_size_max    = ReadFloat(path, "tune_field_size_max",    g_tune.field_size_max);
    g_tune.field_life_decay  = ReadFloat(path, "tune_field_life_decay",  g_tune.field_life_decay);
    g_tune.lissa_size        = ReadFloat(path, "tune_lissa_size",        g_tune.lissa_size);
    g_tune.lissa_a           = ReadInt  (path, "tune_lissa_a",           g_tune.lissa_a);
    g_tune.lissa_b           = ReadInt  (path, "tune_lissa_b",           g_tune.lissa_b);
    g_tune.lissa_speed       = ReadFloat(path, "tune_lissa_speed",       g_tune.lissa_speed);
    g_tune.lissa_trail       = ReadInt  (path, "tune_lissa_trail",       g_tune.lissa_trail);
    g_tune.lissa_margin_x    = ReadFloat(path, "tune_lissa_margin_x",    g_tune.lissa_margin_x);
    g_tune.lissa_margin_y    = ReadFloat(path, "tune_lissa_margin_y",    g_tune.lissa_margin_y);
    g_tune.lissa_dot_radius  = ReadFloat(path, "tune_lissa_dot_radius",  g_tune.lissa_dot_radius);
    g_tune.lissa_line_width  = ReadFloat(path, "tune_lissa_line_width",  g_tune.lissa_line_width);
    g_tune.splash_title_pt   = ReadFloat(path, "tune_splash_title_pt",   g_tune.splash_title_pt);
    g_font_display_scale     = ReadFloat(path, "tune_font_display_scale", g_font_display_scale);
    g_font_body_scale        = ReadFloat(path, "tune_font_body_scale",    g_font_body_scale);
    g_font_label_scale       = ReadFloat(path, "tune_font_label_scale",   g_font_label_scale);
    g_font_micro_scale       = ReadFloat(path, "tune_font_micro_scale",   g_font_micro_scale);
    {
        const int t = ReadInt(path, "theme_index", (int)Theme::Nerv);
        SetTheme(static_cast<Theme>(
            (t >= 0 && t < (int)Theme::kCount) ? t : (int)Theme::Nerv));
    }
    g_tune_loaded = true;
}

void SaveTune() {
    const std::string path = SettingsPath();
    if (path.empty()) return;
    WriteInt  (path, "tune_field_count",       g_tune.field_count);
    WriteFloat(path, "tune_field_intensity",   g_tune.field_intensity);
    WriteFloat(path, "tune_field_speed_min",   g_tune.field_speed_min);
    WriteFloat(path, "tune_field_speed_max",   g_tune.field_speed_max);
    WriteFloat(path, "tune_field_wobble_amp",  g_tune.field_wobble_amp);
    WriteFloat(path, "tune_field_size_min",    g_tune.field_size_min);
    WriteFloat(path, "tune_field_size_max",    g_tune.field_size_max);
    WriteFloat(path, "tune_field_life_decay",  g_tune.field_life_decay);
    WriteFloat(path, "tune_lissa_size",        g_tune.lissa_size);
    WriteInt  (path, "tune_lissa_a",           g_tune.lissa_a);
    WriteInt  (path, "tune_lissa_b",           g_tune.lissa_b);
    WriteFloat(path, "tune_lissa_speed",       g_tune.lissa_speed);
    WriteInt  (path, "tune_lissa_trail",       g_tune.lissa_trail);
    WriteFloat(path, "tune_lissa_margin_x",    g_tune.lissa_margin_x);
    WriteFloat(path, "tune_lissa_margin_y",    g_tune.lissa_margin_y);
    WriteFloat(path, "tune_lissa_dot_radius",  g_tune.lissa_dot_radius);
    WriteFloat(path, "tune_lissa_line_width",  g_tune.lissa_line_width);
    WriteFloat(path, "tune_splash_title_pt",   g_tune.splash_title_pt);
    WriteFloat(path, "tune_font_display_scale", g_font_display_scale);
    WriteFloat(path, "tune_font_body_scale",    g_font_body_scale);
    WriteFloat(path, "tune_font_label_scale",   g_font_label_scale);
    WriteFloat(path, "tune_font_micro_scale",   g_font_micro_scale);
    WriteInt  (path, "theme_index",             (int)CurrentTheme());
}

void ResetTuneDefaults() {
    g_tune = Tune{};
    g_energy_seeded = false;       // particle count + ranges changed
    g_font_display_scale = 1.0f;
    g_font_body_scale    = 1.0f;
    g_font_label_scale   = 1.0f;
    g_font_micro_scale   = 1.0f;
}

// User mini menu state — used by DrawRightRoster (click) and
// RenderUserMiniMenu (popover render). Defined up here so the popup
// id and state are visible from both.
struct UserMenuState {
    std::string user_id;
    std::string user_nick;
    bool        user_in_match = false;
    ImVec2      anchor_pos { 0, 0 };
};
UserMenuState g_user_menu;
constexpr const char* kUserMenuPopupId = "##user.menu";

// Match hover card state — used by DrawRightRoster IN MATCH list and
// RenderMatchHoverCard. Hover detection sets the index + anchor; the
// card auto-dismisses after kHoverGraceSec without a fresh hover.
struct MatchHoverState {
    int    match_idx  = -1;
    ImVec2 anchor_pos {0, 0};
    double last_seen  = 0.0;
};
MatchHoverState g_match_hover;
constexpr double kMatchHoverGraceSec = 0.18;

// ─── helpers ─────────────────────────────────────────────────────────

// Truncate `s` so the byte length is <= max_bytes WITHOUT cutting a
// UTF-8 codepoint mid-byte. Walks back to the previous lead byte if
// the cut lands on a continuation byte. Used everywhere we render a
// truncated game id / label so Japanese/Korean/etc. don't garble.
std::string Utf8SafeTrunc(const std::string& s, size_t max_bytes) {
    if (s.size() <= max_bytes) return s;
    size_t cut = max_bytes;
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) {
        --cut;
    }
    return s.substr(0, cut);
}

// Strip ".exe"/".EXE" suffix from a filename to derive a hub game id.
// Pattern matches the legacy join at FM2K_LauncherUI.cpp:3140 — hub's
// canonical room id is the exe stem.
std::string GameIdFromExeName(const std::string& exe_name) {
    if (exe_name.size() < 4) return exe_name;
    const std::string lower(exe_name.size() ? exe_name.size() : 0, '\0');
    // Manual ASCII case-insensitive endswith check for ".exe".
    size_t n = exe_name.size();
    if (n >= 4) {
        char a = exe_name[n - 4];
        char b = exe_name[n - 3];
        char c = exe_name[n - 2];
        char d = exe_name[n - 1];
        auto tolower = [](char ch) {
            return (ch >= 'A' && ch <= 'Z') ? (char)(ch + 32) : ch;
        };
        if (a == '.' && tolower(b) == 'e' && tolower(c) == 'x' && tolower(d) == 'e') {
            return exe_name.substr(0, n - 4);
        }
    }
    return exe_name;
}

// Pick a 2-color tint for a game chip from its hash. Stable across
// runs (deterministic from xxh64) so the same game always looks the
// same. Replaced once we have curated per-game art.
struct ChipTint { ImU32 a, b; };
ChipTint TintForGame(const FM2K::FM2KGameInfo& g) {
    static constexpr ImU32 kPalette[] = {
        IM_COL32(0x5a,0x2a,0x85,0xff),
        IM_COL32(0xff,0x30,0x30,0xff),
        IM_COL32(0x00,0x40,0xa0,0xff),
        IM_COL32(0x4a,0xda,0xcc,0xff),
        IM_COL32(0x3d,0x1c,0x5e,0xff),
        IM_COL32(0xff,0x5a,0x3c,0xff),
        IM_COL32(0xff,0xb9,0x38,0xff),
        IM_COL32(0x00,0xa0,0x50,0xff),
        IM_COL32(0xc0,0x68,0xf0,0xff),
        IM_COL32(0x00,0x90,0xff,0xff),
    };
    constexpr int N = (int)(sizeof(kPalette) / sizeof(kPalette[0]));
    uint64_t h = g.xxh64 ? g.xxh64 : (uint64_t)std::hash<std::string>{}(g.exe_path);
    int ai = (int)(h % N);
    int bi = (int)((h / N) % N);
    if (bi == ai) bi = (ai + 3) % N;
    return { kPalette[ai], kPalette[bi] };
}

// Short label that fits in the 36-px-wide LeftRail chip. Drops the
// extension and any version suffix in parens; ALL CAPS.
std::string ShortChipLabel(const FM2K::FM2KGameInfo& g) {
    std::string stem = GameIdFromExeName(g.GetExeName());
    // Cut at first underscore or space — game ids tend to be
    // "KOF98_Reborn" or "WonderfulWorld_ver_0946"; first segment
    // reads better at chip size.
    size_t cut = std::string::npos;
    for (size_t i = 0; i < stem.size(); ++i) {
        if (stem[i] == '_' || stem[i] == ' ' || stem[i] == '-') { cut = i; break; }
    }
    std::string s = (cut != std::string::npos) ? stem.substr(0, cut) : stem;
    // Cap at 6 chars for chip readability.
    if (s.size() > 6) s.resize(6);
    for (auto& ch : s) ch = (ch >= 'a' && ch <= 'z') ? (char)(ch - 32) : ch;
    return s;
}

// Build the "active rooms" list: subscribed rooms ∪ installed games.
// First-class rooms (game installed) come first; lurk-only rooms
// (subscribed but no install) follow in a faded chip style. M1: just
// returns installed games — subscribed_rooms wiring is M2.
struct ActiveRoom {
    std::string                    game_id;
    const FM2K::FM2KGameInfo*      info;       // nullptr = lurk-only
    std::string                    label;
    ChipTint                       tint;
};
std::vector<ActiveRoom> BuildActiveRooms(LauncherUI& lu) {
    auto& s = State();
    std::vector<ActiveRoom> out;
    out.reserve(lu.games().size() + 4);
    for (const auto& g : lu.games()) {
        const std::string gid = GameIdFromExeName(g.GetExeName());
        // Filter by subscribed_rooms when the user has explicitly
        // curated their list (M2.5 wizard). Empty list = M1 default
        // behavior (LeftRail shows all installed games — useful before
        // the user has run the wizard).
        if (!s.subscribed_rooms.empty()) {
            const bool subscribed =
                std::find(s.subscribed_rooms.begin(),
                          s.subscribed_rooms.end(), gid) !=
                s.subscribed_rooms.end();
            if (!subscribed) continue;
        }
        ActiveRoom r;
        r.game_id = gid;
        r.info    = &g;
        r.label   = ShortChipLabel(g);
        r.tint    = TintForGame(g);
        out.push_back(std::move(r));
    }
    return out;
}

// ─── LEFT RAIL ───────────────────────────────────────────────────────
// ─── SHELL TITLEBAR ──────────────────────────────────────────────────
// 32-px custom titlebar rendered at the very top of the SDL viewport,
// taking the place of the OS title bar (which we removed by passing
// SDL_WINDOW_BORDERLESS in FM2K_RollbackClient.cpp). Hit-test set up
// over there returns DRAGGABLE for the empty middle so users can move
// the window by grabbing it; the brand/tab area at the left and the
// window-glyph cluster at the right are NORMAL so ImGui clicks fire.
//
// Layout (matches hit-test x ranges in FM2K_RollbackClient.cpp):
//   [0..100]  brand sigil (• 2DFM forever.)
//   [100..360] 4 tabs: 01 INTRO · 02 LOGIN · 03 SETUP · 04 HUB
//   [360..w-78] draggable empty middle (status pill optional)
//   [w-78..w] 3 window glyphs: − □ ✕  (26 px each)
void DrawShellTitlebar(LauncherUI& lu, ImVec2 origin, float w) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float titlebar_h = S(kTitlebarH);
    ImVec2 br(origin.x + w, origin.y + titlebar_h);
    auto& s = State();

    // Flat solid background — the prior top-down gradient was visual
    // noise; the divider line below is enough to separate from body.
    dl->AddRectFilled(origin, br, kNerv.bg);
    dl->AddLine(ImVec2(origin.x, br.y - 1),
                ImVec2(br.x, br.y - 1), kNerv.line, 1.0f);

    // ── BRAND SIGIL ─────────────────────────────────────────
    float x = origin.x + S(8);
    // Pulsing accent dot (subtle)
    {
        const float t = (float)ImGui::GetTime();
        const float pulse = 0.7f + 0.3f *
            (0.5f + 0.5f * std::sin(t * (IM_PI * 2.0f / 2.4f)));
        ImVec4 c = ImGui::ColorConvertU32ToFloat4(kNerv.acc); c.w *= pulse;
        ImU32 col = ImGui::ColorConvertFloat4ToU32(c);
        dl->AddRectFilled(ImVec2(x, origin.y + S(13)),
                          ImVec2(x + S(5), origin.y + S(18)), col);
        x += S(9);
    }
    // 2DFM word
    DrawTextF(dl, g_font_label, 10.0f,
              ImVec2(x, origin.y + S(11)), kNerv.ink, "2DFM");
    x += S(38);
    // Right vertical divider
    dl->AddLine(ImVec2(x + S(4), origin.y + S(6)),
                ImVec2(x + S(4), origin.y + titlebar_h - S(6)),
                kNerv.line, 1.0f);
    x += S(12);

    // ── TABS ────────────────────────────────────────────────
    // Route-aware. Onboarding shows the wizard step tabs; Hub shows
    // the hub-view tabs (LOBBY / REPLAYS / RANK / STATS / EVTS /
    // PROFILE). Click jumps either route or hub_view.
    // x advances as we render tabs; we publish the final x into
    // g_titlebar_tabs_end_logical_x (in logical units) so the SDL
    // hit-test marks empty titlebar space past the tabs as DRAGGABLE.
    const float tabs_start_x = x;
    const bool in_onboarding = (s.route == ShellRoute::Splash ||
                                s.route == ShellRoute::Login  ||
                                s.route == ShellRoute::Setup  ||
                                s.route == ShellRoute::Completion);
    if (in_onboarding) {
        struct WTab {
            const char* idx;
            const char* k;
            ShellRoute  r;
        };
        WTab tabs[] = {
            { "01", "INTRO",  ShellRoute::Splash },
            { "02", "LOGIN",  ShellRoute::Login  },
            { "03", "SETUP",  ShellRoute::Setup  },
            { "04", "HUB",    ShellRoute::Hub    },
        };
        const float kTabW = S(64.0f);
        const bool hub_locked = ShouldRunWizard(s) ||
                                !lu.discord_signed_in();
        for (const auto& t : tabs) {
            const bool is_hub_tab = (t.r == ShellRoute::Hub);
            const bool disabled = (is_hub_tab && hub_locked);
            const bool active = (s.route == t.r);
            ImVec2 tp(x, origin.y);

            if (active) {
                dl->AddRectFilled(tp, ImVec2(tp.x + kTabW, br.y - 1),
                                  IM_COL32(255, 255, 255, 8));
                dl->AddLine(ImVec2(tp.x, br.y - 1),
                            ImVec2(tp.x + kTabW, br.y - 1),
                            kNerv.acc, 1.0f);
            }

            const ImU32 idx_col = disabled ? kNerv.faint
                               : active   ? kNerv.acc : kNerv.faint;
            const ImU32 lbl_col = disabled ? kNerv.faint
                               : active   ? kNerv.acc : kNerv.dim;
            DrawTextF(dl, g_font_label, 8.0f,
                      ImVec2(tp.x + S(6), origin.y + S(11)), idx_col, t.idx);
            DrawTextF(dl, g_font_label, 9.0f,
                      ImVec2(tp.x + S(22), origin.y + S(11)), lbl_col, t.k);

            ImGui::SetCursorScreenPos(tp);
            char btn_id[16]; std::snprintf(btn_id, sizeof(btn_id),
                                            "##tb.wtab.%d", (int)t.r);
            const bool clicked = ImGui::InvisibleButton(btn_id,
                                                        ImVec2(kTabW, titlebar_h));
            if (clicked && !disabled) {
                s.route = t.r;
                s.route_entered_at = ImGui::GetTime();
                ++s.transition_seq;
            }
            dl->AddLine(ImVec2(tp.x + kTabW, origin.y + S(6)),
                        ImVec2(tp.x + kTabW, origin.y + titlebar_h - S(6)),
                        kNerv.line, 1.0f);
            x += kTabW;
        }
    } else if (s.route == ShellRoute::Hub) {
        // Hub-view tabs in the titlebar. PROFILE is icon-only (the
        // user-icon glyph) and narrower so the row fits at 1280×900
        // without bleeding into the version + peers pills on the
        // right; the rest are 60-px-wide labelled tabs.
        struct HTab { const char* k; HubView v; bool icon_only; };
        HTab tabs[] = {
            { "LOBBY",      HubView::Lobby    , false },
            { "REPLAYS",    HubView::Replays  , false },
            { "RANK",       HubView::Rankings , false },
            { "STATS",      HubView::Stats    , false },
            { "EVTS",       HubView::Events   , false },
            { ICON_FA_USER, HubView::Profile  , true  },
        };
        for (const auto& t : tabs) {
            const float kTabW = t.icon_only ? S(36.0f) : S(60.0f);
            const bool active = (s.hub_view == t.v);
            ImVec2 tp(x, origin.y);

            if (active) {
                dl->AddRectFilled(tp, ImVec2(tp.x + kTabW, br.y - 1),
                                  IM_COL32(255, 255, 255, 10));
                dl->AddLine(ImVec2(tp.x, br.y - 1),
                            ImVec2(tp.x + kTabW, br.y - 1),
                            kNerv.acc, 2.0f);
            }
            const ImU32 col = active ? kNerv.acc : kNerv.ink2;
            const float lbl_pt = t.icon_only ? 13.0f : 10.0f;
            const float pt = lbl_pt * g_ui_scale;
            ImVec2 sz = g_font_label
                ? g_font_label->CalcTextSizeA(pt, FLT_MAX, 0.0f, t.k)
                : ImGui::CalcTextSize(t.k);
            DrawTextF(dl, g_font_label, lbl_pt,
                      ImVec2(tp.x + (kTabW - sz.x) * 0.5f,
                             origin.y + (titlebar_h - sz.y) * 0.5f),
                      col, t.k);

            ImGui::SetCursorScreenPos(tp);
            char btn_id[24]; std::snprintf(btn_id, sizeof(btn_id),
                                            "##tb.htab.%d", (int)t.v);
            if (ImGui::InvisibleButton(btn_id, ImVec2(kTabW, titlebar_h))) {
                s.hub_view = t.v;
            }
            dl->AddLine(ImVec2(tp.x + kTabW, origin.y + S(6)),
                        ImVec2(tp.x + kTabW, origin.y + titlebar_h - S(6)),
                        kNerv.line, 1.0f);
            x += kTabW;
        }
    }

    // Publish where the tabs ended in LOGICAL units (subtract origin
    // because origin.x can include window letterboxing offsets in
    // theory; in practice it's window-zero but we stay correct here).
    g_titlebar_tabs_end_logical_x = (x - origin.x) / g_ui_scale;
    if (g_titlebar_tabs_end_logical_x < tabs_start_x / g_ui_scale) {
        g_titlebar_tabs_end_logical_x = tabs_start_x / g_ui_scale;
    }

    // ── RIGHT SIDE: peers count + window glyphs ─────────────
    const float kBtnW = S(26.0f);
    float right_x = br.x;

    struct WinBtn { const char* glyph; ImU32 col; int kind; };
    WinBtn wb[] = {
        { "\xc3\x97",     kNerv.dim, 2 },  // ✕  close
        { "\xe2\x96\xa1", kNerv.dim, 1 },  // □  maximize toggle
        { "\xe2\x88\x92", kNerv.dim, 0 },  // −  minimize
    };
    for (const auto& b : wb) {
        right_x -= kBtnW;
        ImVec2 bp(right_x, origin.y);
        ImGui::SetCursorScreenPos(bp);
        char btn_id[16];
        std::snprintf(btn_id, sizeof(btn_id), "##tb.win.%d", b.kind);
        const bool clicked = ImGui::InvisibleButton(btn_id,
                                                    ImVec2(kBtnW, titlebar_h));
        const bool hov = ImGui::IsItemHovered();
        if (hov) {
            const ImU32 hov_bg = (b.kind == 2)
                ? IM_COL32(0xff, 0x30, 0x30, 0x80)   // close = red
                : IM_COL32(0xff, 0xff, 0xff, 0x14);  // others = subtle
            dl->AddRectFilled(bp, ImVec2(bp.x + kBtnW, br.y - 1), hov_bg);
        }
        DrawTextF(dl, g_font_label, 11.0f,
                  ImVec2(bp.x + S(9), origin.y + S(9)),
                  hov ? kNerv.ink : b.col, b.glyph);
        if (clicked) {
            switch (b.kind) {
                case 0:  // minimize
                    if (lu.window()) SDL_MinimizeWindow(lu.window());
                    break;
                case 1: { // maximize toggle
                    SDL_Window* win = lu.window();
                    if (!win) break;
                    const SDL_WindowFlags fl = SDL_GetWindowFlags(win);
                    if (fl & SDL_WINDOW_MAXIMIZED) SDL_RestoreWindow(win);
                    else                            SDL_MaximizeWindow(win);
                    break;
                }
                case 2:  // close
                    if (lu.on_exit) lu.on_exit();
                    break;
            }
        }
    }

    // Status pills (peers + version) — sit just left of the buttons.
    // Both use the same vertical baseline; widths are measured at the
    // SCALED font size so right_x walks the visual extent, not the
    // 1× pt extent (which was the misalignment bug at high ui_scale).
    const float pill_pt = 9.0f * g_ui_scale;
    const float pill_y  = origin.y + (titlebar_h -
        (g_font_label ? g_font_label->CalcTextSizeA(pill_pt, FLT_MAX, 0.0f, "M").y
                      : ImGui::CalcTextSize("M").y)) * 0.5f;
    if (lu.hub_connected()) {
        char peers_str[24];
        std::snprintf(peers_str, sizeof(peers_str), ICON_FA_CIRCLE " %d",
                      (int)lu.hub_users().size());
        ImVec2 sz = g_font_label
            ? g_font_label->CalcTextSizeA(pill_pt, FLT_MAX, 0.0f, peers_str)
            : ImGui::CalcTextSize(peers_str);
        right_x -= sz.x + S(8);
        DrawTextF(dl, g_font_label, 9.0f,
                  ImVec2(right_x, pill_y), kNerv.phos, peers_str);
    }
    {
        const char* ver = "v0.2.18";
        ImVec2 sz = g_font_label
            ? g_font_label->CalcTextSizeA(pill_pt, FLT_MAX, 0.0f, ver)
            : ImGui::CalcTextSize(ver);
        right_x -= sz.x + S(8);
        // Skip rendering if the pill would overlap the last tab's
        // right divider — better to hide than to bleed across the
        // border line.
        const float tabs_end_x = origin.x + g_titlebar_tabs_end_logical_x * g_ui_scale;
        if (right_x > tabs_end_x + S(6)) {
            DrawTextF(dl, g_font_label, 9.0f,
                      ImVec2(right_x, pill_y), kNerv.faint, ver);
        }
    }
}

void DrawLeftRail(LauncherUI& lu, ImVec2 origin, float height,
                  const std::vector<ActiveRoom>& rooms) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float rail_w = S(kRailW);
    ImVec2 br(origin.x + rail_w, origin.y + height);

    // Flat rail background — solid bg2 (one notch up from canvas bg)
    // instead of the semi-transparent purple tint.
    dl->AddRectFilled(origin, br, kNerv.bg2);
    dl->AddLine(ImVec2(br.x, origin.y), br, kNerv.line, 1.0f);

    // Logo dot — pulsing ring only. The "F" letter was corny per
    // user feedback; just the breathing accent ring now.
    {
        ImVec2 c(origin.x + rail_w * 0.5f, origin.y + S(22));
        float t = (float)ImGui::GetTime();
        float pulse = 0.5f + 0.5f * std::sin(t * (IM_PI * 2.0f / 2.4f));
        ImU32 ring_col = MixCol(kNerv.acc, kNerv.bg2, pulse);
        dl->AddCircle(c, S(14.0f), ring_col, 24, 1.5f);
        // Tiny inner dot so the ring isn't a hollow donut at idle.
        dl->AddCircleFilled(c, S(2.0f), kNerv.acc);
    }

    // Anchor nav blocks from the bottom. Per-row advance shrunk to
    // S(30) (was S(38)) so the chip column above can fit 4 chips at
    // every common ui_scale — at scale 3 the prior reservation ate
    // most of the rail and only 1 chip showed.
    const float kBottomNavH = S(60.0f);   // 2 × S(30)
    const float kTopNavH    = S(90.0f);   // 3 × S(30)
    const float kAvatarH    = S(32.0f);
    const float top_nav_y   = origin.y + height - kAvatarH - kBottomNavH - S(8) - kTopNavH;
    const float chip_bottom = top_nav_y - S(6.0f);
    const float kOverflowH  = S(14.0f);   // "+N MORE" indicator when chips clip

    // Game chips (active rooms). 36×27 base each, vertically stacked.
    // Capped at 4-chips-visible height; overflow scrolls inside the
    // BeginChild. The window stops just above the BROWSE button so the
    // chip column never bleeds into the top nav.
    //
    // Chip rendering uses the CHILD's draw list (captured AFTER
    // BeginChild) so the clip rect actually constrains where pixels
    // can land. Using the outer rail draw list here let chips overflow
    // visually past the child's bottom edge — the bug the user hit.
    const float chip_w = S(36.0f), chip_h = S(27.0f);
    const float chip_advance = chip_h + S(3);
    const float chips_y0 = origin.y + S(50);
    const float max_chips_h = chip_advance * 4;       // 4 chips visible
    const float chips_h  = std::min(std::max(0.0f, chip_bottom - chips_y0),
                                    max_chips_h);
    ImGui::SetCursorScreenPos(ImVec2(origin.x, chips_y0));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0,0,0,0));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, IM_COL32(0,0,0,0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    // Show the scrollbar so users have an obvious affordance when
    // their subscription list overflows 4 chips. ScrollbarSize is
    // pushed small so it doesn't dominate the 48-px rail.
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, S(6.0f));
    ImGui::BeginChild("##rail.chips",
                      ImVec2(rail_w, chips_h),
                      false,
                      ImGuiWindowFlags_NoBackground);
    ImDrawList* cdl = ImGui::GetWindowDrawList();   // child-clipped
    float y = chips_y0;
    auto& s = State();
    for (int i = 0; i < (int)rooms.size(); ++i) {
        const auto& r = rooms[i];

        ImVec2 cp = ImGui::GetCursorScreenPos();
        cp.x += S(6);
        ImVec2 cb(cp.x + chip_w, cp.y + chip_h);
        y = cp.y;
        // Flat solid fill — pick the primary tint, no gradient mix.
        cdl->AddRectFilled(cp, cb, r.tint.a);

        const bool active = (i == s.active_room_index);
        cdl->AddRect(cp, cb, active ? kNerv.acc : kNerv.line);
        if (active) {
            cdl->AddRectFilled(ImVec2(cp.x - S(2), cp.y + S(4)),
                               ImVec2(cp.x,        cb.y - S(4)),
                               kNerv.acc);
        }

        if (g_font_label && !r.label.empty()) {
            // Silkscreen native is 8px; 9px lands at the nearest crisp
            // bake. UTF-8-safe trunc to 4 bytes so JP/KR ids don't
            // garble.
            std::string lab = Utf8SafeTrunc(r.label, 4);
            const float pt = 9.0f * g_ui_scale;
            ImVec2 sz = g_font_label->CalcTextSizeA(pt, FLT_MAX, 0.0f, lab.c_str());
            ImVec2 tp(cp.x + (chip_w - sz.x) * 0.5f,
                      cp.y + (chip_h - sz.y) * 0.5f);
            DrawTextF(cdl, g_font_label, 9.0f,
                      ImVec2(tp.x + S(1), tp.y + S(1)),
                      IM_COL32(0,0,0,210), lab.c_str());
            DrawTextF(cdl, g_font_label, 9.0f, tp,
                      IM_COL32(255,255,255,255), lab.c_str());
        }

        ImGui::SetCursorScreenPos(cp);
        char btn_id[32]; std::snprintf(btn_id, sizeof(btn_id), "##rail.room.%d", i);
        if (ImGui::InvisibleButton(btn_id, ImVec2(chip_w, chip_h))) {
            s.active_room_index = i;
            lu.SetActiveRoom(r.game_id, r.game_id);
            s.hub_view = HubView::Lobby;
        }
        if (ImGui::IsItemHovered() && r.info) {
            ImGui::SetTooltip("%s\n%s", r.info->GetExeName().c_str(),
                              r.game_id.c_str());
        }
        // Advance with Dummy so BeginChild tracks the chip column's
        // total height + enables mousewheel scroll when overflowing.
        ImGui::Dummy(ImVec2(0, S(3)));  // 27 chip + 3 gap = 30 advance
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(2);   // WindowPadding + ScrollbarSize
    ImGui::PopStyleColor(2);

    // ── nav icons (browse / rank / events) ──
    struct Nav { const char* glyph; const char* label; HubView view; };
    Nav navs[] = {
        { ICON_FA_LIST,    "browse", HubView::Browse   },
        { ICON_FA_TROPHY,  "rank",   HubView::Rankings },
        { ICON_FA_CALENDAR,"evts",   HubView::Events   },
    };

    auto draw_nav = [&](const Nav& n, float ny) {
        const bool active = (s.hub_view == n.view);
        // Center the icon + label horizontally inside the rail.
        const float slot_x = origin.x + S(0);
        const float slot_w = rail_w;
        if (active) {
            dl->AddRectFilled(ImVec2(origin.x, ny),
                              ImVec2(origin.x + S(2), ny + S(24)),
                              kNerv.acc);
        }
        ImU32 col = active ? kNerv.acc : kNerv.faint;
        if (g_font_body) {
            const float pt = 17.0f * g_ui_scale;
            ImVec2 sz = g_font_body->CalcTextSizeA(pt, FLT_MAX, 0.0f, n.glyph);
            DrawTextF(dl, g_font_body, 17.0f,
                      ImVec2(slot_x + (slot_w - sz.x) * 0.5f, ny + 0),
                      col, n.glyph);
        }
        if (g_font_micro) {
            const float pt = 9.0f * g_ui_scale;
            ImVec2 sz = g_font_micro->CalcTextSizeA(pt, FLT_MAX, 0.0f, n.label);
            DrawTextF(dl, g_font_micro, 9.0f,
                      ImVec2(slot_x + (slot_w - sz.x) * 0.5f, ny + S(20)),
                      col, n.label);
        }
        ImGui::SetCursorScreenPos(ImVec2(slot_x, ny));
        char btn_id[32]; std::snprintf(btn_id, sizeof(btn_id), "##rail.nav.%d", (int)n.view);
        if (ImGui::InvisibleButton(btn_id, ImVec2(slot_w, S(32)))) {
            s.hub_view = n.view;
        }
    };

    // (Used to be a "+N MORE" badge here — chip column is scrollable
    // now via BeginChild, so all subscribed rooms are reachable.)
    (void)y;

    // Top nav advance. S(30) is the compromise that lets a 4-chip
    // column fit at scale 3 windows (1920×1080) while still leaving
    // enough vertical space for icon + label.
    const float kNavRowH = S(30);
    {
        float ny = top_nav_y;
        for (const auto& n : navs) { draw_nav(n, ny); ny += kNavRowH; }
    }

    // Bottom-anchored: avatar + bottom nav.
    Nav navs_bottom[] = {
        { ICON_FA_GAMEPAD, "ctrl",   HubView::Map    },
        { ICON_FA_GEAR,    "cfg",    HubView::Config },
    };
    const float kAv = S(32.0f);
    float by = origin.y + height - S(36);  // avatar slot
    {
        ImVec2 av(origin.x + S(8), by);
        // Flat avatar — solid bg2 with accent border instead of the
        // purple→orange gradient block.
        dl->AddRectFilled(av, ImVec2(av.x + kAv, av.y + kAv), kNerv.bg2);
        dl->AddRect(av, ImVec2(av.x + kAv, av.y + kAv), kNerv.acc);
        // First letter of nick (or '?' if not signed in)
        const std::string& nick = lu.discord_signed_in() ? lu.discord_nick()
                                                         : lu.hub_my_nick();
        char init[2] = { nick.empty() ? '?' : (char)std::toupper((unsigned char)nick[0]), '\0' };
        if (g_font_display) {
            const float pt = 18.0f * g_ui_scale;
            ImVec2 sz = g_font_display->CalcTextSizeA(pt, FLT_MAX, 0.0f, init);
            DrawTextF(dl, g_font_display, 18.0f,
                      ImVec2(av.x + (kAv - sz.x) * 0.5f,
                             av.y + (kAv - sz.y) * 0.5f),
                      IM_COL32(255,255,255,255), init);
        }
        // Online dot — phos when hub-connected, dim when offline.
        ImU32 dot_col = lu.hub_connected() ? kNerv.phos : kNerv.faint;
        dl->AddCircleFilled(ImVec2(av.x + S(30), av.y + S(30)), S(4.0f), dot_col);
        dl->AddCircle      (ImVec2(av.x + S(30), av.y + S(30)), S(4.0f), IM_COL32(0,0,0,255));
    }

    // Same vertical advance as the top nav so the two clusters look
    // visually consistent (was S(28) — too cramped).
    float by2 = by - kNavRowH * (int)(sizeof(navs_bottom) / sizeof(navs_bottom[0]));
    for (const auto& n : navs_bottom) { draw_nav(n, by2); by2 += kNavRowH; }
}

// ─── TOP BAR ─────────────────────────────────────────────────────────
void DrawTopBar(LauncherUI& lu, ImVec2 origin, float width,
                const std::vector<ActiveRoom>& rooms) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float top_h = S(kTopH);
    ImVec2 br(origin.x + width, origin.y + top_h);
    // Flat — solid bg2 to match the rail; was semi-transparent black.
    dl->AddRectFilled(origin, br, kNerv.bg2);
    dl->AddLine(ImVec2(origin.x, br.y), ImVec2(br.x, br.y), kNerv.line, 1.0f);

    auto& s = State();
    const ActiveRoom* room = nullptr;
    if (s.active_room_index >= 0 && s.active_room_index < (int)rooms.size()) {
        room = &rooms[s.active_room_index];
    }

    float x = origin.x + S(8);
    float ax = br.x - S(8);

    // Room context (game eyebrow + view tabs + TEST/TRAIN/BOOT) stays
    // visible across every hub view. The actively-selected room is
    // independent of which sub-view (Replays / Stats / etc.) you're
    // poking at, and TEST/TRAIN/BOOT need to be one click away no
    // matter what tab the user wandered into.
    const bool show_room_chrome = true;

    // Game eyebrow + short name (or "NO GAME SELECTED" placeholder)
    if (room) {
        if (g_font_micro) {
            DrawTextF(dl, g_font_micro, 9.0f,
                      ImVec2(x, origin.y + S(6)), kNerv.acc,
                      room->game_id.c_str());
        }
        if (g_font_body && room->info) {
            DrawTextF(dl, g_font_body, 13.0f,
                      ImVec2(x + S(60), origin.y + S(8)), kNerv.ink,
                      room->info->GetExeName().c_str());
        }
    } else {
        if (g_font_micro) {
            DrawTextF(dl, g_font_micro, 9.0f,
                      ImVec2(x, origin.y + S(11)), kNerv.dim,
                      "NO GAME SELECTED");
        }
    }
    if (show_room_chrome) {
        dl->AddLine(ImVec2(x + S(220), origin.y + S(6)),
                    ImVec2(x + S(220), origin.y + top_h - S(6)),
                    kNerv.line, 1.0f);
    }

    if (show_room_chrome) {
        // Hub-view tabs moved into the titlebar (DrawShellTitlebar);
        // TopBar keeps just game-context + TEST/TRAIN/BOOT now.

        // Right-side action buttons (◐ TEST / ▤ TRAIN / ▶ BOOT)
        struct ABtn { const char* label; bool accent; };
        ABtn abtns[] = {
            { ICON_FA_FLASK         " TEST",  false },
            { ICON_FA_GRADUATION_CAP " TRAIN", false },
            { ICON_FA_PLAY          " BOOT",  true  },
        };
        for (int i = (int)(sizeof(abtns) / sizeof(abtns[0])) - 1; i >= 0; --i) {
            const auto& b = abtns[i];
            const float pt = 9.0f * g_ui_scale;
            ImVec2 sz = g_font_label
                ? g_font_label->CalcTextSizeA(pt, FLT_MAX, 0.0f, b.label)
                : ImGui::CalcTextSize(b.label);
            // Make the button a bit taller and vertically center the
            // label/icon inside it. Was too cramped + glyph baselines
            // didn't match the text baseline (icons read as "high").
            const float btn_h = S(18);
            ax -= sz.x + S(10);
            ImVec2 bp(ax, origin.y + (top_h - btn_h) * 0.5f);
            ImVec2 b1(bp.x + sz.x + S(6), bp.y + btn_h);
            ImU32 border = b.accent ? kNerv.acc : kNerv.line;
            ImU32 fg     = b.accent ? kNerv.acc : kNerv.ink2;
            if (b.accent) dl->AddRectFilled(bp, b1, IM_COL32(0xff,0x5a,0x3c,0x14));
            dl->AddRect(bp, b1, border);
            DrawTextF(dl, g_font_label, 9.0f,
                      ImVec2(bp.x + S(3), bp.y + (btn_h - sz.y) * 0.5f),
                      fg, b.label);
            ax -= S(4);
        }
    }

    // Gear icon — small popup menu with re-run wizard + Settings + Quit.
    // Placed left of the action buttons so it doesn't move when the
    // wider TEST / TRAIN / BOOT chrome resizes.
    const char* gear_glyph = ICON_FA_GEAR;
    {
        const float pt = 16.0f * g_ui_scale;
        ImVec2 sz = g_font_body
            ? g_font_body->CalcTextSizeA(pt, FLT_MAX, 0.0f, gear_glyph)
            : ImGui::CalcTextSize(gear_glyph);
        ax -= sz.x + S(10);
        ImVec2 bp(ax, origin.y + S(6));
        ImGui::SetCursorScreenPos(bp);
        ImGui::InvisibleButton("##topbar.gear",
                               ImVec2(sz.x + S(6), top_h - S(12)));
        const bool hov = ImGui::IsItemHovered();
        if (hov) {
            dl->AddRectFilled(bp, ImVec2(bp.x + sz.x + S(6), bp.y + top_h - S(12)),
                              IM_COL32(0xff,0x6a,0x00,0x14));
        }
        DrawTextF(dl, g_font_body, 16.0f,
                  ImVec2(bp.x + S(3), bp.y),
                  hov ? kNerv.acc : kNerv.ink2, gear_glyph);
        if (ImGui::IsItemClicked()) {
            ImGui::OpenPopup("##topbar.gear.menu");
        }
        ax -= S(4);
    }

    // Popup body — checked every frame; ImGui shows it only when open.
    ImGui::PushStyleColor(ImGuiCol_PopupBg, IM_COL32(0x0f, 0x0c, 0x12, 0xfa));
    ImGui::PushStyleColor(ImGuiCol_Border, kNerv.acc);
    ImGui::PushStyleColor(ImGuiCol_Text,
                          ImGui::ColorConvertU32ToFloat4(kNerv.ink));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 6));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1.0f);
    if (ImGui::BeginPopup("##topbar.gear.menu")) {
        if (ImGui::MenuItem("Re-run setup wizard")) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "shell: gear -> re-run wizard "
                        "(route Setup step 0; setup_version stays %d)",
                        s.setup_version);
            s.route = ShellRoute::Setup;
            s.setup_step = 0;
            s.route_entered_at = ImGui::GetTime();
            ++s.transition_seq;
        }
        if (ImGui::MenuItem("Settings\xe2\x80\xa6")) {
            s.hub_view = HubView::Config;
        }
        if (ImGui::MenuItem("Sign out")) {
            // Drop cached Discord auth + disconnect from hub. The
            // SIGN IN affordance in the StatusBar surfaces again.
            fm2k::discord_auth::ClearCached();
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "shell: gear -> sign out (Discord cache cleared)");
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Quit")) {
            if (lu.on_exit) lu.on_exit();
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(3);
}

// ─── RIGHT ROSTER ────────────────────────────────────────────────────
// Real users from lu.hub_users(). Sorted by ping ascending so low-ping
// peers float to the top — matches Fightcade IRC convention.
//
// IN MATCH list is filtered to the *active room* — when sitting in
// pkmncc, you don't want to see matches from other rooms. Hub-side
// hub_users is already room-scoped (see RoomJoined handler in
// FM2K_LauncherUI.cpp), but hub_current_matches is global so we filter
// here.
void DrawRightRoster(LauncherUI& lu, ImVec2 origin, ImVec2 size,
                     const std::vector<ActiveRoom>& rooms) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 br(origin.x + size.x, origin.y + size.y);
    // Flat solid bg2 for the roster panel.
    dl->AddRectFilled(origin, br, kNerv.bg2);
    dl->AddLine(origin, ImVec2(origin.x, br.y), kNerv.line, 1.0f);

    auto& s = State();
    const ActiveRoom* room = nullptr;
    if (s.active_room_index >= 0 && s.active_room_index < (int)rooms.size()) {
        room = &rooms[s.active_room_index];
    }
    const std::string active_gid = room ? room->game_id : std::string();

    const auto& all_matches = lu.hub_current_matches();
    const auto& users       = lu.hub_users();

    // Build filtered match index list — empty active_gid means "show
    // nothing" (forces the user to pick a room before seeing matches).
    std::vector<size_t> match_idx;
    match_idx.reserve(all_matches.size());
    for (size_t i = 0; i < all_matches.size(); ++i) {
        if (!active_gid.empty() && all_matches[i].game_id == active_gid) {
            match_idx.push_back(i);
        }
    }

    // Header — IN MATCH count
    {
        ImVec2 hb(br.x, origin.y + S(16));
        dl->AddRectFilled(origin, hb, IM_COL32(0xff,0x5a,0x3c,0x10));
        dl->AddLine(ImVec2(origin.x, hb.y), ImVec2(br.x, hb.y), kNerv.line, 1.0f);
        if (g_font_micro) {
            DrawTextF(dl, g_font_micro, 8.0f,
                      ImVec2(origin.x + S(6), origin.y + S(4)),
                      kNerv.acc, ICON_FA_CIRCLE_DOT " IN MATCH");
        }
        char num[8]; std::snprintf(num, sizeof(num), "%d", (int)match_idx.size());
        if (g_font_body) {
            const float pt = 11.0f * g_ui_scale;
            ImVec2 sz = g_font_body->CalcTextSizeA(pt, FLT_MAX, 0.0f, num);
            DrawTextF(dl, g_font_body, 11.0f,
                      ImVec2(br.x - sz.x - S(6), origin.y + S(2)),
                      kNerv.acc, num);
        }
    }

    const float row_h = S(22);
    float y = origin.y + S(18);
    for (size_t k = 0; k < match_idx.size(); ++k) {
        const size_t mi = match_idx[k];
        const auto& m = all_matches[mi];
        if (y + row_h > br.y) break;

        ImVec2 mp(origin.x, y);
        ImVec2 m1(br.x, y + row_h);
        ImGui::SetCursorScreenPos(mp);
        char btn_id[32]; std::snprintf(btn_id, sizeof(btn_id), "##match.%zu", mi);
        ImGui::InvisibleButton(btn_id, ImVec2(size.x, row_h));
        if (ImGui::IsItemHovered()) {
            g_match_hover.match_idx  = (int)mi;
            g_match_hover.anchor_pos =
                ImVec2(ImGui::GetItemRectMin().x - S(244),
                       ImGui::GetItemRectMin().y);
            g_match_hover.last_seen  = ImGui::GetTime();
            dl->AddRectFilled(mp, m1, IM_COL32(0xff,0x6a,0x00,0x10));
            dl->AddRectFilled(mp, ImVec2(mp.x + S(2), m1.y), kNerv.acc);
        }

        // Row 1: nickA vs nickB
        char buf[96];
        std::snprintf(buf, sizeof(buf), "%s vs %s",
                      m.p1_nick.c_str(), m.p2_nick.c_str());
        DrawTextF(dl, g_font_label, 9.0f,
                  ImVec2(origin.x + S(4), y), kNerv.ink, buf);
        std::string sub;
        if (!m.p1_char_name.empty() && !m.p2_char_name.empty()) {
            sub = m.p1_char_name + " \xc2\xb7 " + m.p2_char_name;
            if (!m.stage_name.empty()) {
                sub += " \xc2\xb7 " + m.stage_name;
            }
        } else {
            sub = m.game_id;
        }
        DrawTextF(dl, g_font_label, 9.0f,
                  ImVec2(origin.x + S(4), y + S(10)), kNerv.dim, sub.c_str());
        y += row_h;
    }

    // LOOKING TO PLAY header
    {
        ImVec2 hp(origin.x, y + S(4));
        ImVec2 hb(br.x, hp.y + S(14));
        dl->AddLine(ImVec2(hp.x, hp.y), ImVec2(br.x, hp.y), kNerv.line, 1.0f);
        if (g_font_micro) {
            DrawTextF(dl, g_font_micro, 8.0f,
                      ImVec2(hp.x + S(6), hp.y + S(3)),
                      kNerv.dim, "LOOKING TO PLAY");
        }
        char num[8]; std::snprintf(num, sizeof(num), "%d", (int)users.size());
        if (g_font_body) {
            const float pt = 11.0f * g_ui_scale;
            ImVec2 sz = g_font_body->CalcTextSizeA(pt, FLT_MAX, 0.0f, num);
            DrawTextF(dl, g_font_body, 11.0f,
                      ImVec2(br.x - sz.x - S(6), hp.y + S(1)),
                      kNerv.acc, num);
        }
        dl->AddLine(ImVec2(hp.x, hb.y), ImVec2(br.x, hb.y), kNerv.line, 1.0f);
        y = hb.y + 1;
    }

    // Sort users by ping ascending. Copy to a local vec so we don't
    // touch hub state.
    std::vector<const fm2k::HubUser*> sorted;
    sorted.reserve(users.size());
    for (const auto& kv : users) {
        if (lu.IsUserIgnored(kv.first)) continue;
        sorted.push_back(&kv.second);
    }
    std::sort(sorted.begin(), sorted.end(),
              [](const fm2k::HubUser* a, const fm2k::HubUser* b) {
                  // Treat 0/negative as "unknown, sort last"
                  int ar = a->rtt_ms > 0 ? a->rtt_ms : 9999;
                  int bp = b->rtt_ms > 0 ? b->rtt_ms : 9999;
                  return ar < bp;
              });

    int row = 0;
    const float urow_h = S(14);
    for (const fm2k::HubUser* u : sorted) {
        if (y + urow_h > br.y) break;
        ImVec2 rp(origin.x, y);
        ImVec2 r1(br.x, y + urow_h);
        ImGui::SetCursorScreenPos(rp);
        char btn_id[32]; std::snprintf(btn_id, sizeof(btn_id), "##usr.%d", row);
        ImGui::InvisibleButton(btn_id, ImVec2(size.x, urow_h));
        const bool hov = ImGui::IsItemHovered();
        if (hov) {
            dl->AddRectFilled(rp, r1, IM_COL32(0xff,0x5a,0x3c,0x14));
            dl->AddRectFilled(rp, ImVec2(rp.x + S(2), r1.y), kNerv.acc);
        }
        if (ImGui::IsItemClicked()) {
            g_user_menu.user_id   = u->id;
            g_user_menu.user_nick = u->nick;
            g_user_menu.user_in_match = (u->status == "in_match");
            g_user_menu.anchor_pos =
                ImVec2(ImGui::GetItemRectMax().x - S(144),
                       ImGui::GetItemRectMax().y);
            ImGui::OpenPopup(kUserMenuPopupId);
        }
        const bool in_match = (u->status == "in_match");
        ImU32 name_col = in_match ? kNerv.faint : kNerv.ink;
        DrawTextF(dl, g_font_label, 9.0f,
                  ImVec2(rp.x + S(6), rp.y + S(2)),
                  name_col, u->nick.c_str());
        if (u->rtt_ms > 0 && size.x > S(100)) {
            DrawPingWave(dl, ImVec2(br.x - S(60), rp.y + S(3)), u->rtt_ms);
        }
        const char* sg = in_match ? ICON_FA_CIRCLE_DOT : ICON_FA_CIRCLE;
        ImU32 sc = in_match ? kNerv.acc : kNerv.phos;
        DrawTextF(dl, g_font_label, 9.0f,
                  ImVec2(br.x - S(14), rp.y + S(2)), sc, sg);
        y += urow_h;
        ++row;
    }
}

// ─── STATUS BAR ──────────────────────────────────────────────────────
void DrawStatusBar(LauncherUI& lu, ImVec2 origin, float width) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float status_h = S(kStatusH);
    ImVec2 br(origin.x + width, origin.y + status_h);
    // Flat solid bg2.
    dl->AddRectFilled(origin, br, kNerv.bg2);
    dl->AddLine(origin, ImVec2(br.x, origin.y), kNerv.line, 1.0f);

    // Walk left-to-right with measured widths so segments stop overlapping
    // when the nick is long. fs is the design pt size (DrawTextF auto-
    // scales via g_ui_scale); fs_px is the visual pixel size for layout
    // measurements.
    const float fs    = 9.0f;
    const float fs_px = fs * g_ui_scale;
    const float y  = origin.y + S(4);
    float       cx = origin.x + S(6);

    auto measure = [&](const char* s) -> float {
        return g_font_label ? g_font_label->CalcTextSizeA(fs_px, FLT_MAX, 0.0f, s).x
                            : ImGui::CalcTextSize(s).x;
    };
    auto seg = [&](ImU32 col, const char* s) {
        DrawTextF(dl, g_font_label, fs, ImVec2(cx, y), col, s);
        cx += measure(s) + S(10.0f);
    };

    const bool connected = lu.hub_connected();

    // ●  +  ONLINE/OFFLINE
    seg(connected ? kNerv.phos : kNerv.faint, ICON_FA_CIRCLE);
    seg(connected ? kNerv.faint : kNerv.dim,
        connected ? "ONLINE \xe2\x80\xa2 HUB" : "OFFLINE");

    // SIGN IN affordance — click reopens the OAuth modal.
    if (!connected && !lu.discord_signed_in()) {
        const char* signin = "[ SIGN IN ]";
        ImVec2 sp(cx, y);
        float w = measure(signin);
        ImGui::SetCursorScreenPos(ImVec2(sp.x - S(2), sp.y - S(1)));
        ImGui::InvisibleButton("##shell.signin", ImVec2(w + S(4), fs_px + S(2)));
        const bool hov = ImGui::IsItemHovered();
        if (hov) {
            dl->AddRectFilled(ImVec2(sp.x - S(2), sp.y - S(1)),
                              ImVec2(sp.x + w + S(2), sp.y + fs_px + S(1)),
                              IM_COL32(0xff,0x6a,0x00,0x18));
        }
        DrawTextF(dl, g_font_label, fs, sp,
                  hov ? kNerv.acc : kNerv.amber, signin);
        if (ImGui::IsItemClicked()) {
            // Route to LoginV2 instead of popping the legacy modal —
            // the inline OAuth flow there does not use OpenDiscordAuth.
            auto& s = State();
            s.route = ShellRoute::Login;
            s.route_entered_at = ImGui::GetTime();
            ++s.transition_seq;
        }
        cx += w + S(10.0f);
    }

    if (connected) {
        const std::string& nick = lu.hub_my_nick();
        if (!nick.empty()) seg(kNerv.ink2, nick.c_str());

        const int wins = lu.my_wins(), losses = lu.my_losses(), draws = lu.my_draws();
        if (wins >= 0) {
            char wld[32];
            std::snprintf(wld, sizeof(wld), "%d-%d-%d", wins, losses, draws);
            seg(kNerv.ink, wld);
        }
        char roster_n[32];
        std::snprintf(roster_n, sizeof(roster_n), "ROSTER %d",
                      (int)lu.hub_users().size());
        seg(kNerv.faint, roster_n);
    }

    // Right-aligned build label.
    const char* build = "FM2K_ROLLBACK \xe2\x80\xa2 build";
    float bw = measure(build);
    DrawTextF(dl, g_font_label, fs,
              ImVec2(br.x - bw - S(6), y), kNerv.faint, build);
}

void RenderUserMiniMenu(LauncherUI& lu) {
    if (!ImGui::IsPopupOpen(kUserMenuPopupId)) return;

    ImGui::SetNextWindowPos(g_user_menu.anchor_pos, ImGuiCond_Appearing);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, IM_COL32(0x0a, 0x06, 0x12, 0xfa));
    ImGui::PushStyleColor(ImGuiCol_Border,  kNerv.acc);
    ImGui::PushStyleColor(ImGuiCol_Text,
                          ImGui::ColorConvertU32ToFloat4(kNerv.ink));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1.0f);

    if (ImGui::BeginPopup(kUserMenuPopupId,
                          ImGuiWindowFlags_NoMove |
                          ImGuiWindowFlags_AlwaysAutoResize |
                          ImGuiWindowFlags_NoSavedSettings)) {
        // Header band — nick + room context
        ImGui::PushStyleColor(ImGuiCol_ChildBg,
                              IM_COL32(0xff,0x5a,0x3c,0x14));
        ImGui::BeginChild("##user.menu.head",
                          ImVec2(140.0f, 22.0f), 0,
                          ImGuiWindowFlags_NoScrollbar);
        ImGui::SetCursorPos(ImVec2(8, 4));
        ImGui::PushStyleColor(ImGuiCol_Text,
                              ImGui::ColorConvertU32ToFloat4(kNerv.acc));
        ImGui::Text("%s", g_user_menu.user_nick.c_str());
        ImGui::PopStyleColor();
        ImGui::EndChild();
        ImGui::PopStyleColor();

        struct Item {
            const char* glyph; const char* label; ImU32 col;
            int kind;  // 0=challenge 1=spectate 2=dm 3=profile 4=ignore
        };
        const bool spectate_ok = g_user_menu.user_in_match;
        const bool ignored     = lu.IsUserIgnored(g_user_menu.user_id);
        const Item items[] = {
            { ICON_FA_BOLT,     "CHALLENGE",                   kNerv.acc,  0 },
            { ICON_FA_EYE,      "SPECTATE",                    kNerv.phos, 1 },
            { ICON_FA_ENVELOPE, "DIRECT MSG",                  kNerv.ink,  2 },
            { ICON_FA_USER,     "PROFILE",                     kNerv.ink2, 3 },
            { ICON_FA_BAN,      ignored ? "UNIGNORE" : "IGNORE",
              ignored ? kNerv.amber : kNerv.dim,                            4 },
        };
        for (const auto& it : items) {
            const bool disabled = (it.kind == 1 && !spectate_ok);
            if (disabled) ImGui::BeginDisabled();
            ImGui::PushStyleColor(ImGuiCol_Text,
                                  ImGui::ColorConvertU32ToFloat4(it.col));
            char buf[64];
            std::snprintf(buf, sizeof(buf), " %s  %s", it.glyph, it.label);
            const bool clicked = ImGui::Selectable(buf, false,
                                                   ImGuiSelectableFlags_None,
                                                   ImVec2(140.0f - 0, 16.0f));
            ImGui::PopStyleColor();
            if (disabled) ImGui::EndDisabled();
            if (clicked && !disabled) {
                switch (it.kind) {
                case 0:  // CHALLENGE
                    lu.ChallengeUser(g_user_menu.user_id,
                                     g_user_menu.user_nick);
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                "shell: user-menu CHALLENGE %s",
                                g_user_menu.user_nick.c_str());
                    break;
                case 1:  // SPECTATE — only enabled when target is in_match
                    lu.SpectateUser(g_user_menu.user_id);
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                "shell: user-menu SPECTATE %s",
                                g_user_menu.user_nick.c_str());
                    break;
                case 2:  // DIRECT MSG — out of v2 scope (no hub support yet)
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                "shell: user-menu DM %s "
                                "(no hub DM channel — chat is room-scoped)",
                                g_user_menu.user_nick.c_str());
                    break;
                case 3:  // PROFILE — switch hub view
                    State().hub_view = HubView::Profile;
                    break;
                case 4:  // IGNORE — local per-session
                    if (lu.IsUserIgnored(g_user_menu.user_id)) {
                        lu.UnignoreUser(g_user_menu.user_id);
                    } else {
                        lu.IgnoreUser(g_user_menu.user_id);
                    }
                    break;
                }
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    }

    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(3);
}

// ─── CHALLENGE MODALS ────────────────────────────────────────────────
// Two click-blocking modals driven by LauncherUI's challenge state:
// one for incoming (someone challenged us), one for outgoing (we
// challenged someone, waiting for their response). Modals bidirectionally
// sync with hub state — hub-cleared flags close us; user actions clear
// the hub flag through the LauncherUI accessors.

void RenderChallengeIncomingModal(LauncherUI& lu) {
    constexpr const char* id = "##challenge.incoming";
    const bool pending = lu.incoming_challenge_pending();
    if (pending && !ImGui::IsPopupOpen(id)) {
        ImGui::OpenPopup(id);
    }

    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, kNerv.bg);
    ImGui::PushStyleColor(ImGuiCol_Border,  kNerv.acc);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20, 16));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1.0f);

    if (ImGui::BeginPopupModal(id, nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize |
                               ImGuiWindowFlags_NoTitleBar |
                               ImGuiWindowFlags_NoSavedSettings)) {
        if (!pending) {
            // Hub side cancelled the challenge — close immediately.
            ImGui::CloseCurrentPopup();
        } else {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            DrawTextF(dl, g_font_micro, 9.0f,
                      ImGui::GetCursorScreenPos(), kNerv.acc,
                      "INCOMING CHALLENGE");
            ImGui::Dummy(ImVec2(0, 14));
            DrawTextF(dl, g_font_display, 24.0f,
                      ImGui::GetCursorScreenPos(), kNerv.ink,
                      lu.incoming_challenger_nick().c_str());
            ImGui::Dummy(ImVec2(0, 28));
            DrawTextF(dl, g_font_label, 11.0f,
                      ImGui::GetCursorScreenPos(), kNerv.ink2,
                      "wants to play.");
            ImGui::Dummy(ImVec2(0, 16));
            ImGui::Dummy(ImVec2(0, 4));

            // Buttons
            ImGui::PushStyleColor(ImGuiCol_Button,
                                  IM_COL32(0xff,0x6a,0x00,0x40));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                                  IM_COL32(0xff,0x6a,0x00,0x80));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                                  IM_COL32(0xff,0x6a,0x00,0xc0));
            if (ImGui::Button("ACCEPT", ImVec2(120, 28))) {
                lu.AcceptIncomingChallenge();
                ImGui::CloseCurrentPopup();
            }
            ImGui::PopStyleColor(3);
            ImGui::SameLine();
            if (ImGui::Button("DECLINE", ImVec2(120, 28))) {
                lu.DeclineIncomingChallenge();
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
}

void RenderChallengeOutgoingModal(LauncherUI& lu) {
    constexpr const char* id = "##challenge.outgoing";
    const bool pending = lu.outgoing_challenge_pending();
    if (pending && !ImGui::IsPopupOpen(id)) {
        ImGui::OpenPopup(id);
    }

    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, kNerv.bg);
    ImGui::PushStyleColor(ImGuiCol_Border,  kNerv.amber);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20, 16));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1.0f);

    if (ImGui::BeginPopupModal(id, nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize |
                               ImGuiWindowFlags_NoTitleBar |
                               ImGuiWindowFlags_NoSavedSettings)) {
        if (!pending) {
            // Hub event closed the challenge (Failed/Cancelled/Declined/MatchStart).
            ImGui::CloseCurrentPopup();
        } else {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            DrawTextF(dl, g_font_micro, 9.0f,
                      ImGui::GetCursorScreenPos(), kNerv.amber,
                      "WAITING FOR RESPONSE\xe2\x80\xa6");
            ImGui::Dummy(ImVec2(0, 14));
            DrawTextF(dl, g_font_display, 24.0f,
                      ImGui::GetCursorScreenPos(), kNerv.ink,
                      lu.outgoing_challenge_nick().c_str());
            ImGui::Dummy(ImVec2(0, 28));
            DrawTextF(dl, g_font_label, 11.0f,
                      ImGui::GetCursorScreenPos(), kNerv.ink2,
                      "challenge sent. Hub may take a few seconds.");
            ImGui::Dummy(ImVec2(0, 18));

            if (ImGui::Button("CANCEL", ImVec2(120, 28))) {
                lu.CancelOutgoingChallenge();
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
}

// ─── MATCH HOVER CARD ────────────────────────────────────────────────
// Anchored card that pops up to the LEFT of the right-rail IN MATCH row
// being hovered. Shows full match details — players, char names, stage,
// game id. Driven by hover state set inside DrawRightRoster.
//
// Renders to the foreground draw list so it always sits above every
// other window's content (lobby chrome, chat, modals not in dialog
// mode, etc). Earlier versions used ImGui::Begin which was beaten in
// z-order whenever the parent window claimed focus.
void RenderMatchHoverCard(LauncherUI& lu) {
    const double now = ImGui::GetTime();
    if (g_match_hover.match_idx < 0 ||
        now - g_match_hover.last_seen > kMatchHoverGraceSec) {
        return;
    }
    const auto& matches = lu.hub_current_matches();
    if (g_match_hover.match_idx >= (int)matches.size()) return;
    const auto& m = matches[g_match_hover.match_idx];

    constexpr float W = 240.0f, H = 130.0f;
    ImVec2 anchor = g_match_hover.anchor_pos;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    // Clamp to viewport edges.
    if (anchor.x < vp->WorkPos.x + 4) anchor.x = vp->WorkPos.x + 4;
    if (anchor.y + H > vp->WorkPos.y + vp->WorkSize.y - 4) {
        anchor.y = vp->WorkPos.y + vp->WorkSize.y - H - 4;
    }

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImVec2 wmin = anchor;
    ImVec2 wmax(anchor.x + W, anchor.y + H);
    constexpr float kPadX = 10.0f, kPadY = 8.0f;
    ImVec2 p(wmin.x + kPadX, wmin.y + kPadY);

    // Card background + 1px border in accent.
    dl->AddRectFilled(wmin, wmax, IM_COL32(0x0a, 0x06, 0x12, 0xfa));
    dl->AddRect(wmin, wmax, kNerv.acc, 0.0f, 0, 1.0f);

    // Header — eyebrow + game_id right-aligned
    DrawTextF(dl, g_font_micro, 9.0f,
              ImVec2(p.x, p.y), kNerv.acc, ICON_FA_CIRCLE_DOT " LIVE MATCH");
    if (g_font_micro && !m.game_id.empty()) {
        ImVec2 sz = g_font_micro->CalcTextSizeA(9.0f, FLT_MAX, 0.0f,
                                                 m.game_id.c_str());
        DrawTextF(dl, g_font_micro, 9.0f,
                  ImVec2(wmax.x - kPadX - sz.x, p.y),
                  kNerv.dim, m.game_id.c_str());
    }
    dl->AddLine(ImVec2(p.x, p.y + 14),
                ImVec2(wmax.x - kPadX, p.y + 14), kNerv.line, 1.0f);

    // P1 / P2 block
    DrawTextF(dl, g_font_label, 11.0f,
              ImVec2(p.x, p.y + 22), kNerv.ink, m.p1_nick.c_str());
    if (!m.p1_char_name.empty()) {
        DrawTextF(dl, g_font_label, 9.0f,
                  ImVec2(p.x + 4, p.y + 36), kNerv.acc,
                  m.p1_char_name.c_str());
    }
    DrawTextF(dl, g_font_label, 9.0f,
              ImVec2(p.x, p.y + 52), kNerv.faint, "vs");
    DrawTextF(dl, g_font_label, 11.0f,
              ImVec2(p.x, p.y + 64), kNerv.ink, m.p2_nick.c_str());
    if (!m.p2_char_name.empty()) {
        DrawTextF(dl, g_font_label, 9.0f,
                  ImVec2(p.x + 4, p.y + 78), kNerv.acc,
                  m.p2_char_name.c_str());
    }

    // Stage on bottom row
    if (!m.stage_name.empty()) {
        dl->AddLine(ImVec2(p.x, p.y + 96),
                    ImVec2(wmax.x - kPadX, p.y + 96), kNerv.line, 1.0f);
        DrawTextF(dl, g_font_micro, 9.0f,
                  ImVec2(p.x, p.y + 102), kNerv.dim, "STAGE");
        DrawTextF(dl, g_font_label, 9.0f,
                  ImVec2(p.x + 56, p.y + 102), kNerv.ink2,
                  m.stage_name.c_str());
    }
    (void)lu;
}

// ─── MATCH FLOW ──────────────────────────────────────────────────────
// Three overlays driven by `LauncherState`:
//   * Connecting -> full-viewport handshake / booting splash
//   * InGame     -> top banner ("IN MATCH vs <peer>") so a returning
//                   alt-tab to the launcher tells the user the game
//                   is still live + offers an END button
//   * hash mismatch -> red modal (drives off hub_state_'s flag, can
//                      fire in any state when hook reports a desync).
//
// MatchFlow visual is one merged Connecting overlay. The launch
// pipeline doesn't currently expose a "peer-punched, now booting" sub-
// state to LauncherUI, so we estimate the phase from elapsed time
// instead — STUN+punch is sub-second, the rest is game spawn + hook
// init (typically 5-30 s). When `LauncherUI` grows a real connect-
// phase accessor, switch the highlight from time-derived to event-
// derived; the rendering math stays the same.

void RenderMatchFlowConnecting(LauncherUI& lu) {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0, 0, 0, 220));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));

    if (ImGui::Begin("##matchflow.connecting", nullptr,
                     ImGuiWindowFlags_NoTitleBar |
                     ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoBringToFrontOnFocus |
                     ImGuiWindowFlags_NoSavedSettings)) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 c = vp->GetCenter();
        const float t = (float)ImGui::GetTime();

        // Track when this overlay first appeared so we can drive the
        // phase pills off elapsed time. The function only renders
        // while LauncherState == Connecting; if we see a gap > 0.5s
        // since the last frame, we left + re-entered (next match) and
        // the timer should restart from 0.
        static double s_connecting_entered = 0.0;
        static double s_last_frame         = 0.0;
        const double now_t = ImGui::GetTime();
        if (s_last_frame == 0.0 || now_t - s_last_frame > 0.5) {
            s_connecting_entered = now_t;
        }
        s_last_frame = now_t;
        const float elapsed = (float)(now_t - s_connecting_entered);

        // Animated pulsing "P2P / NAT TRAVERSAL" dot above title
        const float pulse = 0.3f + 0.7f *
            (0.5f + 0.5f * std::sin(t * (IM_PI * 2.0f / 1.4f)));
        ImU32 dot_col =
            IM_COL32(0xff, 0x6a, 0x00, (int)(255.0f * pulse));
        dl->AddCircleFilled(ImVec2(c.x, c.y - 80), 4.0f, dot_col, 16);

        DrawTextF(dl, g_font_micro, 9.0f,
                  ImVec2(c.x - 90, c.y - 64), kNerv.acc,
                  "P2P \xc2\xb7 NAT TRAVERSAL");

        const std::string& peer = lu.current_match_peer_nick();
        const float peer_w = g_font_display
            ? g_font_display->CalcTextSizeA(36.0f, FLT_MAX, 0.0f, peer.c_str()).x
            : ImGui::CalcTextSize(peer.c_str()).x;
        DrawTextF(dl, g_font_display, 36.0f,
                  ImVec2(c.x - peer_w * 0.5f, c.y - 40),
                  kNerv.ink, peer.c_str());

        DrawTextF(dl, g_font_label, 11.0f,
                  ImVec2(c.x - 92, c.y + 16), kNerv.ink2,
                  "Connecting peer-to-peer\xe2\x80\xa6");

        // Phase pills — STUN → PUNCH → BOOT, highlight progresses
        // through them based on elapsed time. Heuristic, but it
        // gives the user real "still happening" feedback instead of
        // a static label.
        struct Phase { const char* label; float at; };
        const Phase phases[] = {
            { "STUN",  0.0f },
            { "PUNCH", 0.4f },
            { "BOOT",  1.0f },
        };
        int active = 0;
        for (int i = 0; i < 3; ++i) if (elapsed >= phases[i].at) active = i;
        const float px0 = c.x - 92, py = c.y + 36;
        float cx = px0;
        for (int i = 0; i < 3; ++i) {
            const ImU32 col = (i < active) ? kNerv.acc
                            : (i == active) ? kNerv.amber
                            : kNerv.dim;
            const char* sep = (i + 1 < 3) ? " \xc2\xb7 " : "";
            char seg[32];
            std::snprintf(seg, sizeof(seg), "%s%s", phases[i].label, sep);
            ImVec2 sz = g_font_micro
                ? g_font_micro->CalcTextSizeA(9.0f, FLT_MAX, 0.0f, seg)
                : ImGui::CalcTextSize(seg);
            DrawTextF(dl, g_font_micro, 9.0f, ImVec2(cx, py), col, seg);
            cx += sz.x;
        }

        // Cancel — hooks into legacy on_session_stop which the launch
        // pipeline already supports.
        ImGui::SetCursorScreenPos(ImVec2(c.x - 60, c.y + 70));
        ImGui::PushStyleColor(ImGuiCol_Button,        IM_COL32(0xff,0x30,0x30,0x40));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(0xff,0x30,0x30,0x80));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  IM_COL32(0xff,0x30,0x30,0xc0));
        if (ImGui::Button("CANCEL", ImVec2(120, 28))) {
            if (lu.on_session_stop) lu.on_session_stop();
        }
        ImGui::PopStyleColor(3);
    }
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void RenderInGameBanner(LauncherUI& lu) {
    // Compact banner pinned to the top of the viewport so an alt-tab
    // back to the launcher confirms the match is still live + offers a
    // way to bail out. Sits above the existing chrome (before TopBar).
    ImGuiViewport* vp = ImGui::GetMainViewport();
    const float h = 28.0f;
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x, vp->WorkPos.y));
    ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, h));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0xff, 0x6a, 0x00, 0x40));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 4));

    if (ImGui::Begin("##matchflow.ingame", nullptr,
                     ImGuiWindowFlags_NoTitleBar |
                     ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoBringToFrontOnFocus |
                     ImGuiWindowFlags_NoSavedSettings)) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 p = ImGui::GetCursorScreenPos();
        DrawTextF(dl, g_font_micro, 9.0f,
                  ImVec2(p.x, p.y + 4), kNerv.ink, ICON_FA_CIRCLE_DOT " IN MATCH");
        const std::string& peer = lu.current_match_peer_nick();
        DrawTextF(dl, g_font_label, 11.0f,
                  ImVec2(p.x + 100, p.y + 3), kNerv.ink, peer.c_str());

        // END button right-aligned
        ImGui::SetCursorScreenPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - 110,
                                          vp->WorkPos.y + 2));
        ImGui::PushStyleColor(ImGuiCol_Button,        IM_COL32(0xff,0x30,0x30,0x40));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(0xff,0x30,0x30,0x80));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  IM_COL32(0xff,0x30,0x30,0xc0));
        if (ImGui::Button("END MATCH", ImVec2(100, 22))) {
            if (lu.on_session_stop) lu.on_session_stop();
        }
        ImGui::PopStyleColor(3);
    }
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void RenderDesyncModal(LauncherUI& lu) {
    constexpr const char* id = "##desync.modal";
    const bool show = lu.show_hash_mismatch();
    if (show && !ImGui::IsPopupOpen(id)) {
        ImGui::OpenPopup(id);
    }

    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, kNerv.bg);
    ImGui::PushStyleColor(ImGuiCol_Border,  kNerv.red);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,    ImVec2(20, 16));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize,  1.0f);

    if (ImGui::BeginPopupModal(id, nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize |
                               ImGuiWindowFlags_NoTitleBar |
                               ImGuiWindowFlags_NoSavedSettings)) {
        if (!show) {
            ImGui::CloseCurrentPopup();
        } else {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            DrawTextF(dl, g_font_micro, 9.0f,
                      ImGui::GetCursorScreenPos(), kNerv.red,
                      "E_HASH \xc2\xb7 MATCH INVALID");
            ImGui::Dummy(ImVec2(0, 14));
            DrawTextF(dl, g_font_display, 24.0f,
                      ImGui::GetCursorScreenPos(), kNerv.ink,
                      "Hash mismatch.");
            ImGui::Dummy(ImVec2(0, 28));
            DrawTextF(dl, g_font_label, 11.0f,
                      ImGui::GetCursorScreenPos(), kNerv.ink2,
                      "Your local game files differ from your peer's.");
            ImGui::Dummy(ImVec2(0, 2));
            DrawTextF(dl, g_font_label, 11.0f,
                      ImGui::GetCursorScreenPos(), kNerv.ink2,
                      "Compare your installation against the manifest below.");
            ImGui::Dummy(ImVec2(0, 8));

            // Excerpt — readable in JetBrains Mono inside a child box.
            const std::string& excerpt = lu.hash_mismatch_excerpt();
            if (!excerpt.empty()) {
                ImGui::PushStyleColor(ImGuiCol_ChildBg, kNerv.bg2);
                ImGui::BeginChild("##desync.log", ImVec2(540, 140),
                                  ImGuiChildFlags_Borders);
                ImGui::PushStyleColor(ImGuiCol_Text,
                                      ImGui::ColorConvertU32ToFloat4(kNerv.ink2));
                ImGui::PushTextWrapPos(530);
                ImGui::TextUnformatted(excerpt.c_str());
                ImGui::PopTextWrapPos();
                ImGui::PopStyleColor();
                ImGui::EndChild();
                ImGui::PopStyleColor();
            }

            ImGui::Dummy(ImVec2(0, 12));
            if (ImGui::Button("DISMISS", ImVec2(120, 28))) {
                lu.DismissHashMismatch();
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
}

// ─── CONNECTION LOST MODAL ───────────────────────────────────────────
// Fires when the hub WS goes from connected to disconnected. Doesn't
// fire when we never connected (offline launch) or when the user
// explicitly signed out. Re-arms on reconnect so subsequent drops
// pop the modal again.
void RenderConnectionLostModal(LauncherUI& lu) {
    constexpr const char* id = "##connection.lost";
    static bool s_seen_connected = false;
    static bool s_lost_armed     = false;

    const bool now_connected = lu.hub_connected();
    if (now_connected) {
        s_seen_connected = true;
        s_lost_armed     = false;          // re-arm for next disconnect
    } else if (s_seen_connected && !s_lost_armed) {
        s_lost_armed = true;
        ImGui::OpenPopup(id);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "shell: hub disconnected -> ConnectionLost modal");
    }

    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, kNerv.bg);
    ImGui::PushStyleColor(ImGuiCol_Border,  kNerv.red);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,    ImVec2(20, 16));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize,  1.0f);

    if (ImGui::BeginPopupModal(id, nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize |
                               ImGuiWindowFlags_NoTitleBar |
                               ImGuiWindowFlags_NoSavedSettings)) {
        if (now_connected) {
            // Auto-close if hub came back before user dismissed.
            ImGui::CloseCurrentPopup();
        } else {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            DrawTextF(dl, g_font_micro, 9.0f,
                      ImGui::GetCursorScreenPos(), kNerv.red,
                      "E_NET \xc2\xb7 CONNECTION LOST");
            ImGui::Dummy(ImVec2(0, 14));
            DrawTextF(dl, g_font_display, 24.0f,
                      ImGui::GetCursorScreenPos(), kNerv.ink,
                      "Hub disconnected.");
            ImGui::Dummy(ImVec2(0, 28));
            DrawTextF(dl, g_font_label, 11.0f,
                      ImGui::GetCursorScreenPos(), kNerv.ink2,
                      "Network drop or hub restart. The launcher will keep");
            ImGui::Dummy(ImVec2(0, 2));
            DrawTextF(dl, g_font_label, 11.0f,
                      ImGui::GetCursorScreenPos(), kNerv.ink2,
                      "your session; sign in again if it doesn't recover.");
            ImGui::Dummy(ImVec2(0, 18));

            if (ImGui::Button("DISMISS", ImVec2(120, 28))) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Button,
                                  IM_COL32(0xff,0x6a,0x00,0x40));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                                  IM_COL32(0xff,0x6a,0x00,0x80));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                                  IM_COL32(0xff,0x6a,0x00,0xc0));
            if (ImGui::Button("SIGN IN AGAIN", ImVec2(150, 28))) {
                lu.OpenDiscordAuth();
                ImGui::CloseCurrentPopup();
            }
            ImGui::PopStyleColor(3);
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
}

// ─── CHAT RING BUFFER (per room) ─────────────────────────────────────
// Per-room chat log, capped at kChatMaxLines. Lines are pushed by
// LauncherUI on K::ChatReceived (own-messages echo too — the hub
// IRC-style broadcasts back to the sender; do NOT push locally on
// send, or duplicates appear).
struct ChatLine {
    std::string ts;         // "HH:MM" derived from the hub's authoritative unix seconds
    std::string user_id;    // empty for system lines
    std::string nick;
    std::string text;
    bool        system = false;  // true for hub-generated lines (joins/leaves/etc)
};

constexpr size_t kChatMaxLines = 200;

std::deque<ChatLine>& GetRoomChat(const std::string& room_id) {
    static std::unordered_map<std::string, std::deque<ChatLine>> store;
    return store[room_id];
}

void PushRoomChat(const std::string& room_id, ChatLine line) {
    auto& q = GetRoomChat(room_id);
    q.push_back(std::move(line));
    while (q.size() > kChatMaxLines) q.pop_front();
}

// Format a hub unix-seconds stamp as "HH:MM" in local time. Falls back
// to the literal "now" when ts == 0 (system lines that didn't carry a
// stamp).
std::string FormatChatTs(int64_t ts_unix_seconds) {
    if (ts_unix_seconds <= 0) return "now";
    std::time_t t = static_cast<std::time_t>(ts_unix_seconds);
    std::tm tm_local{};
#if defined(_WIN32)
    localtime_s(&tm_local, &t);
#else
    localtime_r(&t, &tm_local);
#endif
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%02d:%02d",
                  tm_local.tm_hour, tm_local.tm_min);
    return buf;
}

// ─── IN-ROOM VIEW ────────────────────────────────────────────────────
// Renders the active room's lobby chat + RULES/RESOURCES band. Reached
// by clicking a populated lobby card on the home dashboard, or a chip
// on the LeftRail. Returning to home dashboard goes through ShellState's
// in_room flag (see DrawHomeDashboard's HOME affordance).
void DrawInRoomView(LauncherUI& lu, ImVec2 origin, ImVec2 size,
                    const std::vector<ActiveRoom>& rooms) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    auto& s = State();

    // ── Slim 4px tint divider (was 30px room-context strip). The
    // TopBar already shows the active game id + .exe label; repeating
    // it here was redundant. Just a 2px tint underline + a "CONNECTED
    // · N PEERS" status chip in the top-right of the body.
    const ActiveRoom* room = nullptr;
    if (s.active_room_index >= 0 && s.active_room_index < (int)rooms.size()) {
        room = &rooms[s.active_room_index];
    }
    if (room) {
        dl->AddRectFilled(origin,
                          ImVec2(origin.x + size.x, origin.y + 2),
                          room->tint.a);
    } else {
        dl->AddLine(origin,
                    ImVec2(origin.x + size.x, origin.y),
                    kNerv.line, 1.0f);
    }

    // ── HOME affordance — pinned top-right above the body.
    {
        const char* label = "\xe2\x86\x90 HOME";
        const float pt = 10.0f * g_ui_scale;
        ImVec2 sz = g_font_label
            ? g_font_label->CalcTextSizeA(pt, FLT_MAX, 0.0f, label)
            : ImGui::CalcTextSize(label);
        ImVec2 bp(origin.x + size.x - sz.x - S(16), origin.y + S(6));
        ImVec2 b1(bp.x + sz.x + S(8), bp.y + S(16));
        ImGui::SetCursorScreenPos(bp);
        ImGui::InvisibleButton("##in_room.home", ImVec2(sz.x + S(8), S(16)));
        const bool hov = ImGui::IsItemHovered();
        if (hov) {
            dl->AddRectFilled(bp, b1, IM_COL32(0xff,0x6a,0x00,0x18));
        }
        dl->AddRect(bp, b1, kNerv.line);
        DrawTextF(dl, g_font_label, 10.0f,
                  ImVec2(bp.x + S(4), bp.y + S(1)),
                  hov ? kNerv.acc : kNerv.ink2, label);
        if (ImGui::IsItemClicked()) {
            // "← HOME" leaves the chat for the discovery view (BROWSE
            // now hosts the home dashboard, not Lobby).
            s.hub_view = HubView::Browse;
        }
    }

    // ── Body — RULES + (future) RESOURCES + chat.
    ImVec2 body(origin.x + S(14), origin.y + S(24));
    ImVec2 body1(origin.x + size.x - S(14), origin.y + size.y - S(12));

    // RULES — static, always visible.
    {
        if (g_font_micro) {
            DrawTextF(dl, g_font_micro, 9.0f,
                      ImVec2(body.x, body.y), kNerv.acc, "RULES");
        }
        const char* rules =
            "By using FM2K_ROLLBACK you adhere to fgcoc.com. Be civil. No "
            "racism, sexism, impersonation, threats. Ignore User via "
            "right-click. Mute Chat in settings. No turbo / macro / "
            "rank-boosting.";
        ImGui::SetCursorScreenPos(ImVec2(body.x, body.y + S(12)));
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(kNerv.ink2));
        PushFontIf(g_font_body);
        ImGui::PushTextWrapPos(body1.x);
        ImGui::TextWrapped("%s", rules);
        ImGui::PopTextWrapPos();
        PopFontIf(g_font_body);
        ImGui::PopStyleColor();
    }

    float ry = body.y + S(70);

    // Chat
    const float cy = ry;
    const float chat_input_h = S(22.0f);
    const float chat_log_top = cy + S(14);
    const float chat_log_bot = body1.y - chat_input_h - S(4);

    const bool chat_live = lu.hub_has_chat() && lu.hub_connected();

    if (g_font_micro) {
        char chat_header[64];
        std::snprintf(chat_header, sizeof(chat_header),
                      "CHAT \xc2\xb7 #%s", room ? room->game_id.c_str() : "lobby");
        DrawTextF(dl, g_font_micro, 9.0f,
                  ImVec2(body.x, cy), kNerv.dim, chat_header);
        const char* tag = nullptr;
        if (!lu.hub_connected())      tag = "[ OFFLINE ]";
        else if (!lu.hub_has_chat())  tag = "[ HUB \xe2\x80\xa2 no chat_v1 ]";
        if (tag) {
            const float pt = 9.0f * g_ui_scale;
            ImVec2 tsz = g_font_micro->CalcTextSizeA(pt, FLT_MAX, 0.0f, tag);
            DrawTextF(dl, g_font_micro, 9.0f,
                      ImVec2(body1.x - tsz.x - S(4), cy), kNerv.amber, tag);
        }
    }

    if (room) {
        const auto& log = GetRoomChat(room->game_id);
        const float row_h = S(13.0f);
        const int   max_visible = (int)std::max(0.0f,
                                  (chat_log_bot - chat_log_top) / row_h);
        const int   first = std::max(0, (int)log.size() - max_visible);
        const std::string& my_id = lu.hub_my_id();
        float ly = chat_log_top;
        for (int i = first; i < (int)log.size(); ++i) {
            const auto& l = log[i];
            if (ly + row_h > chat_log_bot) break;
            if (!l.user_id.empty() && lu.IsUserIgnored(l.user_id)) continue;
            const bool me = !l.system && !my_id.empty() && l.user_id == my_id;
            ImU32 ts_col   = kNerv.faint;
            ImU32 who_col  = l.system ? kNerv.dim
                                      : (me ? kNerv.acc : kNerv.ink2);
            ImU32 text_col = l.system ? kNerv.dim : kNerv.ink;
            DrawTextF(dl, g_font_label, 9.0f,
                      ImVec2(body.x, ly), ts_col, l.ts.c_str());
            DrawTextF(dl, g_font_label, 9.0f,
                      ImVec2(body.x + S(36), ly), who_col, l.nick.c_str());
            DrawTextF(dl, g_font_body, 13.0f,
                      ImVec2(body.x + S(150), ly - S(2)), text_col, l.text.c_str());
            ly += row_h;
        }
    }

    // Input bar at the bottom
    if (room) {
        ImVec2 in_pos(body.x + S(4), body1.y - chat_input_h);
        ImGui::SetCursorScreenPos(in_pos);
        if (!chat_live) ImGui::BeginDisabled();
        ImGui::PushStyleColor(ImGuiCol_FrameBg,        kNerv.bg2);
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IM_COL32(0xff,0x6a,0x00,0x10));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive,  IM_COL32(0xff,0x6a,0x00,0x18));
        ImGui::PushStyleColor(ImGuiCol_Text,
                              ImGui::ColorConvertU32ToFloat4(kNerv.ink));
        const float send_w  = S(50.0f);
        const float input_w = (body1.x - in_pos.x) - send_w - S(8.0f);
        ImGui::SetNextItemWidth(input_w);
        static char s_chat_input[256] = "";
        const bool entered = ImGui::InputText(
            "##chat.input", s_chat_input, sizeof(s_chat_input),
            ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine(0, S(4));
        const bool send_clicked = ImGui::Button("SEND", ImVec2(send_w, 0));
        ImGui::PopStyleColor(4);
        if (!chat_live) ImGui::EndDisabled();

        if (chat_live && (entered || send_clicked) && s_chat_input[0]) {
            lu.SendChat(room->game_id, s_chat_input);
            s_chat_input[0] = '\0';
            // Re-focus the input so user can keep typing.
            ImGui::SetKeyboardFocusHere(-1);
        }
    }
}

// ─── HOME DASHBOARD ─ HubView::Lobby (out-of-room) ───────────────────
// Replaces the bare "RULES + chat" landing with a search/filter strip
// + Popular / Populated lobbies / Categories / Hidden gems / Upcoming
// events sections. Clicking a room card flips s.in_room=true and the
// existing DrawInRoomView takes over.
//
// Data sources:
//   * Popular         — sorted lu.hub_rooms() by user_count desc, capped
//   * Populated       — lu.hub_rooms() with user_count > 0
//   * Categories      — hand-curated tag map (catalog tags not shipped
//                       hub-side yet); each tag → list of game_ids
//   * Hidden gems     — rotating subset of installed games not in the
//                       popular slice
//   * Upcoming events — placeholder; renders BACKEND PENDING chip
//
// Filter rules: hidden_rooms in ShellState are excluded from every
// section unless show_hidden_rooms is true. Search filters by case-
// insensitive substring against the game id + display name.

namespace {

bool ContainsIgnoreCase(const std::string& hay, const std::string& needle) {
    if (needle.empty()) return true;
    auto lower = [](char c) {
        return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
    };
    if (hay.size() < needle.size()) return false;
    for (size_t i = 0; i + needle.size() <= hay.size(); ++i) {
        bool match = true;
        for (size_t j = 0; j < needle.size(); ++j) {
            if (lower(hay[i + j]) != lower(needle[j])) { match = false; break; }
        }
        if (match) return true;
    }
    return false;
}

bool RoomPassesFilters(const ShellState& s, const std::string& game_id,
                       const std::string& display_name,
                       const std::string& search_lower) {
    if (!s.show_hidden_rooms && IsRoomHidden(s, game_id)) return false;
    if (!search_lower.empty()) {
        if (!ContainsIgnoreCase(game_id, search_lower) &&
            !ContainsIgnoreCase(display_name, search_lower)) return false;
    }
    return true;
}

// Hand-curated category map. Keys are short labels rendered as chips;
// values are game_ids that should appear inside that category. Until
// the hub catalog ships proper tags, this lives as a static fallback
// — easy to extend in code, eventually swap for a /api/games tag join.
struct CategoryDef {
    const char* label;
    std::vector<std::string> game_ids;
};
const std::vector<CategoryDef>& GetCategoryDefs() {
    static const std::vector<CategoryDef> cats = {
        { "POPULAR",  { "WonderfulWorld_ver_0946", "pkmncc",
                        "URORFGRelease102", "vanpri" } },
        { "DOUJIN",   { "vanpri", "WonderfulWorld_ver_0946" } },
        { "ANIME",    { "pkmncc" } },
        { "OTHER",    { "URORFGRelease102" } },
    };
    return cats;
}

}  // namespace

void DrawHomeDashboard(LauncherUI& lu, ImVec2 origin, ImVec2 size,
                       const std::vector<ActiveRoom>& rooms) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    auto& s = State();

    // Left content padding — small breathing room from the LeftRail.
    // S(5) ≈ 10 px at the 1280×900 default ui_scale=2.
    const float kContentLeftPad = S(5.0f);

    // Header band — sticky at top. Search input centered horizontally;
    // hidden toggle and count chip flank it.
    const float header_h = S(32.0f);
    {
        dl->AddRectFilled(origin,
                          ImVec2(origin.x + size.x, origin.y + header_h),
                          kNerv.bg2);
        dl->AddLine(ImVec2(origin.x, origin.y + header_h),
                    ImVec2(origin.x + size.x, origin.y + header_h),
                    kNerv.line, 1.0f);

        // Centered search input — width fixed, x derived from canvas
        // center so the search visually anchors the strip.
        const float search_w = S(220.0f);
        const float search_x = origin.x + (size.x - search_w) * 0.5f;
        ImGui::SetCursorScreenPos(ImVec2(search_x, origin.y + S(7)));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, kNerv.bg);
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IM_COL32(0xff,0x6a,0x00,0x10));
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(kNerv.ink));
        ImGui::SetNextItemWidth(search_w);
        ImGui::InputTextWithHint("##home.search", "search games...",
                                 s.home_search_buf, sizeof(s.home_search_buf));
        ImGui::PopStyleColor(3);

        // Hidden toggle — anchored to the LEFT (where the search used
        // to live), so it doesn't displace the centered search.
        ImGui::SetCursorScreenPos(
            ImVec2(origin.x + kContentLeftPad, origin.y + S(7)));
        ImGui::PushStyleColor(ImGuiCol_Text,
                              ImGui::ColorConvertU32ToFloat4(
                                  s.show_hidden_rooms ? kNerv.amber : kNerv.dim));
        if (ImGui::Button(s.show_hidden_rooms ? "[ " ICON_FA_EYE       " SHOWING HIDDEN ]"
                                              : "[ " ICON_FA_EYE_SLASH " SHOW HIDDEN ]")) {
            s.show_hidden_rooms = !s.show_hidden_rooms;
            const std::string path = SettingsPath();
            if (!path.empty()) {
                WriteBool(path, "show_hidden_rooms", s.show_hidden_rooms);
            }
        }
        ImGui::PopStyleColor();

        // Right-side count chip.
        int rooms_total = 0, rooms_live = 0;
        for (const auto& r : lu.hub_rooms()) {
            if (!s.show_hidden_rooms && IsRoomHidden(s, r.id)) continue;
            ++rooms_total;
            if (r.user_count > 0) ++rooms_live;
        }
        char chip[64];
        std::snprintf(chip, sizeof(chip), "%d ROOMS \xc2\xb7 %d LIVE",
                      rooms_total, rooms_live);
        const float pt = 9.0f * g_ui_scale;
        ImVec2 csz = g_font_micro
            ? g_font_micro->CalcTextSizeA(pt, FLT_MAX, 0.0f, chip)
            : ImGui::CalcTextSize(chip);
        DrawTextF(dl, g_font_micro, 9.0f,
                  ImVec2(origin.x + size.x - csz.x - S(10), origin.y + S(12)),
                  kNerv.dim, chip);
    }

    // Helper for "enter room" clicks from any section card. Subscribes
    // the room (so it shows up in the LeftRail), rebuilds active rooms
    // to find the new index, sets hub_view to Lobby, and joins the
    // hub-side room. Wraps the boilerplate that used to live inline in
    // each section's click handler.
    auto enter_room = [&](const std::string& gid,
                          const std::string& display_name) {
        bool present = false;
        for (const auto& sr : s.subscribed_rooms) {
            if (sr == gid) { present = true; break; }
        }
        if (!present) s.subscribed_rooms.push_back(gid);
        const auto rebuilt = BuildActiveRooms(lu);
        for (int k = 0; k < (int)rebuilt.size(); ++k) {
            if (rebuilt[k].game_id == gid) {
                s.active_room_index = k;
                break;
            }
        }
        lu.SetActiveRoom(gid, display_name);
        s.hub_view = HubView::Lobby;
    };

    // Scrollable body. The cards/section rows are drawn with raw
    // ImDrawList calls, so ImGui doesn't auto-track content height —
    // capture the start cursor and call ImGui::Dummy(end - start) at
    // the bottom so the child knows when to enable mousewheel scroll.
    const std::string search_lower = s.home_search_buf;
    // Body inset by kContentLeftPad on the left so sections breathe
    // away from the sidebar. Width shrinks correspondingly.
    ImGui::SetCursorScreenPos(ImVec2(origin.x + kContentLeftPad,
                                     origin.y + header_h));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, IM_COL32(0,0,0,0));
    ImGui::PushStyleColor(ImGuiCol_Text,
                          ImGui::ColorConvertU32ToFloat4(kNerv.ink));
    ImGui::BeginChild("##home.body",
                      ImVec2(size.x - kContentLeftPad,
                             size.y - header_h),
                      false,
                      ImGuiWindowFlags_NoBackground |
                      ImGuiWindowFlags_AlwaysVerticalScrollbar);
    const ImVec2 home_body_start = ImGui::GetCursorScreenPos();

    auto section_header = [&](const char* label, const char* sub) {
        ImDrawList* sdl = ImGui::GetWindowDrawList();
        ImVec2 cp = ImGui::GetCursorScreenPos();
        DrawTextF(sdl, g_font_micro, 9.0f, cp, kNerv.acc, label);
        if (sub) {
            const float pt = 9.0f * g_ui_scale;
            ImVec2 sz = g_font_micro
                ? g_font_micro->CalcTextSizeA(pt, FLT_MAX, 0.0f, label)
                : ImGui::CalcTextSize(label);
            DrawTextF(sdl, g_font_micro, 9.0f,
                      ImVec2(cp.x + sz.x + S(10), cp.y), kNerv.dim, sub);
        }
        ImGui::Dummy(ImVec2(0, S(14)));
    };

    // ── POPULAR — top 6 rooms by user_count, fallback to first 6 known ──
    {
        ImGui::Dummy(ImVec2(0, S(8)));
        section_header("POPULAR " ICON_FA_FIRE, "live activity");

        struct Cand { std::string id, name; int user_count; };
        std::vector<Cand> cands;
        for (const auto& r : lu.hub_rooms()) {
            if (!RoomPassesFilters(s, r.id, r.name, search_lower)) continue;
            cands.push_back({r.id, r.name, r.user_count});
        }
        if (cands.empty()) {
            for (const auto& g : lu.games()) {
                const std::string id = GameIdFromExeName(g.GetExeName());
                if (!RoomPassesFilters(s, id, g.GetExeName(), search_lower)) continue;
                cands.push_back({id, g.GetExeName(), 0});
            }
        }
        std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) {
            return a.user_count > b.user_count;
        });
        const int kMax = 6;
        if ((int)cands.size() > kMax) cands.resize(kMax);

        // Card row — sized so 6 fit in the body width at common UI
        // scales. 78 × 60 base × 6 + 5×6 gap = 498 logical — fits in
        // 640×450 minus rail (≈592 logical) with margin. At 2×
        // window: 1556 px needed, 1184 main_w available. Tightened
        // gap so all 6 popular cards land on one row.
        const float card_w = S(78.0f), card_h = S(60.0f), gap = S(6.0f);
        ImVec2 row_p = ImGui::GetCursorScreenPos();
        for (int i = 0; i < (int)cands.size(); ++i) {
            ImVec2 cp(row_p.x + i * (card_w + gap), row_p.y);
            ImVec2 c1(cp.x + card_w, cp.y + card_h);
            ImDrawList* sdl = ImGui::GetWindowDrawList();
            sdl->AddRectFilled(cp, c1, kNerv.bg2);
            sdl->AddRect(cp, c1, kNerv.line);
            std::string label = Utf8SafeTrunc(cands[i].id, 9);
            DrawTextF(sdl, g_font_label, 11.0f,
                      ImVec2(cp.x + S(6), cp.y + S(6)), kNerv.ink, label.c_str());
            char uc[16];
            if (cands[i].user_count > 0) {
                std::snprintf(uc, sizeof(uc), ICON_FA_CIRCLE_DOT " %d", cands[i].user_count);
                DrawTextF(sdl, g_font_micro, 9.0f,
                          ImVec2(cp.x + S(6), cp.y + card_h - S(14)),
                          kNerv.acc, uc);
            } else {
                DrawTextF(sdl, g_font_micro, 9.0f,
                          ImVec2(cp.x + S(6), cp.y + card_h - S(14)),
                          kNerv.faint, ICON_FA_CIRCLE " idle");
            }
            ImGui::SetCursorScreenPos(cp);
            char btn_id[48]; std::snprintf(btn_id, sizeof(btn_id), "##pop.%d", i);
            ImGui::InvisibleButton(btn_id, ImVec2(card_w, card_h));
            if (ImGui::IsItemClicked()) {
                enter_room(cands[i].id, cands[i].name);
            }
            if (ImGui::IsItemHovered()) {
                sdl->AddRect(cp, c1, kNerv.acc, 0.0f, 0, 1.5f);
            }
        }
        // Reset cursor and reserve exact section height. InvisibleButton
        // auto-advances already accumulated above, so without the reset
        // the explicit Dummy doubles the gap.
        ImGui::SetCursorScreenPos(row_p);
        ImGui::Dummy(ImVec2(0, card_h + S(14)));
    }

    // ── POPULATED LOBBIES ────────────────────────────────────────────
    {
        section_header("POPULATED LOBBIES " ICON_FA_USERS, nullptr);
        std::vector<fm2k::HubRoom> live;
        for (const auto& r : lu.hub_rooms()) {
            if (r.user_count <= 0) continue;
            if (!RoomPassesFilters(s, r.id, r.name, search_lower)) continue;
            live.push_back(r);
        }
        std::sort(live.begin(), live.end(),
                  [](const fm2k::HubRoom& a, const fm2k::HubRoom& b) {
                      return a.user_count > b.user_count;
                  });
        if (live.empty()) {
            DrawTextF(ImGui::GetWindowDrawList(), g_font_body, 13.0f,
                      ImGui::GetCursorScreenPos(), kNerv.dim,
                      "no live lobbies right now \xc2\xb7 challenge in any room to start one");
            ImGui::Dummy(ImVec2(0, S(18)));
        } else {
            const float lrow_h = S(18);
            for (size_t i = 0; i < live.size(); ++i) {
                ImVec2 rp = ImGui::GetCursorScreenPos();
                ImDrawList* sdl = ImGui::GetWindowDrawList();
                ImVec2 r1(rp.x + size.x - S(24), rp.y + lrow_h);
                if (i % 2 == 0) {
                    sdl->AddRectFilled(rp, r1, IM_COL32(0xff, 0x6a, 0x00, 0x06));
                }
                DrawTextF(sdl, g_font_body, 13.0f,
                          ImVec2(rp.x, rp.y), kNerv.ink, live[i].name.c_str());
                char uc[32];
                std::snprintf(uc, sizeof(uc), "%d users", live[i].user_count);
                DrawTextF(sdl, g_font_label, 9.0f,
                          ImVec2(rp.x + S(280), rp.y + S(4)), kNerv.acc, uc);
                ImGui::SetCursorScreenPos(rp);
                char btn_id[48]; std::snprintf(btn_id, sizeof(btn_id), "##live.%zu", i);
                ImGui::InvisibleButton(btn_id, ImVec2(size.x - S(24), lrow_h));
                if (ImGui::IsItemClicked()) {
                    enter_room(live[i].id, live[i].name);
                }
                if (ImGui::IsItemHovered()) {
                    sdl->AddRect(rp, r1, kNerv.acc, 0.0f, 0, 1.0f);
                }
            }
        }
        ImGui::Dummy(ImVec2(0, 8));
    }

    // ── CATEGORIES — chip grid, click filters Browse view ────────────
    {
        section_header("BROWSE BY", nullptr);
        const auto& cats = GetCategoryDefs();
        ImVec2 row_p = ImGui::GetCursorScreenPos();
        const float chip_start_y = row_p.y;
        float cx = row_p.x;
        const float chip_h = S(22.0f);
        for (const auto& cat : cats) {
            const float pt = 11.0f * g_ui_scale;
            ImVec2 sz = g_font_label
                ? g_font_label->CalcTextSizeA(pt, FLT_MAX, 0.0f, cat.label)
                : ImGui::CalcTextSize(cat.label);
            if (cx + sz.x + S(18) > row_p.x + size.x - S(24)) {
                cx = row_p.x;
                row_p.y += chip_h + S(4);
            }
            ImVec2 cp(cx, row_p.y);
            ImVec2 c1(cp.x + sz.x + S(14), cp.y + chip_h);
            ImDrawList* sdl = ImGui::GetWindowDrawList();
            sdl->AddRectFilled(cp, c1, IM_COL32(0xff,0x6a,0x00,0x10));
            sdl->AddRect(cp, c1, kNerv.line);
            DrawTextF(sdl, g_font_label, 11.0f,
                      ImVec2(cp.x + S(7), cp.y + S(4)),
                      kNerv.ink2, cat.label);
            ImGui::SetCursorScreenPos(cp);
            char btn_id[48]; std::snprintf(btn_id, sizeof(btn_id), "##cat.%s", cat.label);
            ImGui::InvisibleButton(btn_id, ImVec2(sz.x + S(14), chip_h));
            if (ImGui::IsItemClicked()) {
                State().hub_view = HubView::Browse;
            }
            cx += sz.x + S(14) + S(6);
        }
        // Advance with Dummy so the child window tracks the chip rows
        // (multiple if wrapped). chip_total_h = bottom of the last row
        // minus where we started + trailing padding.
        const float chip_total_h = (row_p.y + chip_h) - chip_start_y + S(12);
        ImGui::SetCursorScreenPos(ImVec2(origin.x + S(14), chip_start_y));
        ImGui::Dummy(ImVec2(0, chip_total_h));
    }

    // ── LIBRARY — full card grid of installed games (the merged
    //   "boxes and stuff" Browse work). Same visual language as the
    //   original DrawBrowseView card layout: gradient cover, eng
    //   banner, big initials, title band, fav star + JOIN pill on
    //   hover. Click → enter_room. Hover top-right star → toggle fav.
    {
        section_header("LIBRARY " ICON_FA_LIST, "all installed");

        const auto& games = lu.games();
        if (games.empty()) {
            DrawTextF(ImGui::GetWindowDrawList(), g_font_body, 13.0f,
                      ImGui::GetCursorScreenPos(), kNerv.dim,
                      "No games discovered. Add a root folder in Setup.");
            ImGui::Dummy(ImVec2(0, S(18)));
        } else {
            // Card aspect locked at 4:3 cover + small body band.
            // Pick the natural column count from a 120-px target
            // width, then EXPAND each card to fill the row evenly so
            // there's no dead gap on the right. Cards stay > 100 px
            // wide and ≤ a reasonable max so they don't grow huge on
            // ultrawide windows.
            const float kGap         = S(8.0f);
            const float kTargetCardW = S(120.0f);
            const float kBodyH       = S(32.0f);

            ImDrawList* sdl    = ImGui::GetWindowDrawList();
            const ImVec2 row_p = ImGui::GetCursorScreenPos();
            const float content_w = ImGui::GetContentRegionAvail().x;
            const int   cols  = std::max(1, (int)((content_w + kGap) /
                                                  (kTargetCardW + kGap)));
            // Justify cards to fill the row: split the leftover
            // (content_w - cols*target - (cols-1)*gap) back into each
            // card's width. Net: no gap on the right edge.
            const float kCardW = (content_w - (cols - 1) * kGap) / (float)cols;
            const float kCoverW = kCardW;
            // Re-derive cover height from the 4:3 aspect on the new
            // card width so the cards stay proportional.
            const float kCoverH = kCardW * (90.0f / 120.0f);
            const float kCardH  = kCoverH + kBodyH;

            for (size_t i = 0; i < games.size(); ++i) {
                const auto&       g    = games[i];
                const ChipTint    tint = TintForGame(g);
                const std::string gid  = GameIdFromExeName(g.GetExeName());
                if (!RoomPassesFilters(s, gid, g.GetExeName(), search_lower))
                    continue;

                const int col = (int)(i % cols);
                const int row = (int)(i / cols);
                ImVec2 cp(row_p.x + col * (kCardW + kGap),
                          row_p.y + row * (kCardH + kGap));
                ImVec2 cover_br(cp.x + kCoverW, cp.y + kCoverH);

                // Flat tint — solid primary, no top-to-bottom gradient.
                sdl->AddRectFilled(cp, cover_br, tint.a);

                // Banner — engine + fav star indicator
                const float banner_h = S(14);
                ImVec2 ban_br(cover_br.x, cp.y + banner_h);
                sdl->AddRectFilled(cp, ban_br, IM_COL32(0,0,0,160));
                const char* eng = (g.engine == FM2K::Engine::FM95) ? "FM95" : "FM2K";
                DrawTextF(sdl, g_font_micro, 8.0f,
                          ImVec2(cp.x + S(5), cp.y + S(3)), kNerv.acc, eng);
                const bool is_fav = IsFavorite(s, gid);
                if (is_fav) {
                    DrawTextF(sdl, g_font_micro, 9.0f,
                              ImVec2(cover_br.x - S(12), cp.y + S(2)),
                              kNerv.amber, ICON_FA_STAR);
                }

                // Initials — UTF-8-aware fallback so non-ASCII gids
                // (BishiBashi-style) get visible glyphs instead of "??".
                std::string init;
                {
                    int alpha_count = 0;
                    for (char c : gid) {
                        if (alpha_count >= 2) break;
                        if (std::isalpha((unsigned char)c)) {
                            init.push_back((char)std::toupper((unsigned char)c));
                            ++alpha_count;
                        }
                    }
                    if (init.empty()) init = Utf8SafeTrunc(gid, 6);
                    if (init.empty()) init = "??";
                }
                if (g_font_display) {
                    const float pt = 36.0f * g_ui_scale;
                    ImVec2 sz = g_font_display->CalcTextSizeA(pt, FLT_MAX, 0.0f, init.c_str());
                    ImVec2 ip(cp.x + (kCoverW - sz.x) * 0.5f,
                              cp.y + banner_h + (kCoverH - banner_h - sz.y) * 0.5f);
                    DrawTextF(sdl, g_font_display, 36.0f,
                              ImVec2(ip.x + S(1), ip.y + S(1)),
                              IM_COL32(0,0,0,180), init.c_str());
                    DrawTextF(sdl, g_font_display, 36.0f, ip,
                              IM_COL32(255,255,255,235), init.c_str());
                }
                sdl->AddRect(cp, cover_br, kNerv.line, 0.0f, 0, 1.0f);

                // Body band — title + subscribed indicator
                ImVec2 body_tl(cp.x, cover_br.y);
                ImVec2 body_br(cp.x + kCardW, cp.y + kCardH);
                sdl->AddRectFilled(body_tl, body_br, kNerv.bg2);
                sdl->AddRect(body_tl, body_br, kNerv.line, 0.0f, 0, 1.0f);

                std::string title;
                {
                    std::string dir = g.GetExeDir();
                    size_t slash = dir.find_last_of("/\\");
                    title = (slash == std::string::npos) ? dir : dir.substr(slash + 1);
                    if (title.empty()) title = gid;
                }
                if (title.size() > 18) {
                    title = Utf8SafeTrunc(title, 17) + "\xe2\x80\xa6";
                }
                DrawTextF(sdl, g_font_label, 10.0f,
                          ImVec2(cp.x + S(6), cover_br.y + S(6)),
                          kNerv.ink, title.c_str());

                bool subscribed = false;
                for (const auto& r : rooms) {
                    if (r.game_id == gid) { subscribed = true; break; }
                }
                DrawTextF(sdl, g_font_micro, 8.0f,
                          ImVec2(cp.x + S(6), cover_br.y + S(20)),
                          subscribed ? kNerv.acc : kNerv.faint,
                          subscribed ? ICON_FA_CIRCLE_DOT " IN ROOM"
                                     : ICON_FA_CIRCLE    " ROOM");

                // Hit target — full card. Top-right corner = star
                // toggle (matches DrawBrowseView semantics).
                ImGui::SetCursorScreenPos(cp);
                char btn_id[32]; std::snprintf(btn_id, sizeof(btn_id),
                                                "##lib.card.%zu", i);
                const bool clicked = ImGui::InvisibleButton(btn_id,
                                                            ImVec2(kCardW, kCardH));
                const bool hovered = ImGui::IsItemHovered();
                const ImVec2 fav_min(cover_br.x - S(22), cp.y + S(1));
                const ImVec2 fav_max(cover_br.x - S(1),  cp.y + S(22));
                if (clicked) {
                    const ImVec2 mp = ImGui::GetIO().MousePos;
                    const bool on_fav = (mp.x >= fav_min.x && mp.x <= fav_max.x &&
                                         mp.y >= fav_min.y && mp.y <= fav_max.y);
                    if (on_fav) ToggleFavorite(s, gid);
                    else        enter_room(gid, g.GetExeName());
                }
                if (hovered) {
                    const ImVec2 mp = ImGui::GetIO().MousePos;
                    const bool over_fav = (mp.x >= fav_min.x && mp.x <= fav_max.x &&
                                           mp.y >= fav_min.y && mp.y <= fav_max.y);
                    sdl->AddRectFilled(fav_min, fav_max,
                                       over_fav ? IM_COL32(0,0,0,200)
                                                : IM_COL32(0,0,0,140));
                    sdl->AddRect(fav_min, fav_max,
                                 over_fav ? kNerv.amber : kNerv.line,
                                 0.0f, 0, 1.0f);
                    DrawTextF(sdl, g_font_micro, 12.0f,
                              ImVec2(fav_min.x + S(4), fav_min.y + S(3)),
                              is_fav ? kNerv.amber
                                     : (over_fav ? kNerv.amber : kNerv.ink2),
                              ICON_FA_STAR);

                    // JOIN pill bottom-right of cover
                    const char* join_lbl = "JOIN " ICON_FA_PLAY;
                    const float pt = 9.0f * g_ui_scale;
                    ImVec2 jsz = g_font_label
                        ? g_font_label->CalcTextSizeA(pt, FLT_MAX, 0.0f, join_lbl)
                        : ImVec2(S(40), S(12));
                    ImVec2 j_min(cover_br.x - jsz.x - S(12), cover_br.y - S(20));
                    ImVec2 j_max(cover_br.x - S(4),          cover_br.y - S(4));
                    sdl->AddRectFilled(j_min, j_max, IM_COL32(0xff,0x5a,0x3c,0xc0));
                    sdl->AddRect(j_min, j_max, kNerv.acc, 0.0f, 0, 1.0f);
                    DrawTextF(sdl, g_font_label, 9.0f,
                              ImVec2(j_min.x + S(4), j_min.y + S(2)),
                              IM_COL32(255,255,255,255), join_lbl);

                    sdl->AddRect(cp, body_br, kNerv.acc, 0.0f, 0, 1.5f);
                    ImGui::SetTooltip("%s\n%s",
                                      title.c_str(), g.exe_path.c_str());
                }
            }

            // Reserve total grid extent so the home BeginChild
            // includes all rows in its scrollable height.
            const int total_rows = ((int)games.size() + cols - 1) / cols;
            const float grid_h = total_rows * (kCardH + kGap) - kGap;
            ImGui::SetCursorScreenPos(row_p);
            ImGui::Dummy(ImVec2(0, std::max(0.0f, grid_h) + S(8)));
        }
    }

    // ── HIDDEN GEMS ─────────────────────────────────────────────────
    {
        section_header("HIDDEN GEMS " ICON_FA_GEM, "low play count, locally installed");
        std::vector<std::string> popular_ids;
        {
            std::vector<fm2k::HubRoom> sorted = lu.hub_rooms();
            std::sort(sorted.begin(), sorted.end(),
                      [](const fm2k::HubRoom& a, const fm2k::HubRoom& b) {
                          return a.user_count > b.user_count;
                      });
            for (size_t i = 0; i < sorted.size() && i < 3; ++i) {
                popular_ids.push_back(sorted[i].id);
            }
        }
        std::vector<std::pair<std::string,std::string>> gems;
        for (const auto& g : lu.games()) {
            const std::string id = GameIdFromExeName(g.GetExeName());
            if (!RoomPassesFilters(s, id, g.GetExeName(), search_lower)) continue;
            bool is_popular = false;
            for (const auto& pid : popular_ids) if (pid == id) { is_popular = true; break; }
            if (is_popular) continue;
            gems.push_back({id, g.GetExeName()});
        }
        const int kMaxGems = 4;
        if ((int)gems.size() > kMaxGems) gems.resize(kMaxGems);
        if (gems.empty()) {
            DrawTextF(ImGui::GetWindowDrawList(), g_font_body, 13.0f,
                      ImGui::GetCursorScreenPos(), kNerv.dim,
                      "no off-the-beaten-path picks this round.");
            ImGui::Dummy(ImVec2(0, S(18)));
        } else {
            ImVec2 row_p = ImGui::GetCursorScreenPos();
            const float card_w = S(132.0f), card_h = S(36.0f), gap = S(6.0f);
            for (int i = 0; i < (int)gems.size(); ++i) {
                ImVec2 cp(row_p.x + i * (card_w + gap), row_p.y);
                ImVec2 c1(cp.x + card_w, cp.y + card_h);
                ImDrawList* sdl = ImGui::GetWindowDrawList();
                sdl->AddRectFilled(cp, c1, kNerv.bg2);
                sdl->AddRect(cp, c1, kNerv.line);
                DrawTextF(sdl, g_font_label, 10.0f,
                          ImVec2(cp.x + S(6), cp.y + S(6)), kNerv.ink, gems[i].first.c_str());
                DrawTextF(sdl, g_font_micro, 9.0f,
                          ImVec2(cp.x + S(6), cp.y + S(22)),
                          kNerv.dim, "installed");
                ImGui::SetCursorScreenPos(cp);
                char btn_id[48]; std::snprintf(btn_id, sizeof(btn_id), "##gem.%d", i);
                ImGui::InvisibleButton(btn_id, ImVec2(card_w, card_h));
                if (ImGui::IsItemClicked()) {
                    enter_room(gems[i].first, gems[i].second);
                }
                if (ImGui::IsItemHovered()) {
                    sdl->AddRect(cp, c1, kNerv.acc, 0.0f, 0, 1.5f);
                }
            }
            ImGui::SetCursorScreenPos(row_p);
            ImGui::Dummy(ImVec2(0, card_h + S(8)));
        }
    }

    // ── UPCOMING EVENTS ─────────────────────────────────────────────
    {
        ImVec2 hp = ImGui::GetCursorScreenPos();
        ImDrawList* sdl = ImGui::GetWindowDrawList();
        DrawTextF(sdl, g_font_micro, 9.0f,
                  hp, kNerv.acc, "UPCOMING EVENTS " ICON_FA_CALENDAR);
        const char* tag = "[ BACKEND PENDING ]";
        const float pt = 9.0f * g_ui_scale;
        ImVec2 tsz = g_font_micro
            ? g_font_micro->CalcTextSizeA(pt, FLT_MAX, 0.0f, tag)
            : ImGui::CalcTextSize(tag);
        (void)tsz;
        DrawTextF(sdl, g_font_micro, 9.0f,
                  ImVec2(hp.x + S(140), hp.y), kNerv.amber, tag);
        ImGui::Dummy(ImVec2(0, S(16)));
        DrawTextF(sdl, g_font_body, 13.0f,
                  ImGui::GetCursorScreenPos(), kNerv.dim,
                  "tournaments + community events land when hub /api/events ships.");
        ImGui::Dummy(ImVec2(0, S(24)));
    }

    // Trailing pad so the last section isn't flush against the scroll
    // child's bottom edge.
    (void)home_body_start;
    ImGui::Dummy(ImVec2(0, S(8)));

    ImGui::EndChild();
    ImGui::PopStyleColor(2);
}

// ─── STUB VIEWS ──────────────────────────────────────────────────────
// Stub views render a header band + body text + a "BACKEND PENDING"
// chip when the data needs hub work that isn't shipped yet. M-plan
// fills these in later; for now they signal intent rather than look
// like rendering bugs.
void DrawStubView(ImVec2 origin, ImVec2 size,
                  const char* title, const char* note,
                  bool backend_pending = false) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 br(origin.x + size.x, origin.y + size.y);

    ImVec2 hero1(br.x, origin.y + S(92));
    dl->AddRectFilled(origin, hero1, kNerv.bg2);
    dl->AddLine(ImVec2(origin.x, hero1.y),
                ImVec2(br.x, hero1.y), kNerv.line, 1.0f);

    if (g_font_micro) {
        DrawTextF(dl, g_font_micro, 9.0f,
                  ImVec2(origin.x + S(14), origin.y + S(12)),
                  kNerv.dim, "// VIEW \xe2\x80\xa2 stub");
    }
    if (g_font_display) {
        DrawTextF(dl, g_font_display, 26.0f,
                  ImVec2(origin.x + S(14), origin.y + S(26)),
                  kNerv.ink, title);
    }
    if (g_font_body) {
        DrawTextF(dl, g_font_body, 14.0f,
                  ImVec2(origin.x + S(14), origin.y + S(60)),
                  kNerv.ink2, note);
    }
    if (backend_pending && g_font_micro) {
        const char* tag = "[ BACKEND PENDING ]";
        const float pt = 9.0f * g_ui_scale;
        ImVec2 sz = g_font_micro->CalcTextSizeA(pt, FLT_MAX, 0.0f, tag);
        ImVec2 cp(br.x - sz.x - S(18), origin.y + S(14));
        dl->AddRectFilled(ImVec2(cp.x - S(4), cp.y - S(2)),
                          ImVec2(cp.x + sz.x + S(4), cp.y + S(12)),
                          IM_COL32(0xff,0xb0,0x00,0x14));
        dl->AddRect(ImVec2(cp.x - S(4), cp.y - S(2)),
                    ImVec2(cp.x + sz.x + S(4), cp.y + S(12)),
                    kNerv.amber);
        DrawTextF(dl, g_font_micro, 9.0f, cp, kNerv.amber, tag);
    }
}

// ─── MAP VIEW ─ HubView::Map ─────────────────────────────────────────
// Embed FM2KInputBinder::RenderBody for each player slot. Two-tab
// layout (P1 / P2) keeps the picker compact. State is shared with
// the legacy Settings → Input Bindings tab; auto-saves on change.
void DrawMapView(LauncherUI& lu, ImVec2 origin, ImVec2 size) {
    (void)lu;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 br(origin.x + size.x, origin.y + size.y);

    ImVec2 hero1(br.x, origin.y + S(60));
    dl->AddRectFilled(origin, hero1, kNerv.bg2);
    dl->AddLine(ImVec2(origin.x, hero1.y),
                ImVec2(br.x, hero1.y), kNerv.line, 1.0f);
    if (g_font_micro) {
        DrawTextF(dl, g_font_micro, 9.0f,
                  ImVec2(origin.x + S(14), origin.y + S(12)),
                  kNerv.dim, "// VIEW \xc2\xb7 controls");
    }
    if (g_font_display) {
        DrawTextF(dl, g_font_display, 22.0f,
                  ImVec2(origin.x + S(14), origin.y + S(26)),
                  kNerv.ink, "CTRL MAP");
    }

    static bool s_binder_initialized = false;
    if (!s_binder_initialized) {
        FM2KInputBinder::Init();
        s_binder_initialized = true;
    }

    ImGui::SetCursorScreenPos(ImVec2(origin.x + S(14), hero1.y + S(10)));
    ImGui::PushStyleColor(ImGuiCol_Text,
                          ImGui::ColorConvertU32ToFloat4(kNerv.ink));
    ImGui::BeginChild("##shell.map.body",
                      ImVec2(size.x - S(28), size.y - S(70) - S(4)),
                      false,
                      ImGuiWindowFlags_NoBackground);
    if (ImGui::BeginTabBar("##shell.map.players")) {
        if (ImGui::BeginTabItem("P1")) {
            if (FM2KInputBinder::RenderBody(0)) FM2KInputBinder::Save();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("P2")) {
            if (FM2KInputBinder::RenderBody(1)) FM2KInputBinder::Save();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

// ─── CONFIG VIEW ─ HubView::Config ───────────────────────────────────
// Wraps the existing legacy Settings tab bar (input bindings / host
// config / hub server / games folders / display). State is shared
// with the legacy Settings window — anything edited in either is
// reflected in the other since both call the same body functions.
void DrawConfigView(LauncherUI& lu, ImVec2 origin, ImVec2 size) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 br(origin.x + size.x, origin.y + size.y);

    ImVec2 hero1(br.x, origin.y + S(60));
    dl->AddRectFilled(origin, hero1, kNerv.bg2);
    dl->AddLine(ImVec2(origin.x, hero1.y),
                ImVec2(br.x, hero1.y), kNerv.line, 1.0f);
    if (g_font_micro) {
        DrawTextF(dl, g_font_micro, 9.0f,
                  ImVec2(origin.x + S(14), origin.y + S(12)),
                  kNerv.dim, "// VIEW \xc2\xb7 config");
    }
    if (g_font_display) {
        DrawTextF(dl, g_font_display, 22.0f,
                  ImVec2(origin.x + S(14), origin.y + S(26)),
                  kNerv.ink, "CONFIG");
    }

    ImGui::SetCursorScreenPos(ImVec2(origin.x + S(14), hero1.y + S(10)));
    ImGui::PushStyleColor(ImGuiCol_Text,
                          ImGui::ColorConvertU32ToFloat4(kNerv.ink));
    ImGui::BeginChild("##shell.config.body",
                      ImVec2(size.x - S(28), size.y - S(70) - S(4)),
                      false,
                      ImGuiWindowFlags_NoBackground);
    lu.RenderConfigBody();
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

// ─── PROFILE VIEW ─ HubView::Profile ─────────────────────────────────
// Big W-L-D summary + top-N vs-breakdown derived from the recent-
// matches cache (newest 50). The hub-side `my_vs` hash has the
// numbers but no nicks for offline opponents; deriving from
// hub_recent_matches gives both in one pass without needing a
// separate accessor.
void DrawProfileView(LauncherUI& lu, ImVec2 origin, ImVec2 size) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 br(origin.x + size.x, origin.y + size.y);

    ImVec2 hero1(br.x, origin.y + S(84));
    dl->AddRectFilled(origin, hero1, kNerv.bg2);
    dl->AddLine(ImVec2(origin.x, hero1.y),
                ImVec2(br.x, hero1.y), kNerv.line, 1.0f);

    if (g_font_micro) {
        DrawTextF(dl, g_font_micro, 9.0f,
                  ImVec2(origin.x + S(14), origin.y + S(12)),
                  kNerv.dim, "// VIEW \xc2\xb7 profile");
    }
    const std::string& nick = lu.discord_signed_in()
        ? lu.discord_nick() : lu.hub_my_nick();
    if (g_font_display && !nick.empty()) {
        DrawTextF(dl, g_font_display, 26.0f,
                  ImVec2(origin.x + S(14), origin.y + S(26)),
                  kNerv.ink, nick.c_str());
    } else if (g_font_display) {
        DrawTextF(dl, g_font_display, 22.0f,
                  ImVec2(origin.x + S(14), origin.y + S(26)),
                  kNerv.dim, "(no nick)");
    }

    {
        const auto& users = lu.hub_users();
        const std::string& my_id = lu.hub_my_id();
        auto it = my_id.empty() ? users.end() : users.find(my_id);
        const std::string tier = (it != users.end() && !it->second.tier.empty())
            ? it->second.tier : std::string("tester");
        if (g_font_label) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "TIER \xc2\xb7 %s",
                          tier.c_str());
            DrawTextF(dl, g_font_label, 9.0f,
                      ImVec2(origin.x + S(14), origin.y + S(60)),
                      kNerv.acc, buf);
        }
    }

    // Big record summary
    const int wins   = lu.my_wins();
    const int losses = lu.my_losses();
    const int draws  = lu.my_draws();
    const int gp     = std::max(0, wins) + std::max(0, losses) + std::max(0, draws);
    const int win_pct = (gp > 0 && wins >= 0)
        ? (int)((wins * 100 + gp / 2) / gp) : 0;

    if (g_font_display) {
        char rec[48];
        if (wins < 0) {
            std::snprintf(rec, sizeof(rec), "—");
        } else {
            std::snprintf(rec, sizeof(rec), "%d-%d-%d", wins, losses, draws);
        }
        DrawTextF(dl, g_font_display, 38.0f,
                  ImVec2(origin.x + S(14), hero1.y + S(14)),
                  kNerv.acc, rec);
    }
    if (g_font_label && wins >= 0) {
        char pct[40];
        std::snprintf(pct, sizeof(pct), "%d%% \xc2\xb7 %d games", win_pct, gp);
        DrawTextF(dl, g_font_label, 11.0f,
                  ImVec2(origin.x + S(14), hero1.y + S(56)),
                  kNerv.dim, pct);
    }

    // Vs-breakdown table — derive from recent matches, top-by-games.
    struct VsRow {
        std::string nick;
        int wins = 0, losses = 0, draws = 0;
    };
    std::unordered_map<std::string, VsRow> vs;
    const std::string& my_id = lu.hub_my_id();
    if (!my_id.empty()) {
        for (const auto& m : lu.hub_recent_matches()) {
            const bool im_p1 = (m.p1_id == my_id);
            const bool im_p2 = (m.p2_id == my_id);
            if (!im_p1 && !im_p2) continue;
            const std::string& opp_id   = im_p1 ? m.p2_id   : m.p1_id;
            const std::string& opp_nick = im_p1 ? m.p2_nick : m.p1_nick;
            VsRow& v = vs[opp_id];
            if (v.nick.empty()) v.nick = opp_nick;
            if (m.winner_id.empty())              ++v.draws;
            else if (m.winner_id == my_id)        ++v.wins;
            else                                  ++v.losses;
        }
    }
    std::vector<VsRow> rows;
    rows.reserve(vs.size());
    for (auto& kv : vs) rows.push_back(std::move(kv.second));
    std::sort(rows.begin(), rows.end(), [](const VsRow& a, const VsRow& b) {
        return (a.wins + a.losses + a.draws) > (b.wins + b.losses + b.draws);
    });

    float ty = hero1.y + S(16);
    if (g_font_micro) {
        DrawTextF(dl, g_font_micro, 9.0f,
                  ImVec2(origin.x + S(240), ty), kNerv.dim, "TOP MATCHUPS \xc2\xb7 last 50");
    }
    ty += S(18);
    const float col_x = origin.x + S(240);
    const float col_w = S(56.0f);
    const float col_nick_w = S(180);
    if (g_font_label) {
        DrawTextF(dl, g_font_label, 9.0f, ImVec2(col_x,                          ty), kNerv.dim, "OPPONENT");
        DrawTextF(dl, g_font_label, 9.0f, ImVec2(col_x + col_nick_w,             ty), kNerv.dim, "W");
        DrawTextF(dl, g_font_label, 9.0f, ImVec2(col_x + col_nick_w + col_w,     ty), kNerv.dim, "L");
        DrawTextF(dl, g_font_label, 9.0f, ImVec2(col_x + col_nick_w + 2 * col_w, ty), kNerv.dim, "D");
    }
    ty += S(14);
    dl->AddLine(ImVec2(col_x, ty - S(3)), ImVec2(br.x - S(14), ty - S(3)), kNerv.line, 1.0f);
    const int kMaxRows = 8;
    int shown = 0;
    const float prow_h = S(18);
    for (const auto& r : rows) {
        if (shown >= kMaxRows) break;
        if (ty + prow_h > br.y - S(12)) break;
        DrawTextF(dl, g_font_body, 13.0f, ImVec2(col_x, ty),
                  kNerv.ink, r.nick.empty() ? "(unknown)" : r.nick.c_str());
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%d", r.wins);
        DrawTextF(dl, g_font_body, 13.0f, ImVec2(col_x + col_nick_w, ty), kNerv.acc, buf);
        std::snprintf(buf, sizeof(buf), "%d", r.losses);
        DrawTextF(dl, g_font_body, 13.0f, ImVec2(col_x + col_nick_w + col_w, ty), kNerv.ink2, buf);
        std::snprintf(buf, sizeof(buf), "%d", r.draws);
        DrawTextF(dl, g_font_body, 13.0f, ImVec2(col_x + col_nick_w + 2 * col_w, ty), kNerv.dim, buf);
        ty += prow_h;
        ++shown;
    }
    if (rows.empty() && g_font_body) {
        DrawTextF(dl, g_font_body, 13.0f,
                  ImVec2(col_x, ty), kNerv.dim,
                  "No tracked matches yet.");
    }
}

// ─── STATS VIEW ─ HubView::Stats ─────────────────────────────────────
// Aggregates lu.hub_recent_matches() into a per-character W/L/D table
// from MY perspective. Hub answers RequestRecentMatches with at most
// 50 rows by default — small enough to walk every frame. When the
// hub ships a real /api/stats with full history (post-v2), swap this
// for a server-side leaderboard query.
void DrawStatsView(LauncherUI& lu, ImVec2 origin, ImVec2 size) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 br(origin.x + size.x, origin.y + size.y);

    // Header band
    ImVec2 hero1(br.x, origin.y + S(60));
    dl->AddRectFilled(origin, hero1, kNerv.bg2);
    dl->AddLine(ImVec2(origin.x, hero1.y),
                ImVec2(br.x, hero1.y), kNerv.line, 1.0f);
    if (g_font_micro) {
        DrawTextF(dl, g_font_micro, 9.0f,
                  ImVec2(origin.x + S(14), origin.y + S(12)),
                  kNerv.dim, "// VIEW \xc2\xb7 stats \xc2\xb7 last 50");
    }
    if (g_font_display) {
        DrawTextF(dl, g_font_display, 22.0f,
                  ImVec2(origin.x + S(14), origin.y + S(26)),
                  kNerv.ink, "STATS");
    }

    const std::string& my_id = lu.hub_my_id();
    const auto& matches = lu.hub_recent_matches();

    if (my_id.empty() || matches.empty()) {
        if (g_font_body) {
            const char* msg = my_id.empty()
                ? "Connect to the hub to populate stats."
                : "No recent matches yet. Play a netplay set; stats fill in as match_result events land.";
            DrawTextF(dl, g_font_body, 13.0f,
                      ImVec2(origin.x + S(14), hero1.y + S(14)),
                      kNerv.dim, msg);
        }
        return;
    }

    // Aggregation pass — per-my-char row.
    struct Row {
        std::string char_label;     // resolved name, fallback "Char #N"
        int wins   = 0;
        int losses = 0;
        int draws  = 0;
    };
    std::unordered_map<std::string, Row> by_char;
    int total_w = 0, total_l = 0, total_d = 0;
    auto label_for = [](const std::string& name, int32_t id) -> std::string {
        if (!name.empty()) return name;
        if (id < 0) return "?";
        char buf[24];
        std::snprintf(buf, sizeof(buf), "Char #%d", (int)id);
        return buf;
    };
    for (const auto& m : matches) {
        const bool im_p1 = (m.p1_id == my_id);
        const bool im_p2 = (m.p2_id == my_id);
        if (!im_p1 && !im_p2) continue;
        const std::string my_char = label_for(
            im_p1 ? m.p1_char_name : m.p2_char_name,
            im_p1 ? m.p1_char_id   : m.p2_char_id);
        Row& r = by_char[my_char];
        r.char_label = my_char;
        if (m.winner_id.empty())               { ++r.draws;  ++total_d; }
        else if (m.winner_id == my_id)         { ++r.wins;   ++total_w; }
        else                                   { ++r.losses; ++total_l; }
    }

    std::vector<Row> rows;
    rows.reserve(by_char.size());
    for (auto& kv : by_char) rows.push_back(std::move(kv.second));
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
        const int ta = a.wins + a.losses + a.draws;
        const int tb = b.wins + b.losses + b.draws;
        return ta > tb;
    });

    // Totals strip — drawn on the parent (sticky in the header band).
    char totals[96];
    std::snprintf(totals, sizeof(totals),
                  "TOTAL  %d-%d-%d  \xc2\xb7  %d games \xc2\xb7 %d chars",
                  total_w, total_l, total_d,
                  total_w + total_l + total_d, (int)rows.size());
    if (g_font_micro) {
        DrawTextF(dl, g_font_micro, 9.0f,
                  ImVec2(origin.x + S(14), hero1.y + S(8)),
                  kNerv.acc, totals);
    }

    // Scrollable body
    ImGui::SetCursorScreenPos(ImVec2(origin.x + S(14), hero1.y + S(28)));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0,0,0,0));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, IM_COL32(0,0,0,0));
    ImGui::BeginChild("##shell.stats.body",
                      ImVec2(size.x - S(28), size.y - (hero1.y - origin.y) - S(28) - S(4)),
                      false,
                      ImGuiWindowFlags_NoBackground);

    ImDrawList* cdl = ImGui::GetWindowDrawList();
    const ImVec2 cbase = ImGui::GetCursorScreenPos();

    // Table — char | W | L | D | TOTAL | win%
    const float row_h = S(18.0f);
    float ty = cbase.y;
    const float col_char_w = S(220.0f);
    const float col_w      = S(56.0f);
    if (g_font_label) {
        const char* hdrs[] = { "CHARACTER", "W", "L", "D", "GP", "WIN%" };
        float cx = cbase.x;
        for (int i = 0; i < 6; ++i) {
            DrawTextF(cdl, g_font_label, 9.0f,
                      ImVec2(i == 0 ? cx : cx + col_char_w + (i - 1) * col_w, ty),
                      kNerv.dim, hdrs[i]);
        }
    }
    ty += S(14);
    cdl->AddLine(ImVec2(cbase.x, ty - S(3)),
                 ImVec2(cbase.x + size.x - S(28), ty - S(3)),
                 kNerv.line, 1.0f);

    for (const auto& r : rows) {
        const int gp = r.wins + r.losses + r.draws;
        const int win_pct = (gp > 0) ? (int)((r.wins * 100 + gp / 2) / gp) : 0;
        const float cx = cbase.x;
        DrawTextF(cdl, g_font_body, 13.0f, ImVec2(cx, ty),
                  kNerv.ink, r.char_label.c_str());
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%d", r.wins);
        DrawTextF(cdl, g_font_body, 13.0f, ImVec2(cx + col_char_w, ty),
                  kNerv.acc, buf);
        std::snprintf(buf, sizeof(buf), "%d", r.losses);
        DrawTextF(cdl, g_font_body, 13.0f, ImVec2(cx + col_char_w + col_w, ty),
                  kNerv.ink2, buf);
        std::snprintf(buf, sizeof(buf), "%d", r.draws);
        DrawTextF(cdl, g_font_body, 13.0f, ImVec2(cx + col_char_w + 2 * col_w, ty),
                  kNerv.dim, buf);
        std::snprintf(buf, sizeof(buf), "%d", gp);
        DrawTextF(cdl, g_font_body, 13.0f, ImVec2(cx + col_char_w + 3 * col_w, ty),
                  kNerv.ink2, buf);
        std::snprintf(buf, sizeof(buf), "%d%%", win_pct);
        DrawTextF(cdl, g_font_body, 13.0f, ImVec2(cx + col_char_w + 4 * col_w, ty),
                  kNerv.acc, buf);
        ty += row_h;
    }
    // Reserve content height so the child knows when to scroll.
    ImGui::Dummy(ImVec2(0, std::max(0.0f, ty - cbase.y)));

    ImGui::EndChild();
    ImGui::PopStyleColor(2);
}

// ─── REPLAYS VIEW ─ HubView::Replays ─────────────────────────────────
// Wraps LauncherUI::RenderReplayBrowser inside a NERV header band +
// child window. The legacy browser handles its own scan + tree, so we
// just need the chrome and the embed. Click → on_replay_play wired
// via the existing callback hook.
void DrawReplaysView(LauncherUI& lu, ImVec2 origin, ImVec2 size) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 br(origin.x + size.x, origin.y + size.y);

    ImVec2 hero1(br.x, origin.y + S(60));
    dl->AddRectFilled(origin, hero1, kNerv.bg2);
    dl->AddLine(ImVec2(origin.x, hero1.y),
                ImVec2(br.x, hero1.y), kNerv.line, 1.0f);
    if (g_font_micro) {
        DrawTextF(dl, g_font_micro, 9.0f,
                  ImVec2(origin.x + S(14), origin.y + S(12)),
                  kNerv.dim, "// VIEW \xc2\xb7 replays");
    }
    if (g_font_display) {
        DrawTextF(dl, g_font_display, 22.0f,
                  ImVec2(origin.x + S(14), origin.y + S(26)),
                  kNerv.ink, "REPLAYS");
    }

    ImGui::SetCursorScreenPos(ImVec2(origin.x + S(14), hero1.y + S(10)));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0,0,0,0));
    ImGui::PushStyleColor(ImGuiCol_Text,
                          ImGui::ColorConvertU32ToFloat4(kNerv.ink));
    ImGui::BeginChild("##shell.replays.body",
                      ImVec2(size.x - S(28), size.y - S(70) - S(4)),
                      false,
                      ImGuiWindowFlags_NoBackground);
    lu.RenderReplayBrowser();
    ImGui::EndChild();
    ImGui::PopStyleColor(2);
}

// ─── BROWSE VIEW ─ HubView::Browse (M6.6) ────────────────────────────
// Real catalog grid backed by lu.games() (the local install set).
// Cards mirror FM2K's native 320×240 cover-art aspect (4:3) at half
// scale — 132×99 cover with a banner strip on top + small body band
// underneath. Click a card → switches the active room to that game's
// id (auto-subscribing it if it wasn't already) and drops back into
// the Lobby. When hub /api/games ships, append non-installed entries
// in a faded variant with an [INSTALL] badge.
//
// No header chrome — the LeftRail's "browse" pane indicator is enough
// signal that this is the catalog. Going straight to grid mirrors
// fightcade and recovers ~56px of vertical space (lets us show 8
// cards at 640×450 minimum).
void DrawBrowseView(LauncherUI& lu, ImVec2 origin, ImVec2 size,
                    const std::vector<ActiveRoom>& rooms) {
    ImVec2 br(origin.x + size.x, origin.y + size.y);
    auto& s = State();
    const auto& games = lu.games();

    // Empty state — no installed games yet.
    if (games.empty()) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        if (g_font_body) {
            DrawTextF(dl, g_font_body, 13.0f,
                      ImVec2(origin.x + S(14), origin.y + S(18)),
                      kNerv.ink2,
                      "No games discovered. Add a root folder in Setup or "
                      "drop FM2K/FM95 builds into a scanned directory.");
        }
        return;
    }

    // ── Card grid in a scroll child. Sizes scale via S() so the layout
    // tracks ui_scale; at 1× we fit 4 cols × 2 rows in 640×450.
    const float kCoverW = S(136.0f);       // 4:3 cover
    const float kCoverH = S(102.0f);
    const float kBodyH  = S(36.0f);
    const float kCardW  = kCoverW;
    const float kCardH  = kCoverH + kBodyH;
    const float kGap    = S(8.0f);
    const float kPadX   = S(10.0f);
    const float kPadY   = S(10.0f);

    ImVec2 child_origin = origin;
    ImVec2 child_size  (size.x, br.y - child_origin.y);

    ImGui::SetCursorScreenPos(child_origin);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0,0,0,0));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, IM_COL32(0,0,0,0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(kPadX, kPadY));
    ImGui::BeginChild("##browse.grid", child_size, false,
                      ImGuiWindowFlags_NoScrollWithMouse |
                      ImGuiWindowFlags_AlwaysVerticalScrollbar);

    ImDrawList* cdl       = ImGui::GetWindowDrawList();   // child-clipped
    const ImVec2 grid_o   = ImGui::GetCursorScreenPos();  // captured ONCE
    const float  content_w = ImGui::GetContentRegionAvail().x;
    const int    cols      = std::max(1, (int)((content_w + kGap) /
                                               (kCardW + kGap)));

    for (size_t i = 0; i < games.size(); ++i) {
        const auto&       g    = games[i];
        const ChipTint    tint = TintForGame(g);
        const std::string gid  = GameIdFromExeName(g.GetExeName());

        const int col = (int)(i % cols);
        const int row = (int)(i / cols);
        ImVec2 cp(grid_o.x + col * (kCardW + kGap),
                  grid_o.y + row * (kCardH + kGap));

        // Cover (4:3) — gradient placeholder until cover art lands.
        ImVec2 cover_br(cp.x + kCoverW, cp.y + kCoverH);
        cdl->AddRectFilledMultiColor(cp, cover_br,
                                     tint.a, tint.a, tint.b, tint.b);

        // Top banner strip
        const float banner_h = S(14);
        ImVec2 ban_br(cover_br.x, cp.y + banner_h);
        cdl->AddRectFilled(cp, ban_br, IM_COL32(0,0,0,160));
        const char* eng = (g.engine == FM2K::Engine::FM95) ? "FM95" : "FM2K";
        if (g_font_micro) {
            DrawTextF(cdl, g_font_micro, 8.0f,
                      ImVec2(cp.x + S(5), cp.y + S(3)), kNerv.acc, eng);
        }
        const bool is_fav = IsFavorite(s, gid);
        if (is_fav) {
            DrawTextF(cdl, g_font_micro ? g_font_micro : nullptr, 9.0f,
                      ImVec2(cover_br.x - S(12), cp.y + S(2)),
                      kNerv.amber, "\xe2\x98\x85");
        }

        // Initials drop-shadowed in cover middle.
        char init[4] = {0};
        int  ic = 0;
        for (char c : gid) {
            if (ic >= 2) break;
            if (std::isalpha((unsigned char)c)) {
                init[ic++] = (char)std::toupper((unsigned char)c);
            }
        }
        if (ic == 0) { init[0] = '?'; init[1] = '?'; }
        if (g_font_display) {
            const float pt = 36.0f * g_ui_scale;
            ImVec2 sz = g_font_display->CalcTextSizeA(pt, FLT_MAX, 0.0f, init);
            ImVec2 ip(cp.x + (kCoverW - sz.x) * 0.5f,
                      cp.y + banner_h + (kCoverH - banner_h - sz.y) * 0.5f);
            DrawTextF(cdl, g_font_display, 36.0f,
                      ImVec2(ip.x + S(1), ip.y + S(1)),
                      IM_COL32(0,0,0,180), init);
            DrawTextF(cdl, g_font_display, 36.0f, ip,
                      IM_COL32(255,255,255,235), init);
        }
        cdl->AddRect(cp, cover_br, kNerv.line, 0.0f, 0, 1.0f);

        // Body band under the cover — title (game folder name) +
        // subscribed indicator.
        ImVec2 body_tl(cp.x, cover_br.y);
        ImVec2 body_br(cp.x + kCardW, cp.y + kCardH);
        cdl->AddRectFilled(body_tl, body_br, kNerv.bg2);
        cdl->AddRect(body_tl, body_br, kNerv.line, 0.0f, 0, 1.0f);

        // Title source = game folder name (basename of GetExeDir).
        // That's the directory the user recognizes as "the game" in
        // their file browser. clean_label is the *engine* version
        // (e.g. "WonderfulWorld v0.946") which is identical across
        // every clean FM2K install — not useful as a card title.
        std::string title;
        {
            std::string dir = g.GetExeDir();
            size_t slash = dir.find_last_of("/\\");
            title = (slash == std::string::npos) ? dir : dir.substr(slash + 1);
            if (title.empty()) title = gid;
        }
        if (title.size() > 18) {
            title.resize(17);
            title += "\xe2\x80\xa6";
        }
        if (g_font_label) {
            DrawTextF(cdl, g_font_label, 10.0f,
                      ImVec2(cp.x + S(6), cover_br.y + S(6)),
                      kNerv.ink, title.c_str());
        }

        bool subscribed = false;
        for (const auto& r : rooms) { if (r.game_id == gid) { subscribed = true; break; } }
        if (g_font_micro) {
            DrawTextF(cdl, g_font_micro, 8.0f,
                      ImVec2(cp.x + S(6), cover_br.y + S(20)),
                      subscribed ? kNerv.acc : kNerv.faint,
                      subscribed ? "\xe2\x97\x86 IN ROOM"
                                 : "\xe2\x97\x87 ROOM");
        }

        // Hit-target — full card. Click action depends on cursor pos:
        // top-right corner = toggle fav, anywhere else = join. The
        // [★] and [JOIN] hover overlays below are visual hints for
        // each region; they don't need their own InvisibleButtons.
        ImGui::SetCursorScreenPos(cp);
        char btn_id[32]; std::snprintf(btn_id, sizeof(btn_id),
                                       "##browse.card.%zu", i);
        const bool clicked = ImGui::InvisibleButton(btn_id, ImVec2(kCardW, kCardH));
        const bool hovered = ImGui::IsItemHovered();

        // Hot-zone for the star — top-right corner of the cover.
        const ImVec2 fav_min(cover_br.x - S(22), cp.y + S(1));
        const ImVec2 fav_max(cover_br.x - S(1),  cp.y + S(22));

        if (clicked) {
            const ImVec2 mp = ImGui::GetIO().MousePos;
            const bool   on_fav = (mp.x >= fav_min.x && mp.x <= fav_max.x &&
                                   mp.y >= fav_min.y && mp.y <= fav_max.y);
            if (on_fav) {
                ToggleFavorite(s, gid);
            } else {
                if (!subscribed && !s.subscribed_rooms.empty()) {
                    bool present = false;
                    for (const auto& sr : s.subscribed_rooms) {
                        if (sr == gid) { present = true; break; }
                    }
                    if (!present) s.subscribed_rooms.push_back(gid);
                }
                const std::vector<ActiveRoom> rebuilt = BuildActiveRooms(lu);
                for (int k = 0; k < (int)rebuilt.size(); ++k) {
                    if (rebuilt[k].game_id == gid) {
                        s.active_room_index = k;
                        break;
                    }
                }
                s.hub_view = HubView::Lobby;
            }
        }

        // Hover overlays — [★] top-right + [JOIN] bottom-right.
        if (hovered) {
            const ImVec2 mp = ImGui::GetIO().MousePos;
            const bool over_fav = (mp.x >= fav_min.x && mp.x <= fav_max.x &&
                                   mp.y >= fav_min.y && mp.y <= fav_max.y);

            // Star button — filled when fav, outlined when not.
            cdl->AddRectFilled(fav_min, fav_max,
                               over_fav ? IM_COL32(0,0,0,200)
                                        : IM_COL32(0,0,0,140));
            cdl->AddRect(fav_min, fav_max,
                         over_fav ? kNerv.amber : kNerv.line, 0.0f, 0, 1.0f);
            if (g_font_micro) {
                ImU32 star_col = is_fav ? kNerv.amber
                                        : (over_fav ? kNerv.amber : kNerv.ink2);
                const char* star_glyph = is_fav ? "\xe2\x98\x85" : "\xe2\x98\x86"; // ★ / ☆
                DrawTextF(cdl, g_font_micro, 12.0f,
                          ImVec2(fav_min.x + S(4), fav_min.y + S(3)),
                          star_col, star_glyph);
            }

            const char* join_lbl = "JOIN \xe2\x96\xb6";
            const float pt = 9.0f * g_ui_scale;
            ImVec2 jsz = g_font_label
                ? g_font_label->CalcTextSizeA(pt, FLT_MAX, 0.0f, join_lbl)
                : ImVec2(S(40), S(12));
            ImVec2 j_min(cover_br.x - jsz.x - S(12), cover_br.y - S(20));
            ImVec2 j_max(cover_br.x - S(4),          cover_br.y - S(4));
            cdl->AddRectFilled(j_min, j_max, IM_COL32(0xff,0x5a,0x3c,0xc0));
            cdl->AddRect(j_min, j_max, kNerv.acc, 0.0f, 0, 1.0f);
            DrawTextF(cdl, g_font_label, 9.0f,
                      ImVec2(j_min.x + S(4), j_min.y + S(2)),
                      IM_COL32(255,255,255,255), join_lbl);

            // Brighten card border on hover.
            cdl->AddRect(cp, body_br, kNerv.acc, 0.0f, 0, 1.5f);

            // Tooltip — full game name + path.
            ImGui::SetTooltip("%s\n%s",
                              title.c_str(),
                              g.exe_path.c_str());
        }
    }

    // Reserve the vertical extent so the scrollbar sees the grid height.
    const int total_rows = ((int)games.size() + cols - 1) / cols;
    ImGui::SetCursorScreenPos(grid_o);
    ImGui::Dummy(ImVec2(content_w,
                        total_rows * (kCardH + kGap) - kGap));

    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);
}

void DrawMainContent(LauncherUI& lu, ImVec2 origin, ImVec2 size,
                     const std::vector<ActiveRoom>& rooms) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(origin,
                     ImVec2(origin.x + size.x, origin.y + size.y), true);
    auto& s = State();
    switch (s.hub_view) {
        case HubView::Lobby:
            // Lobby is always the in-room view (RULES + chat). When no
            // room is selected, render a minimal "pick a room" cue —
            // user goes to BROWSE for discovery.
            if (State().active_room_index >= 0) {
                DrawInRoomView(lu, origin, size, rooms);
            } else {
                ImDrawList* sdl = ImGui::GetWindowDrawList();
                if (g_font_micro) {
                    DrawTextF(sdl, g_font_micro, 9.0f,
                              ImVec2(origin.x + S(14), origin.y + S(14)),
                              kNerv.acc, "// LOBBY \xc2\xb7 no room selected");
                }
                if (g_font_body) {
                    DrawTextF(sdl, g_font_body, 13.0f,
                              ImVec2(origin.x + S(14), origin.y + S(34)),
                              kNerv.dim,
                              "Pick a room from BROWSE \xe2\x80\xa2 or click a "
                              "game on the LeftRail.");
                }
            }
            break;
        case HubView::Browse:   DrawHomeDashboard(lu, origin, size, rooms); break;
        case HubView::Replays:  DrawReplaysView(lu, origin, size); break;
        case HubView::Rankings: DrawStubView(origin, size, "RANK",
                                             "Per-game ELO ladder. Needs hub leaderboard endpoint.",
                                             /*backend_pending=*/true); break;
        case HubView::Stats:    DrawStatsView(lu, origin, size); break;
        case HubView::Events:   DrawStubView(origin, size, "EVENTS",
                                             "Tournament feed. Needs hub /api/events or external source.",
                                             /*backend_pending=*/true); break;
        case HubView::Profile:  DrawProfileView(lu, origin, size); break;
        case HubView::Config:   DrawConfigView(lu, origin, size); break;
        case HubView::Map:      DrawMapView(lu, origin, size); break;
        default:                break;
    }
    dl->PopClipRect();
}

// ─── LISSAJOUS ───────────────────────────────────────────────────────
// Orbiting dot tracing a 3:2 (or arbitrary a:b) Lissajous curve, with a
// fading polyline tail. Ported from motion.jsx::Lissajous. The trail
// is a fixed-size ring buffer of normalized (-1..1) sample points.
// One persistent instance is fine for our single-use SplashV2 corner
// graphic; expand to per-id state if/when more on-screen at once.
constexpr int kLissajousTrail = 120;
struct LissajousState {
    float x[kLissajousTrail] = {0};
    float y[kLissajousTrail] = {0};
    int   head   = 0;
    int   filled = 0;
};
LissajousState g_lissajous;

void DrawLissajous(ImVec2 origin, float w, float h,
                   float a, float b, float speed, ImU32 color,
                   int trail = 120, float dot_radius = 2.0f,
                   float line_width = 1.5f) {
    if (trail > kLissajousTrail) trail = kLissajousTrail;
    if (trail < 2) trail = 2;
    const float t = (float)ImGui::GetTime() * speed;
    // π/4 phase offset between axes — gives the classic open-curve look.
    const float nx = std::sin(a * t);
    const float ny = std::sin(b * t + 0.7853982f);
    g_lissajous.x[g_lissajous.head] = nx;
    g_lissajous.y[g_lissajous.head] = ny;
    g_lissajous.head = (g_lissajous.head + 1) % trail;
    if (g_lissajous.filled < trail) ++g_lissajous.filled;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 c(origin.x + w * 0.5f, origin.y + h * 0.5f);
    const float  r = std::min(w, h) * 0.45f;

    const ImVec4 base = ImGui::ColorConvertU32ToFloat4(color);
    for (int i = 1; i < g_lissajous.filled; ++i) {
        const int ia = (g_lissajous.head - i + trail) % trail;
        const int ib = (g_lissajous.head - i - 1 + trail) % trail;
        if (ib < 0) break;
        const float fade = 1.0f - (float)i / (float)trail;
        ImVec4 c4 = base; c4.w *= fade;
        const ImU32 col = ImGui::ColorConvertFloat4ToU32(c4);
        const ImVec2 pa(c.x + g_lissajous.x[ia] * r, c.y + g_lissajous.y[ia] * r);
        const ImVec2 pb(c.x + g_lissajous.x[ib] * r, c.y + g_lissajous.y[ib] * r);
        dl->AddLine(pa, pb, col, line_width);
    }
    if (g_lissajous.filled > 0) {
        const int idx = (g_lissajous.head - 1 + trail) % trail;
        const ImVec2 head_pos(c.x + g_lissajous.x[idx] * r,
                              c.y + g_lissajous.y[idx] * r);
        dl->AddCircleFilled(head_pos, dot_radius, color);
    }
}

// ─── ENERGY FIELD ────────────────────────────────────────────────────
// Drifting particle backdrop. Each particle has an independent advancing
// rng-state so respawns spread freshly across the canvas instead of
// snapping back into the same y-lane (the bug in the original port —
// it derived respawn-y from the per-particle seed, so particles formed
// a few horizontal tracks). Count + intensity + speed range live in
// g_tune for live tweaking.
void DrawEnergyField(ImVec2 origin, ImVec2 size, ImU32 base_col,
                     float intensity_override = -1.0f) {
    const int count = std::max(1, std::min(g_tune.field_count, kEnergyParticleCap));
    const float intensity = (intensity_override >= 0.0f)
                            ? intensity_override
                            : g_tune.field_intensity;

    // Cheap per-particle RNG. xorshift32 keeps state in p.seed reinterpreted
    // as a uint32 — independent stream per particle, advances every respawn,
    // so respawn-y is genuinely random instead of locked to a per-id lane.
    auto next_rand = [](uint32_t& s) -> float {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        return (float)(s & 0x00FFFFFF) / (float)0x01000000;  // [0,1)
    };

    const float speed_span = std::max(0.0f,
        g_tune.field_speed_max - g_tune.field_speed_min);
    const float size_span  = std::max(0.0f,
        g_tune.field_size_max - g_tune.field_size_min);

    if (!g_energy_seeded || g_energy_seed_cnt != count) {
        g_energy_seeded   = true;
        g_energy_seed_cnt = count;
        for (int i = 0; i < count; ++i) {
            EnergyParticle& p = g_energy[i];
            uint32_t s = (uint32_t)(i * 2654435761u) ^ 0x9E3779B9u;
            // Advance a couple of times so first sample isn't biased.
            (void)next_rand(s); (void)next_rand(s);
            p.x      = origin.x + next_rand(s) * size.x;
            p.y      = origin.y + next_rand(s) * size.y;
            p.vx     = g_tune.field_speed_min + next_rand(s) * speed_span;
            p.vy     = 0.0f;
            p.life   = 0.4f + 0.6f * next_rand(s);
            p.radius = g_tune.field_size_min + next_rand(s) * size_span;
            // Stash rng state in seed (float bit-cast). Reused as state
            // for subsequent respawns + the y-wobble phase via a fract.
            std::memcpy(&p.seed, &s, sizeof(float));
        }
    }

    const float dt = ImGui::GetIO().DeltaTime;
    const float t  = (float)ImGui::GetTime();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    ImVec4 base = ImGui::ColorConvertU32ToFloat4(base_col);

    for (int i = 0; i < count; ++i) {
        EnergyParticle& p = g_energy[i];
        uint32_t s; std::memcpy(&s, &p.seed, sizeof(uint32_t));

        // Advance — x drift + sin y wobble (phase derived from rng-state
        // bits so siblings decorrelate), life decay.
        const float wobble_phase = (float)((s >> 8) & 0xFFFF) * 0.0001f;
        p.x   += p.vx * dt;
        p.y   += std::sin(t * 1.4f + wobble_phase) * g_tune.field_wobble_amp * dt;
        p.life -= dt * g_tune.field_life_decay;

        // Respawn — life expired, drifted off-canvas to the right, or
        // user resized the splash and the particle is now outside the
        // vertical bounds.
        const bool off_right  = p.x > origin.x + size.x + 8.0f;
        const bool off_v      = p.y < origin.y - 8.0f ||
                                p.y > origin.y + size.y + 8.0f;
        if (p.life <= 0.0f || off_right || off_v) {
            p.x      = origin.x - 8.0f;
            p.y      = origin.y + next_rand(s) * size.y;
            p.vx     = g_tune.field_speed_min + next_rand(s) * speed_span;
            p.life   = 0.5f + 0.5f * next_rand(s);
            p.radius = g_tune.field_size_min + next_rand(s) * size_span;
            std::memcpy(&p.seed, &s, sizeof(float));
        } else {
            // Persist advanced rng state so the next respawn keeps moving.
            std::memcpy(&p.seed, &s, sizeof(float));
        }

        // Render — alpha modulated by life × intensity × depth (smaller
        // particles are dimmer so the size variance reads as parallax).
        const float depth = (size_span > 0.0f)
            ? 0.55f + 0.45f * ((p.radius - g_tune.field_size_min) / size_span)
            : 1.0f;
        const float a = p.life * intensity * depth;
        ImVec4 c = base; c.w *= a;
        const ImU32 col = ImGui::ColorConvertFloat4ToU32(c);
        dl->AddCircleFilled(ImVec2(p.x, p.y), p.radius, col, 6);
    }
}

bool ShellSkipPressed() {
    // Defer to whatever ImGui widget owns keyboard input first — the
    // tune panel + Setup wizard text fields would otherwise eat
    // Enter/Space and bounce the user out of the route they were
    // editing in.
    if (ImGui::GetIO().WantCaptureKeyboard) return false;
    return ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
           ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false) ||
           ImGui::IsKeyPressed(ImGuiKey_Space, false) ||
           ImGui::IsKeyPressed(ImGuiKey_Escape, false);
}


// ─── SETUP V2 ─ ShellRoute::Setup (M6.2) ─────────────────────────────
// Unified setup wizard, replaces the M2 SetupController/Subscribe pair.
// 4 substeps in this order (per user feedback):
//   0  CONTROLS    — controller binding (FM2KInputBinder wrap)
//   1  GAME DIRS   — root-folder picker for game discovery
//   2  NETWORK     — STUN host visibility + auto-port confirmation
//   3  FINISH      — summary + "ENTER HUB"
// Sidebar lets the user click any step to jump. Progress strip at the
// top fills as steps complete. ENTER HUB on step 3 calls
// MarkSetupComplete and routes to Hub.

struct SetupStepDef {
    const char* k;       // step name
    const char* sub;     // one-line subtitle
    const char* hot;     // hotkey hint label
};
constexpr SetupStepDef kSetupSteps[] = {
    { "CONTROLS",  "Bind controller / keyboard",  "1" },
    { "GAME DIRS", "Locate game install folders", "2" },
    { "NETWORK",   "STUN host \xc2\xb7 auto-port",   "3" },
    { "FINISH",    "Confirm \xc2\xb7 enter hub",     "4" },
};
constexpr int kSetupStepCount = (int)(sizeof(kSetupSteps) / sizeof(kSetupSteps[0]));

// Step 0: CONTROLS — wraps FM2KInputBinder body inside the wizard chrome.
void RenderSetupStepControls(LauncherUI& lu, ImVec2 origin, ImVec2 size) {
    static bool s_inited = false;
    if (!s_inited) {
        s_inited = true;
        FM2KInputBinder::Init();
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "shell: FM2KInputBinder::Init() (Setup wizard step Controls)");
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();

    DrawTextF(dl, g_font_display, 22.0f,
              ImVec2(origin.x, origin.y + 4), kNerv.ink,
              "Bind your controller");
    DrawTextF(dl, g_font_label, 11.0f,
              ImVec2(origin.x, origin.y + 32), kNerv.ink2,
              "Map Player 1 inputs. Click a row, then press the binding "
              "to assign.");

    // Embed the binder body in a bordered child filling the body area.
    const float top_inset = 60;
    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + top_inset));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kNerv.bg2);
    ImGui::PushStyleColor(ImGuiCol_Border,  kNerv.line);
    ImGui::BeginChild("##setupv2.controls",
                      ImVec2(size.x, std::max(80.0f, size.y - top_inset - 4)),
                      ImGuiChildFlags_Borders);
    if (FM2KInputBinder::RenderBody(/*player_slot=*/0)) {
        FM2KInputBinder::Save();
    }
    ImGui::EndChild();
    ImGui::PopStyleColor(2);
    (void)lu;
}

// Step 1: GAME DIRS — list current games-root paths + add/remove.
// Uses the existing on_games_folders_set callback to persist.
void RenderSetupStepGameDirs(LauncherUI& lu, ImVec2 origin, ImVec2 size) {
    auto& s = State();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    DrawTextF(dl, g_font_display, 22.0f,
              ImVec2(origin.x, origin.y + 4), kNerv.ink,
              "Where are your games?");
    DrawTextF(dl, g_font_label, 11.0f,
              ImVec2(origin.x, origin.y + 32), kNerv.ink2,
              "Add the parent folder(s) that contain your FM2K / FM95 "
              "game installs.");

    const float top_inset = 60;
    const float bottom_inset = 36;
    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + top_inset));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kNerv.bg2);
    ImGui::PushStyleColor(ImGuiCol_Border,  kNerv.line);
    ImGui::BeginChild("##setupv2.dirs",
                      ImVec2(size.x, std::max(80.0f,
                                              size.y - top_inset - bottom_inset)),
                      ImGuiChildFlags_Borders);

    // Make a working copy so we can mutate; commit via callback only on
    // explicit save/remove.
    std::vector<std::string> paths = lu.games_root_paths();
    int remove_idx = -1;
    for (size_t i = 0; i < paths.size(); ++i) {
        ImGui::PushID((int)i);
        ImGui::PushStyleColor(ImGuiCol_Text,
                              ImGui::ColorConvertU32ToFloat4(kNerv.ink2));
        ImGui::TextUnformatted(paths[i].c_str());
        ImGui::PopStyleColor();
        ImGui::SameLine(0, 12);
        if (ImGui::SmallButton("remove")) {
            remove_idx = (int)i;
        }
        ImGui::PopID();
    }
    if (paths.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text,
                              ImGui::ColorConvertU32ToFloat4(kNerv.dim));
        ImGui::TextUnformatted("(no folders configured yet)");
        ImGui::PopStyleColor();
    }
    if (remove_idx >= 0) {
        paths.erase(paths.begin() + remove_idx);
        if (lu.on_games_folders_set) lu.on_games_folders_set(paths);
    }

    ImGui::EndChild();
    ImGui::PopStyleColor(2);

    // Add-row bar at the bottom
    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + size.y - 28));
    ImGui::SetNextItemWidth(size.x - 100);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, kNerv.bg2);
    ImGui::PushStyleColor(ImGuiCol_Border, kNerv.line);
    ImGui::PushStyleColor(ImGuiCol_Text,
                          ImGui::ColorConvertU32ToFloat4(kNerv.ink));
    const bool entered = ImGui::InputTextWithHint(
        "##setupv2.dirs.add",
        "C:\\path\\to\\games",
        s.setup_new_path_buf,
        sizeof(s.setup_new_path_buf),
        ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::PopStyleColor(3);
    ImGui::SameLine(0, 6);
    const bool add_clicked = ImGui::Button("ADD", ImVec2(80, 22));
    if ((entered || add_clicked) && s.setup_new_path_buf[0]) {
        std::string p(s.setup_new_path_buf);
        // Trim trailing whitespace.
        while (!p.empty() && (p.back() == ' ' || p.back() == '\r' ||
                              p.back() == '\n' || p.back() == '\t')) {
            p.pop_back();
        }
        if (!p.empty()) {
            paths.push_back(p);
            if (lu.on_games_folders_set) lu.on_games_folders_set(paths);
            s.setup_new_path_buf[0] = '\0';
        }
    }
}

// Step 2: NETWORK — read-only STUN/host visibility for now. Real
// editing UI lives in the legacy Settings panel; M6.4 polish can wire
// it inline.
void RenderSetupStepNet(LauncherUI& lu, ImVec2 origin, ImVec2 size) {
    ImDrawList* dl = ImGui::GetWindowDrawList();

    DrawTextF(dl, g_font_display, 22.0f,
              ImVec2(origin.x, origin.y + 4), kNerv.ink,
              "Network");
    DrawTextF(dl, g_font_label, 11.0f,
              ImVec2(origin.x, origin.y + 32), kNerv.ink2,
              "Hub + STUN auto-detected. UDP port picked at connect time.");

    float ry = origin.y + 70;
    auto kv = [&](const char* k, const char* v, ImU32 vc = 0) {
        if (!vc) vc = kNerv.ink;
        DrawTextF(dl, g_font_micro, 9.0f,
                  ImVec2(origin.x, ry), kNerv.dim, k);
        DrawTextF(dl, g_font_label, 11.0f,
                  ImVec2(origin.x + 100, ry - 1), vc, v);
        ry += 22;
    };
    kv("HUB",          "hub.2dfm.org",   kNerv.ink);
    kv("PROTOCOL",     "WSS / 443",       kNerv.ink);
    kv("LOCAL PORT",   "auto-pick",       kNerv.ink2);
    kv("STUN",         "relay-tokyo:3478", kNerv.ink);
    kv("STATUS",       lu.hub_connected() ? "ONLINE" : "OFFLINE",
       lu.hub_connected() ? kNerv.phos : kNerv.dim);

    ry += 10;
    DrawTextF(dl, g_font_label, 10.0f,
              ImVec2(origin.x, ry), kNerv.faint,
              "These defaults work for most setups. Tweak in Settings if needed.");
    (void)size;
}

// Step 3: FINISH — summary + ENTER HUB.
void RenderSetupStepFinish(LauncherUI& lu, ImVec2 origin, ImVec2 size) {
    auto& s = State();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    DrawTextF(dl, g_font_display, 22.0f,
              ImVec2(origin.x, origin.y + 4), kNerv.ink,
              "All set.");
    DrawTextF(dl, g_font_label, 11.0f,
              ImVec2(origin.x, origin.y + 32), kNerv.ink2,
              "Quick summary of your first-run config.");

    float ry = origin.y + 64;
    auto row = [&](const char* k, const char* v, ImU32 vc) {
        DrawTextF(dl, g_font_micro, 9.0f,
                  ImVec2(origin.x, ry), kNerv.dim, k);
        DrawTextF(dl, g_font_label, 11.0f,
                  ImVec2(origin.x + 120, ry - 1), vc, v);
        ry += 20;
    };
    char nick[80];
    if (lu.discord_signed_in()) {
        std::snprintf(nick, sizeof(nick), "%s",
                      lu.discord_nick().empty() ? "(default)"
                                               : lu.discord_nick().c_str());
        row("IDENTITY", nick, kNerv.ink);
    } else {
        row("IDENTITY", "OFFLINE \xe2\x80\x94 sign in later", kNerv.faint);
    }
    char paths_str[64];
    std::snprintf(paths_str, sizeof(paths_str),
                  "%zu folder%s", lu.games_root_paths().size(),
                  lu.games_root_paths().size() == 1 ? "" : "s");
    row("GAME DIRS", paths_str, kNerv.ink);

    char rooms_str[64];
    std::snprintf(rooms_str, sizeof(rooms_str),
                  "%zu subscribed",
                  s.subscribed_rooms.empty() ? lu.games().size()
                                             : s.subscribed_rooms.size());
    row("ROOMS",   rooms_str, kNerv.ink);
    row("HUB",     "hub.2dfm.org \xc2\xb7 WSS", kNerv.ink2);
    row("STATUS",  lu.hub_connected() ? "READY" : "OFFLINE",
        lu.hub_connected() ? kNerv.phos : kNerv.dim);

    // ENTER HUB button — bottom-right
    const float by = origin.y + size.y - 36;
    ImGui::SetCursorScreenPos(ImVec2(origin.x + size.x - 200, by));
    ImGui::PushStyleColor(ImGuiCol_Button,        IM_COL32(0xff, 0x6a, 0x00, 0x80));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(0xff, 0x6a, 0x00, 0xc0));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  IM_COL32(0xd6, 0x55, 0x00, 0xff));
    ImGui::PushStyleColor(ImGuiCol_Text,          IM_COL32(0xff, 0xff, 0xff, 0xff));
    if (ImGui::Button("ENTER HUB \xe2\x86\x92", ImVec2(180, 30))) {
        MarkSetupComplete(s);
        s.route = ShellRoute::Completion;
        s.route_entered_at = ImGui::GetTime();
        ++s.transition_seq;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "shell: route Setup -> Completion (setup complete)");
    }
    ImGui::PopStyleColor(4);
    (void)lu;
}

void RenderSetupV2(LauncherUI& lu, ImVec2 origin, ImVec2 avail) {
    auto& s = State();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 br(origin.x + avail.x, origin.y + avail.y);
    dl->AddRectFilled(origin, br, kNerv.bg);

    // Clamp step in case of stale state.
    if (s.setup_step < 0) s.setup_step = 0;
    if (s.setup_step >= kSetupStepCount) s.setup_step = kSetupStepCount - 1;

    // ── Header band ─────────────────────────────────────────
    {
        DrawTextF(dl, g_font_micro, 9.0f,
                  ImVec2(origin.x + S(14), origin.y + S(8)), kNerv.acc,
                  "02.SETUP");
        char header[64];
        std::snprintf(header, sizeof(header), "STEP %d OF %d \xc2\xb7 %s",
                      s.setup_step + 1, kSetupStepCount,
                      kSetupSteps[s.setup_step].k);
        DrawTextF(dl, g_font_label, 10.0f,
                  ImVec2(origin.x + S(80), origin.y + S(8)), kNerv.dim, header);
        char stepfrac[16];
        std::snprintf(stepfrac, sizeof(stepfrac), "%d/%d",
                      s.setup_step + 1, kSetupStepCount);
        const float pt = 11.0f * g_ui_scale;
        ImVec2 sz = g_font_label
            ? g_font_label->CalcTextSizeA(pt, FLT_MAX, 0.0f, stepfrac)
            : ImGui::CalcTextSize(stepfrac);
        DrawTextF(dl, g_font_label, 11.0f,
                  ImVec2(br.x - sz.x - S(14), origin.y + S(6)), kNerv.acc, stepfrac);
        DrawTextF(dl, g_font_micro, 9.0f,
                  ImVec2(br.x - sz.x - S(14) - S(60), origin.y + S(8)),
                  kNerv.dim, "WIZARD");
        dl->AddLine(ImVec2(origin.x, origin.y + S(26)),
                    ImVec2(br.x, origin.y + S(26)), kNerv.line, 1.0f);
    }
    // ── Progress strip (3 px) ──
    {
        const float strip_y = origin.y + S(27);
        const float strip_h = S(3);
        const float seg_w = avail.x / (float)kSetupStepCount;
        for (int i = 0; i < kSetupStepCount; ++i) {
            ImVec2 a(origin.x + seg_w * i, strip_y);
            ImVec2 b(origin.x + seg_w * (i + 1), strip_y + strip_h);
            const ImU32 col = (i <= s.setup_step) ? kNerv.acc : kNerv.bg2;
            dl->AddRectFilled(a, b, col);
            if (i < kSetupStepCount - 1) {
                dl->AddRectFilled(ImVec2(b.x - 1, strip_y),
                                  ImVec2(b.x, strip_y + strip_h),
                                  IM_COL32(0,0,0,0xff));
            }
        }
    }

    // ── Two-col body ──
    const float kSidebarW = S(132.0f);
    const float top_inset = S(30) + S(4);
    const float content_y = origin.y + top_inset;
    const float content_h = avail.y - top_inset;
    const float right_x = origin.x + kSidebarW;
    const float right_w = avail.x - kSidebarW;
    dl->AddLine(ImVec2(right_x, content_y),
                ImVec2(right_x, content_y + content_h),
                kNerv.line, 1.0f);

    // Sidebar — clickable step list
    const float sidebar_row_h = S(56);
    for (int i = 0; i < kSetupStepCount; ++i) {
        const auto& st = kSetupSteps[i];
        const bool active = (i == s.setup_step);
        const bool done   = (i <  s.setup_step);
        ImVec2 rp(origin.x, content_y + S(6) + i * sidebar_row_h);
        ImVec2 r1(rp.x + kSidebarW, rp.y + sidebar_row_h);
        if (active) {
            dl->AddRectFilled(rp, r1, kNerv.bg2);
            dl->AddRectFilled(rp, ImVec2(rp.x + S(2), r1.y), kNerv.acc);
        } else if (done) {
            dl->AddRectFilled(rp, ImVec2(rp.x + S(2), r1.y), kNerv.line);
        }

        ImGui::SetCursorScreenPos(rp);
        char btn_id[24];
        std::snprintf(btn_id, sizeof(btn_id), "##setupv2.step.%d", i);
        if (ImGui::InvisibleButton(btn_id, ImVec2(kSidebarW, sidebar_row_h))) {
            s.setup_step = i;
        }

        char idx[8];
        std::snprintf(idx, sizeof(idx), "%s",
                      done ? "\xe2\x9c\x93" : (active ? "\xe2\x96\xb6" : "\xe2\x97\x8b"));
        DrawTextF(dl, g_font_micro, 9.0f,
                  ImVec2(rp.x + S(12), rp.y + S(8)),
                  active ? kNerv.acc : (done ? kNerv.phos : kNerv.faint), idx);
        DrawTextF(dl, g_font_label, 11.0f,
                  ImVec2(rp.x + S(28), rp.y + S(8)),
                  active ? kNerv.ink : (done ? kNerv.ink2 : kNerv.dim),
                  st.k);
        DrawTextF(dl, g_font_label, 9.0f,
                  ImVec2(rp.x + S(12), rp.y + S(26)),
                  kNerv.faint, st.sub);
        char hot[8]; std::snprintf(hot, sizeof(hot), "[%s]", st.hot);
        DrawTextF(dl, g_font_label, 9.0f,
                  ImVec2(rp.x + kSidebarW - S(26), rp.y + S(8)),
                  kNerv.faint, hot);
    }

    const float body_pad = S(14.0f);
    ImVec2 body_origin(right_x + body_pad, content_y + body_pad);
    ImVec2 body_size(right_w - body_pad * 2, content_h - body_pad * 2);
    switch (s.setup_step) {
    case 0: RenderSetupStepControls(lu, body_origin, body_size); break;
    case 1: RenderSetupStepGameDirs(lu, body_origin, body_size); break;
    case 2: RenderSetupStepNet     (lu, body_origin, body_size); break;
    case 3: RenderSetupStepFinish  (lu, body_origin, body_size); break;
    default: break;
    }

    // Footer row — back / next buttons (skip on step 3, FINISH has its
    // own "ENTER HUB" button).
    if (s.setup_step < kSetupStepCount - 1) {
        const float by = br.y - S(30);
        if (s.setup_step > 0) {
            ImGui::SetCursorScreenPos(ImVec2(right_x + S(14), by));
            if (ImGui::Button("\xe2\x86\xa9 BACK", ImVec2(S(80), S(22)))) {
                --s.setup_step;
            }
        }
        ImGui::SetCursorScreenPos(ImVec2(br.x - S(110), by));
        ImGui::PushStyleColor(ImGuiCol_Button,        IM_COL32(0xff,0x6a,0x00,0x60));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(0xff,0x6a,0x00,0xa0));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  IM_COL32(0xff,0x6a,0x00,0xe0));
        ImGui::PushStyleColor(ImGuiCol_Text,          IM_COL32(0xff,0xff,0xff,0xff));
        if (ImGui::Button("NEXT \xe2\x86\x92", ImVec2(S(96), S(22)))) {
            ++s.setup_step;
        }
        ImGui::PopStyleColor(4);
    } else if (s.setup_step == kSetupStepCount - 1) {
        ImGui::SetCursorScreenPos(ImVec2(right_x + S(14), br.y - S(30)));
        if (ImGui::Button("\xe2\x86\xa9 BACK", ImVec2(S(80), S(22)))) {
            --s.setup_step;
        }
    }
}

// ─── LOGIN V2 ─ ShellRoute::Login (M6.1, inline OAuth M6.1.2) ────────
// Drives the Discord OAuth pairing INLINE — does not delegate to the
// legacy floating RenderDiscordAuthWindow modal. We own a Pairing
// instance, poll its status each frame, and render the appropriate
// idle / pending / browser_failed / ok state directly inside the
// wizard's right card. On Ok we SaveCached + ping the launcher so its
// mirror state (lu.discord_signed_in_) refreshes.

namespace {  // login-only state
std::unique_ptr<fm2k::discord_auth::Pairing> g_login_pairing;
double      g_login_started_at = 0.0;
bool        g_login_clipboard_dropped = false;  // one-shot copy on browser fail
}  // namespace
void RenderLoginV2(LauncherUI& lu, ImVec2 origin, ImVec2 avail) {
    auto& s = State();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 br(origin.x + avail.x, origin.y + avail.y);
    dl->AddRectFilled(origin, br, kNerv.bg);

    constexpr ImU32 kMagenta       = IM_COL32(0xd6, 0x33, 0x84, 0xff);
    constexpr ImU32 kMagentaSoft   = IM_COL32(0xd6, 0x33, 0x84, 0xc0);
    constexpr ImU32 kMagentaHover  = IM_COL32(0xff, 0x4a, 0x98, 0xff);
    constexpr ImU32 kMagentaActive = IM_COL32(0xb5, 0x28, 0x70, 0xff);

    const bool signed_in = lu.discord_signed_in();

    // Poll inline Pairing every frame. On Ok, persist + tell launcher
    // to refresh its mirror state, then drop the instance. On Expired
    // / Error, leave the instance so the UI can render the failure
    // state until the user clicks AUTHORIZE again (which replaces it).
    if (g_login_pairing) {
        const auto st = g_login_pairing->status();
        if (st == fm2k::discord_auth::Pairing::Status::Ok) {
            const auto res = g_login_pairing->result();
            fm2k::discord_auth::SaveCached(res);
            lu.NotifyDiscordCachePopulated();
            // Pre-populate nick UI from the just-saved cache.
            s.login_use_discord_name = res.use_discord_name;
            std::snprintf(s.login_custom_nick, sizeof(s.login_custom_nick),
                          "%s", res.nick.c_str());
            s.login_state_loaded = true;
            g_login_pairing.reset();
            g_login_clipboard_dropped = false;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "shell: LoginV2 inline OAuth Ok (uid=%s)",
                        res.discord_user_id.c_str());
        }
    }
    const bool pairing_pending = (g_login_pairing &&
        g_login_pairing->status() == fm2k::discord_auth::Pairing::Status::Pending);
    const bool browser_failed = (g_login_pairing &&
        g_login_pairing->browser_open_failed());
    const bool pairing_error = (g_login_pairing &&
        (g_login_pairing->status() == fm2k::discord_auth::Pairing::Status::Expired ||
         g_login_pairing->status() == fm2k::discord_auth::Pairing::Status::Error));

    // Lazy-load nick UI state from cache when entering Login.
    if (!s.login_state_loaded) {
        s.login_state_loaded = true;
        const auto cached = fm2k::discord_auth::LoadCached();
        if (cached.valid) {
            s.login_use_discord_name = cached.use_discord_name;
            std::snprintf(s.login_custom_nick, sizeof(s.login_custom_nick),
                          "%s", cached.nick.c_str());
        }
    }

    // Two-column grid: ~260-px (scaled) left context rail + right card.
    const float left_w = std::min(avail.x * 0.4f, S(260.0f));
    const float left_x = origin.x;
    const float right_x = origin.x + left_w;
    const float right_w = avail.x - left_w;

    // ── LEFT RAIL ───────────────────────────────────────────
    dl->AddLine(ImVec2(right_x, origin.y), ImVec2(right_x, br.y),
                kNerv.line, 1.0f);
    {
        float ly = origin.y + S(16);
        DrawTextF(dl, g_font_micro, 9.0f,
                  ImVec2(left_x + S(14), ly), kMagenta, "Why Discord");
        ly += S(16);
        DrawTextF(dl, g_font_label, 10.0f,
                  ImVec2(left_x + S(14), ly), kNerv.ink2,
                  "The hub uses your Discord identity to gate");
        ly += S(12);
        DrawTextF(dl, g_font_label, 10.0f,
                  ImVec2(left_x + S(14), ly), kNerv.ink2,
                  "matchmaking and assign roles.");
        ly += S(14);
        DrawTextF(dl, g_font_label, 9.0f,
                  ImVec2(left_x + S(14), ly), kNerv.faint,
                  "No password is stored on this device.");
        ly += S(16);
        dl->AddLine(ImVec2(left_x + S(14), ly),
                    ImVec2(left_x + left_w - S(14), ly), kNerv.line, 1.0f);
        ly += S(12);

        DrawTextF(dl, g_font_micro, 9.0f,
                  ImVec2(left_x + S(14), ly), kNerv.dim, "Pairing protocol");
        ly += S(14);
        struct Step { const char* title; const char* note; bool done; };
        const Step steps[] = {
            { "POST /pair/begin",      "hub mints code + url",    signed_in },
            { "ShellExecute browser",  "discord oauth identify",  signed_in },
            { "POLL /pair/{code}",     "token + global_name",     signed_in },
            { "cache \xe2\x86\x92 %APPDATA%", "discord_auth.json",       signed_in },
        };
        for (const auto& st : steps) {
            DrawTextF(dl, g_font_label, 10.0f,
                      ImVec2(left_x + S(22), ly),
                      st.done ? kNerv.phos : kNerv.ink2, st.title);
            DrawTextF(dl, g_font_label, 9.0f,
                      ImVec2(left_x + S(22), ly + S(12)), kNerv.faint, st.note);
            ly += S(26);
        }
    }

    // ── RIGHT CARD ──────────────────────────────────────────
    const float rx = right_x + S(18);
    float ry = origin.y + S(16);

    if (!signed_in && pairing_pending) {
        DrawTextF(dl, g_font_micro, 9.0f,
                  ImVec2(rx, ry),
                  browser_failed ? kNerv.amber : kMagenta,
                  browser_failed ? "BROWSER DID NOT OPEN \xe2\x80\x94 PASTE THE URL"
                                 : "WAITING ON DISCORD");
        ry += S(14);
        DrawTextF(dl, g_font_display, 22.0f,
                  ImVec2(rx, ry), kNerv.ink,
                  browser_failed ? "Paste this URL." : "Browser opened.");
        ry += S(30);
        DrawTextF(dl, g_font_label, 11.0f,
                  ImVec2(rx, ry), kNerv.ink2,
                  browser_failed
                      ? "Common when launcher runs as admin or no http handler is set."
                      : "Click Authorize on Discord and come back to the launcher.");
        ry += S(26);

        // URL row + COPY button. Drop the pair-code display per user
        // feedback (the underlying state= param is internal to the
        // OAuth handshake; users don't need to read it).
        const std::string url = g_login_pairing->authorize_url();
        if (!url.empty()) {
            if (browser_failed && !g_login_clipboard_dropped) {
                ImGui::SetClipboardText(url.c_str());
                g_login_clipboard_dropped = true;
            }
            // COPY button — placed BEFORE the URL box so it doesn't
            // overlap. Themed magenta.
            ImGui::SetCursorScreenPos(ImVec2(rx, ry));
            ImGui::PushStyleColor(ImGuiCol_Button,        kMagentaSoft);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kMagentaHover);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  kMagentaActive);
            ImGui::PushStyleColor(ImGuiCol_Text,
                                  IM_COL32(0xff, 0xff, 0xff, 0xff));
            const bool copied = g_login_clipboard_dropped;
            if (ImGui::Button(copied ? "\xe2\x9c\x93 COPIED"
                                     : "COPY URL",
                              ImVec2(S(110), S(24)))) {
                ImGui::SetClipboardText(url.c_str());
                g_login_clipboard_dropped = true;
            }
            ImGui::PopStyleColor(4);
            ry += S(30);

            const float box_w = right_w - S(36);
            ImVec2 box_p(rx, ry);
            ImGui::PushTextWrapPos(box_p.x + box_w - S(16));
            ImVec2 wrapped_sz = ImGui::CalcTextSize(url.c_str(), nullptr,
                                                   /*hide_double_hash*/true,
                                                   box_w - S(16));
            ImGui::PopTextWrapPos();
            const float box_h = std::max(S(28.0f), wrapped_sz.y + S(14.0f));
            ImVec2 box_b(rx + box_w, ry + box_h);

            dl->AddRectFilled(box_p, box_b, IM_COL32(0x0d, 0x05, 0x0b, 0xff));
            dl->AddRect(box_p, box_b,
                        browser_failed ? kNerv.amber : kMagenta,
                        0.0f, 0, 1.0f);
            ImGui::SetCursorScreenPos(ImVec2(box_p.x + S(8), box_p.y + S(6)));
            ImGui::PushStyleColor(ImGuiCol_Text,
                                  ImGui::ColorConvertU32ToFloat4(kNerv.ink));
            ImGui::PushTextWrapPos(box_p.x + box_w - S(8));
            ImGui::TextWrapped("%s", url.c_str());
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();

            ry += box_h + S(8);
        }

        ImGui::SetCursorScreenPos(ImVec2(rx, br.y - S(36)));
        ImGui::PushStyleColor(ImGuiCol_Button,        IM_COL32(0,0,0,0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(0xff,0x30,0x30,0x14));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  IM_COL32(0xff,0x30,0x30,0x28));
        ImGui::PushStyleColor(ImGuiCol_Border,        kNerv.red);
        ImGui::PushStyleColor(ImGuiCol_Text,
                              ImGui::ColorConvertU32ToFloat4(kNerv.red));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
        if (ImGui::Button("CANCEL", ImVec2(S(110), S(26)))) {
            if (g_login_pairing) g_login_pairing->Cancel();
            g_login_pairing.reset();
            g_login_clipboard_dropped = false;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "shell: LoginV2 OAuth cancelled by user");
        }
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(5);
    } else if (!signed_in && pairing_error) {
        const auto st = g_login_pairing->status();
        const std::string detail = g_login_pairing->error_detail();
        DrawTextF(dl, g_font_micro, 9.0f,
                  ImVec2(rx, ry), kNerv.red,
                  st == fm2k::discord_auth::Pairing::Status::Expired
                      ? "PAIRING EXPIRED" : "PAIRING FAILED");
        ry += S(14);
        DrawTextF(dl, g_font_display, 22.0f,
                  ImVec2(rx, ry), kNerv.ink, "Try again.");
        ry += S(30);
        DrawTextF(dl, g_font_label, 11.0f,
                  ImVec2(rx, ry), kNerv.ink2,
                  detail.empty() ? "The pairing code timed out." : detail.c_str());
        ry += S(26);

        ImGui::SetCursorScreenPos(ImVec2(rx, ry));
        ImGui::PushStyleColor(ImGuiCol_Button,        kMagentaSoft);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kMagentaHover);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  kMagentaActive);
        ImGui::PushStyleColor(ImGuiCol_Text,          IM_COL32(0xff,0xff,0xff,0xff));
        if (ImGui::Button("RETRY", ImVec2(S(140), S(28)))) {
            g_login_pairing.reset(fm2k::discord_auth::Begin(lu.hub_base_url()));
            g_login_started_at = ImGui::GetTime();
            g_login_clipboard_dropped = false;
        }
        ImGui::PopStyleColor(4);
    } else if (!signed_in) {
        DrawTextF(dl, g_font_micro, 9.0f,
                  ImVec2(rx, ry), kMagenta, "NOT PAIRED");
        ry += S(14);
        DrawTextF(dl, g_font_display, 26.0f,
                  ImVec2(rx, ry), kNerv.ink, "Sign in with Discord");
        ry += S(36);
        DrawTextF(dl, g_font_label, 11.0f,
                  ImVec2(rx, ry), kNerv.ink2,
                  "We'll open your browser to discord.com to authorize.");
        ry += S(14);
        DrawTextF(dl, g_font_label, 10.0f,
                  ImVec2(rx, ry), kNerv.faint,
                  "If it doesn't open, you can paste the URL manually.");
        ry += S(22);

        ImGui::SetCursorScreenPos(ImVec2(rx, ry));
        ImGui::PushStyleColor(ImGuiCol_Button,        kMagentaSoft);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kMagentaHover);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  kMagentaActive);
        ImGui::PushStyleColor(ImGuiCol_Text,          IM_COL32(0xff, 0xff, 0xff, 0xff));
        if (ImGui::Button("\xe2\x96\xb6  AUTHORIZE WITH DISCORD",
                          ImVec2(S(240), S(32)))) {
            g_login_pairing.reset(fm2k::discord_auth::Begin(lu.hub_base_url()));
            g_login_started_at = ImGui::GetTime();
            g_login_clipboard_dropped = false;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "shell: LoginV2 OAuth started (base=%s)",
                        lu.hub_base_url().c_str());
        }
        ImGui::PopStyleColor(4);

        ry += S(44);
        dl->AddLine(ImVec2(rx, ry),
                    ImVec2(rx + right_w - S(36), ry), kNerv.line, 1.0f);
        ry += S(12);
        DrawTextF(dl, g_font_micro, 9.0f, ImVec2(rx, ry), kNerv.dim, "Privacy");
        ry += S(14);
        DrawTextF(dl, g_font_label, 10.0f, ImVec2(rx, ry), kNerv.ink2,
                  "We read identify only \xc2\xb7 global_name + user_id.");
        ry += S(12);
        DrawTextF(dl, g_font_label, 9.0f, ImVec2(rx, ry), kNerv.faint,
                  "You can swap to a custom handle next \xe2\x80\x94 good for streamers.");
    } else {
        // OK — paired. Welcome + nick picker + CONTINUE.
        const auto cached = fm2k::discord_auth::LoadCached();
        const std::string& discord_name = cached.discord_global_name;
        const std::string& effective_nick =
            s.login_use_discord_name ? discord_name :
            (s.login_custom_nick[0] ? std::string(s.login_custom_nick) :
             std::string("unnamed"));

        DrawTextF(dl, g_font_micro, 9.0f,
                  ImVec2(rx, ry), kNerv.phos,
                  "\xe2\x97\x8f PAIRED \xc2\xb7 TOKEN ISSUED");
        ry += S(14);
        char welcome[128];
        std::snprintf(welcome, sizeof(welcome), "welcome, %s",
                      effective_nick.c_str());
        DrawTextF(dl, g_font_display, 24.0f,
                  ImVec2(rx, ry), kNerv.ink, welcome);
        ry += S(30);
        char sub[128];
        std::snprintf(sub, sizeof(sub),
                      "discord global_name \xc2\xb7 %s",
                      discord_name.c_str());
        DrawTextF(dl, g_font_label, 10.0f,
                  ImVec2(rx, ry), kNerv.faint, sub);
        ry += S(18);
        dl->AddLine(ImVec2(rx, ry),
                    ImVec2(rx + right_w - S(36), ry), kNerv.line, 1.0f);
        ry += S(12);

        DrawTextF(dl, g_font_micro, 9.0f, ImVec2(rx, ry), kMagenta,
                  "How should we display you?");
        ry += S(14);
        DrawTextF(dl, g_font_label, 10.0f, ImVec2(rx, ry), kNerv.dim,
                  "Streamers \xe2\x80\x94 pick a custom handle so your discord");
        ry += S(12);
        DrawTextF(dl, g_font_label, 10.0f, ImVec2(rx, ry), kNerv.dim,
                  "doesn't leak on lobbies / spectators / replay metadata.");
        ry += S(18);

        // Two-card picker (radio-style)
        const float card_w = (right_w - S(36) - S(8)) * 0.5f;
        const float card_h = S(60);
        struct Card {
            const char* tag;
            const char* sub;
            bool        selected;
        };
        // Card 1 — USE DISCORD NAME
        {
            ImVec2 cp(rx, ry);
            ImVec2 c1(cp.x + card_w, cp.y + card_h);
            ImGui::SetCursorScreenPos(cp);
            if (ImGui::InvisibleButton("##login.card1", ImVec2(card_w, card_h))) {
                s.login_use_discord_name = true;
            }
            const ImU32 border = s.login_use_discord_name ? kMagenta : kNerv.line;
            const ImU32 bg     = s.login_use_discord_name ? IM_COL32(0x17,0x0a,0x13,0xff) : IM_COL32(0,0,0,0);
            dl->AddRectFilled(cp, c1, bg);
            dl->AddRect(cp, c1, border, 0.0f, 0, 1.0f);
            const ImU32 pip = s.login_use_discord_name ? kMagenta : IM_COL32(0,0,0,0);
            dl->AddRectFilled(ImVec2(cp.x + S(8), cp.y + S(10)),
                              ImVec2(cp.x + S(14), cp.y + S(16)), pip);
            dl->AddRect(ImVec2(cp.x + S(8), cp.y + S(10)),
                        ImVec2(cp.x + S(14), cp.y + S(16)), border);
            DrawTextF(dl, g_font_micro, 9.0f,
                      ImVec2(cp.x + S(22), cp.y + S(10)),
                      s.login_use_discord_name ? kMagenta : kNerv.dim,
                      "USE DISCORD NAME");
            DrawTextF(dl, g_font_label, 12.0f,
                      ImVec2(cp.x + S(8), cp.y + S(26)),
                      s.login_use_discord_name ? kNerv.ink : kNerv.ink2,
                      discord_name.c_str());
            DrawTextF(dl, g_font_label, 9.0f,
                      ImVec2(cp.x + S(8), cp.y + S(44)),
                      kNerv.faint, "auto-syncs if you rename");
        }
        // Card 2 — CUSTOM HANDLE
        {
            const float c2x = rx + card_w + S(8);
            ImVec2 cp(c2x, ry);
            ImVec2 c1(cp.x + card_w, cp.y + card_h);
            const ImU32 border = !s.login_use_discord_name ? kMagenta : kNerv.line;
            const ImU32 bg     = !s.login_use_discord_name ? IM_COL32(0x17,0x0a,0x13,0xff) : IM_COL32(0,0,0,0);
            dl->AddRectFilled(cp, c1, bg);
            dl->AddRect(cp, c1, border, 0.0f, 0, 1.0f);
            const ImU32 pip = !s.login_use_discord_name ? kMagenta : IM_COL32(0,0,0,0);
            dl->AddRectFilled(ImVec2(cp.x + S(8), cp.y + S(10)),
                              ImVec2(cp.x + S(14), cp.y + S(16)), pip);
            dl->AddRect(ImVec2(cp.x + S(8), cp.y + S(10)),
                        ImVec2(cp.x + S(14), cp.y + S(16)), border);
            DrawTextF(dl, g_font_micro, 9.0f,
                      ImVec2(cp.x + S(22), cp.y + S(10)),
                      !s.login_use_discord_name ? kMagenta : kNerv.dim,
                      "CUSTOM HANDLE \xc2\xb7 anon");
            ImGui::SetCursorScreenPos(ImVec2(cp.x + S(8), cp.y + S(28)));
            ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0,0,0,0));
            ImGui::PushStyleColor(ImGuiCol_Border, border);
            ImGui::PushStyleColor(ImGuiCol_Text,
                                  ImGui::ColorConvertU32ToFloat4(kNerv.ink));
            ImGui::SetNextItemWidth(card_w - S(16));
            if (ImGui::InputText("##login.custom",
                                 s.login_custom_nick,
                                 sizeof(s.login_custom_nick))) {
                s.login_use_discord_name = false;
            }
            if (ImGui::IsItemActivated()) {
                s.login_use_discord_name = false;
            }
            ImGui::PopStyleColor(3);
            DrawTextF(dl, g_font_label, 9.0f,
                      ImVec2(cp.x + S(8), cp.y + S(48)),
                      kNerv.faint, "only sent to hub");
        }
        ry += S(70);

        // Bottom row — sign-out + CONTINUE
        const float row_y = br.y - S(40);
        ImGui::SetCursorScreenPos(ImVec2(rx, row_y));
        // Sign-out button — outline-only with magenta border to match
        // the rest of LoginV2's chrome.
        ImGui::PushStyleColor(ImGuiCol_Button,        IM_COL32(0,0,0,0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(0xd6,0x33,0x84,0x14));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  IM_COL32(0xd6,0x33,0x84,0x28));
        ImGui::PushStyleColor(ImGuiCol_Border,        kMagenta);
        ImGui::PushStyleColor(ImGuiCol_Text,
                              ImGui::ColorConvertU32ToFloat4(kNerv.faint));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
        if (ImGui::Button("sign out", ImVec2(S(90), S(24)))) {
            fm2k::discord_auth::ClearCached();
            // Refresh launcher mirror state so lu.discord_signed_in()
            // flips false on the next render — without this, the OK
            // card stays visible because the bool is stale.
            lu.NotifyDiscordCachePopulated();
            s.login_state_loaded = false;
            // Reset nick UI buffer so a re-sign-in doesn't carry over
            // the previous user's custom nick.
            s.login_custom_nick[0] = '\0';
            s.login_use_discord_name = true;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "shell: LoginV2 signed out (cache cleared)");
        }
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(5);
        ImGui::SetCursorScreenPos(ImVec2(rx + right_w - S(220), row_y));
        ImGui::PushStyleColor(ImGuiCol_Button,        kMagentaSoft);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kMagentaHover);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  kMagentaActive);
        ImGui::PushStyleColor(ImGuiCol_Text,          IM_COL32(0xff, 0xff, 0xff, 0xff));
        if (ImGui::Button("CONTINUE \xe2\x86\x92 SETUP", ImVec2(S(180), S(28)))) {
            // Persist nick / use_discord_name to cache so the legacy
            // hub Connect path picks them up.
            auto cached_save = fm2k::discord_auth::LoadCached();
            if (cached_save.valid) {
                bool dirty = false;
                std::string custom(s.login_custom_nick);
                if (cached_save.nick != custom) {
                    cached_save.nick = custom;
                    dirty = true;
                }
                if (cached_save.use_discord_name != s.login_use_discord_name) {
                    cached_save.use_discord_name = s.login_use_discord_name;
                    dirty = true;
                }
                if (dirty) fm2k::discord_auth::SaveCached(cached_save);
            }
            const bool wiz = ShouldRunWizard(s);
            s.route = wiz ? ShellRoute::Setup : ShellRoute::Hub;
            if (wiz) s.setup_step = 0;
            s.route_entered_at = ImGui::GetTime();
            ++s.transition_seq;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "shell: route Login -> %s",
                        wiz ? "Setup step 0 (Controls)" : "Hub");
        }
        ImGui::PopStyleColor(4);
    }
}

// ─── SPLASH V2 ─ ShellRoute::Splash (M6.0) ───────────────────────────
// Replaces the M2 BootCrawl + LogoIntro pair with a single screen.
// EnergyField backdrop, Lissajous in the top-right, big "2DFM
// forever." title, build column + PRESS TO ENTER pill at the bottom.
// Auto-advance shows a visible countdown (8s) — user can hit
// Enter / Space / Esc / click the pill to advance early. After
// advance: routes to Login if Discord cache is missing, else either
// Setup (first-run) or Hub (returning user).
// ─── COMPLETION V2 ─ ShellRoute::Completion (M6.4) ───────────────────
// Brief interstitial that fires after Setup wizard finishes. Reads as
// "you're all set" with a hairline + nick + auto-advance pill, mirrors
// the splash/login visual language. Auto-advances to Hub after a few
// seconds; user can hit Enter/Space to skip.
void RenderCompletionV2(LauncherUI& lu, ImVec2 origin, ImVec2 avail) {
    auto& s = State();
    const float elapsedMs = (float)((ImGui::GetTime() - s.route_entered_at) * 1000.0);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 br(origin.x + avail.x, origin.y + avail.y);
    dl->AddRectFilled(origin, br, kNerv.bg);

    // Light backdrop — same EnergyField as Splash but dimmer so the
    // foreground content reads more clearly than the splash hero.
    DrawEnergyField(origin, avail, kNerv.acc, g_tune.field_intensity * 0.6f);

    auto fade_alpha = [&](float delay_ms, float dur_ms) -> float {
        if (elapsedMs < delay_ms) return 0.0f;
        float u = (elapsedMs - delay_ms) / dur_ms;
        if (u > 1.0f) u = 1.0f;
        const float inv = 1.0f - u;
        return 1.0f - inv * inv * inv;
    };
    auto faded = [&](ImU32 col, float a) -> ImU32 {
        ImVec4 c = ImGui::ColorConvertU32ToFloat4(col); c.w *= a;
        return ImGui::ColorConvertFloat4ToU32(c);
    };

    const float content_x = origin.x + S(22.0f);

    // Eyebrow
    {
        const float a = fade_alpha(60, 400);
        if (a > 0.001f) {
            DrawTextF(dl, g_font_micro, 9.0f,
                      ImVec2(content_x, origin.y + S(24)),
                      faded(kNerv.acc, a), "SETUP COMPLETE");
        }
    }
    // Hairline
    {
        const float a = fade_alpha(180, 400);
        if (a > 0.001f) {
            dl->AddLine(ImVec2(content_x, origin.y + S(42)),
                        ImVec2(content_x + S(60.0f) * a, origin.y + S(42)),
                        faded(kNerv.acc, a), 2.0f);
        }
    }
    // Big greeting
    {
        const float a = fade_alpha(320, 600);
        if (a > 0.001f) {
            const std::string nick = lu.discord_signed_in()
                ? lu.discord_nick()
                : (lu.hub_my_nick().empty() ? std::string("FIGHTER")
                                            : lu.hub_my_nick());
            char welcome[128];
            std::snprintf(welcome, sizeof(welcome), "WELCOME,");
            DrawTextF(dl, g_font_display, 38.0f,
                      ImVec2(content_x, origin.y + S(60)),
                      faded(kNerv.ink, a), welcome);
            DrawTextF(dl, g_font_display, 56.0f,
                      ImVec2(content_x, origin.y + S(100)),
                      faded(kNerv.acc, a), nick.c_str());
        }
    }
    // Subtitle
    {
        const float a = fade_alpha(640, 400);
        if (a > 0.001f) {
            DrawTextF(dl, g_font_micro, 9.0f,
                      ImVec2(content_x, origin.y + S(178)),
                      faded(kNerv.ink2, a),
                      "ALL SET \xc2\xb7 LANDING IN HUB \xc2\xb7 GG");
        }
    }

    constexpr int kAutoAdvanceMs = 2500;
    bool advance = false;
    {
        const float a = fade_alpha(800, 400);
        if (a > 0.001f) {
            const int rem_ms = std::max(0, kAutoAdvanceMs - (int)elapsedMs);
            const int rem_s  = (rem_ms + 999) / 1000;
            char auto_str[40];
            std::snprintf(auto_str, sizeof(auto_str),
                          "entering hub in %ds", rem_s);
            const float pt = 9.0f * g_ui_scale;
            ImVec2 asz = g_font_micro
                ? g_font_micro->CalcTextSizeA(pt, FLT_MAX, 0.0f, auto_str)
                : ImGui::CalcTextSize(auto_str);
            DrawTextF(dl, g_font_micro, 9.0f,
                      ImVec2(br.x - S(22) - asz.x, br.y - S(30)),
                      faded(kNerv.faint, a), auto_str);
            if (ShellSkipPressed()) advance = true;
        }
        if (elapsedMs >= kAutoAdvanceMs) advance = true;
    }

    if (advance) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "shell: route Completion -> Hub");
        s.route = ShellRoute::Hub;
        s.route_entered_at = ImGui::GetTime();
        ++s.transition_seq;
    }
}

void RenderSplashV2(LauncherUI& lu, ImVec2 origin, ImVec2 avail) {
    auto& s = State();
    const float elapsedMs = (float)((ImGui::GetTime() - s.route_entered_at) * 1000.0);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 br(origin.x + avail.x, origin.y + avail.y);
    dl->AddRectFilled(origin, br, kNerv.bg);

    // EnergyField backdrop — full-canvas snow/starfield. Density +
    // intensity live in g_tune (Ctrl+` to dial in).
    DrawEnergyField(origin, avail, kNerv.acc);

    // Lissajous top-right
    {
        const float lsz = S(g_tune.lissa_size);
        const ImVec2 lpos(br.x - lsz - S(g_tune.lissa_margin_x),
                          origin.y + S(g_tune.lissa_margin_y));
        DrawLissajous(lpos, lsz, lsz,
                      (float)g_tune.lissa_a,
                      (float)g_tune.lissa_b,
                      g_tune.lissa_speed,
                      kNerv.acc,
                      g_tune.lissa_trail,
                      S(g_tune.lissa_dot_radius),
                      S(g_tune.lissa_line_width));
        char angle_str[32];
        std::snprintf(angle_str, sizeof(angle_str),
                      "%d:%d \xc2\xb7 \xce\xb8 %.0f\xc2\xb0",
                      g_tune.lissa_a, g_tune.lissa_b,
                      std::fmod((float)ImGui::GetTime() * 40.0f, 360.0f));
        const float pt = 8.0f * g_ui_scale;
        ImVec2 sz = g_font_label
            ? g_font_label->CalcTextSizeA(pt, FLT_MAX, 0.0f, angle_str)
            : ImGui::CalcTextSize(angle_str);
        DrawTextF(dl, g_font_label, 8.0f,
                  ImVec2(br.x - S(g_tune.lissa_margin_x) - sz.x,
                         lpos.y + lsz + S(4.0f)),
                  kNerv.faint, angle_str);
    }

    // ── content layout ──
    const float content_x = origin.x + S(22.0f);
    const float content_w = avail.x  - S(44.0f);
    (void)content_w;

    // Per-element fade-in: out-cubic ramp from `delay_ms` over `dur_ms`.
    auto fade_alpha = [&](float delay_ms, float dur_ms) -> float {
        if (elapsedMs < delay_ms) return 0.0f;
        float u = (elapsedMs - delay_ms) / dur_ms;
        if (u > 1.0f) u = 1.0f;
        const float inv = 1.0f - u;
        return 1.0f - inv * inv * inv;
    };
    auto faded = [&](ImU32 col, float a) -> ImU32 {
        ImVec4 c = ImGui::ColorConvertU32ToFloat4(col); c.w *= a;
        return ImGui::ColorConvertFloat4ToU32(c);
    };

    // (80ms) eyebrow
    {
        const float a = fade_alpha(80, 500);
        if (a > 0.001f) {
            DrawTextF(dl, g_font_micro, 9.0f,
                      ImVec2(content_x, origin.y + S(24)),
                      faded(kNerv.acc, a),
                      "A 2DFM COLLECTIVE PRODUCTION");
        }
    }
    // (220ms) hairline accent
    {
        const float a = fade_alpha(220, 500);
        if (a > 0.001f) {
            dl->AddLine(ImVec2(content_x, origin.y + S(42)),
                        ImVec2(content_x + S(60.0f) * a, origin.y + S(42)),
                        faded(kNerv.acc, a), 2.0f);
        }
    }
    // (400ms) big title — 2DFM \n forever.
    {
        const float a = fade_alpha(400, 700);
        if (a > 0.001f) {
            const float pt = g_tune.splash_title_pt;
            DrawTextF(dl, g_font_display, pt,
                      ImVec2(content_x, origin.y + S(56)),
                      faded(kNerv.ink, a), "2DFM");
            DrawTextF(dl, g_font_display, pt,
                      ImVec2(content_x, origin.y + S(56) + S(pt * 0.95f)),
                      faded(kNerv.acc, a), "forever.");
        }
    }
    // (780ms) subtitle
    {
        const float a = fade_alpha(780, 500);
        if (a > 0.001f) {
            DrawTextF(dl, g_font_micro, 9.0f,
                      ImVec2(content_x, origin.y + S(224)),
                      faded(kNerv.ink2, a),
                      "95 \xc2\xb7 2ND/2K \xc2\xb7 ROLLBACK \xc2\xb7 FRAME-PERFECT \xc2\xb7 COMMUNITY HUB");
        }
    }
    // (1100ms) greetz marquee — cycles every 2.4s
    {
        const float a = fade_alpha(1100, 500);
        static const char* kGreets[] = {
            "GREETZ \xe2\x98\x86 AOKUBI \xc2\xb7 KIYO \xc2\xb7 SUIKA \xc2\xb7 SAGEFOX \xc2\xb7 MIRROR \xc2\xb7 BARTRA",
            "RESPECT TO ALL 2DFM AUTHORS \xc2\xb7 KEEP THE SCENE ALIVE",
            "MUSIC \xc2\xb7 NIGHT_LOOP.IT \xc2\xb7 BY SLOTH/2DFM \xc2\xb7 8CH",
            "REMEMBER \xc2\xb7 SHIP YOUR DOUJIN",
            "\xe2\x98\x85\xe2\x98\x85\xe2\x98\x85 2DFM COLLECTIVE PRESENTS \xe2\x98\x85\xe2\x98\x85\xe2\x98\x85",
        };
        const int gi = ((int)(ImGui::GetTime() / 2.4)) %
                       (int)(sizeof(kGreets) / sizeof(kGreets[0]));
        if (a > 0.001f) {
            ImVec2 gp(content_x, origin.y + S(248));
            const char* line = kGreets[gi >= 0 ? gi : 0];
            const float pt = 10.0f * g_ui_scale;
            ImVec2 lsz = g_font_label
                ? g_font_label->CalcTextSizeA(pt, FLT_MAX, 0.0f, line)
                : ImGui::CalcTextSize(line);
            dl->AddRect(ImVec2(gp.x - S(10), gp.y - S(5)),
                        ImVec2(gp.x + lsz.x + S(26), gp.y + S(17)),
                        faded(kNerv.line, a), 0.0f, 0, 1.0f);
            DrawTextF(dl, g_font_micro, 10.0f,
                      ImVec2(gp.x, gp.y), faded(kNerv.acc, a),
                      "\xe2\x99\xaa");
            DrawTextF(dl, g_font_label, 10.0f,
                      ImVec2(gp.x + S(18), gp.y), faded(kNerv.ink2, a), line);
        }
    }

    // (1000ms) build column — bottom-left
    {
        const float a = fade_alpha(1000, 500);
        if (a > 0.001f) {
            const float by = br.y - S(64);
            DrawTextF(dl, g_font_label, 9.0f,
                      ImVec2(content_x, by + 0),
                      faded(kNerv.dim, a), "BUILD       0.2.18 \xc2\xb7 32-BIT");
            DrawTextF(dl, g_font_label, 9.0f,
                      ImVec2(content_x, by + S(12)),
                      faded(kNerv.dim, a), "SIGNATURE   0xDEADBEEF");
            DrawTextF(dl, g_font_label, 9.0f,
                      ImVec2(content_x, by + S(24)),
                      faded(kNerv.dim, a), "HUB         hub.2dfm.org");
            char up_str[40];
            std::snprintf(up_str, sizeof(up_str),
                          "UPTIME      %.1fs", (float)ImGui::GetTime());
            DrawTextF(dl, g_font_label, 9.0f,
                      ImVec2(content_x, by + S(36)),
                      faded(kNerv.faint, a), up_str);
        }
    }

    // (1200ms) PRESS TO ENTER pill — bottom-right
    bool advance = false;
    {
        const float a = fade_alpha(1200, 500);
        if (a > 0.001f) {
            DrawTextF(dl, g_font_micro, 9.0f,
                      ImVec2(content_x, br.y - S(14)),
                      faded(kNerv.faint, a),
                      "ENTER / SPACE / ESC / CLICK \xc2\xb7 ctrl+\x60 \xe2\x86\x92 dev tune");

            const char* enter = "[ \xe2\x86\xb5 ]  PRESS TO ENTER";
            const float pt = 11.0f * g_ui_scale;
            ImVec2 sz = g_font_label
                ? g_font_label->CalcTextSizeA(pt, FLT_MAX, 0.0f, enter)
                : ImGui::CalcTextSize(enter);
            ImVec2 bp(br.x - S(22) - sz.x - S(12), br.y - S(30));
            ImVec2 b1(bp.x + sz.x + S(12), bp.y + S(18));
            ImGui::SetCursorScreenPos(ImVec2(bp.x - S(4), bp.y - S(2)));
            const bool clicked = ImGui::InvisibleButton("##splash.continue",
                                                        ImVec2(sz.x + S(20), S(22)));
            const bool hov = ImGui::IsItemHovered();
            if (hov) {
                dl->AddRectFilled(bp, b1, faded(IM_COL32(0xff,0x6a,0x00,0x18), a));
            }
            dl->AddRect(bp, b1, faded(kNerv.acc, a), 0.0f, 0, 1.0f);
            DrawTextF(dl, g_font_label, 11.0f,
                      ImVec2(bp.x + S(6), bp.y + S(3)),
                      faded(kNerv.acc, a), enter);

            if (clicked || ShellSkipPressed()) advance = true;
        }
    }

    if (advance) {
        const auto cached = fm2k::discord_auth::LoadCached();
        ShellRoute next;
        if (!cached.valid) {
            next = ShellRoute::Login;
        } else if (ShouldRunWizard(s)) {
            next = ShellRoute::Setup;
            s.setup_step = 0;
        } else {
            next = ShellRoute::Hub;
        }
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "shell: route Splash -> %s",
                    next == ShellRoute::Login ? "Login (LoginV2)"
                  : next == ShellRoute::Setup ? "Setup step 0 (Controls)"
                  : "Hub");
        s.route = next;
        s.route_entered_at = ImGui::GetTime();
        ++s.transition_seq;
    }
    (void)lu;
}

// ─── TUNE PANEL ─ Ctrl+` toggles ─────────────────────────────────────
// Hidden dev panel for live-tweaking splash visuals + lissajous +
// title size. Persists via SaveTune to settings.ini under tune_*
// keys. Reset button restores struct defaults; Save commits the
// current values; closing the panel does NOT auto-save (so you can
// experiment without polluting settings.ini).
//
// Viewport caveat: even though the launcher enables
// ImGuiConfigFlags_ViewportsEnable, the imgui_impl_sdlrenderer3
// backend doesn't set BackendFlags_RendererHasViewports — there's
// no per-platform-window SDL_Renderer support. So this panel stays
// inside the host viewport (still floating / movable / resizable /
// dockable, just not poppable onto a second monitor). To enable
// pop-out, swap the renderer backend to one that implements
// Renderer_CreateWindow et al. (OpenGL3 / DX11) or extend
// imgui_impl_sdlrenderer3 ourselves.
void RenderTunePanel() {
    if (!g_tune.panel_open) return;

    ImGui::SetNextWindowSize(ImVec2(380, 0), ImGuiCond_FirstUseEver);

    if (!ImGui::Begin("dev tune  [ctrl+`]", &g_tune.panel_open)) {
        ImGui::End();
        return;
    }

    // ── Theme picker — cycles the active palette. Sets kNerv from the
    //   kThemes[] table; every view that reads kNerv.<role> retints
    //   on the next frame. Ctrl+Shift+T cycles globally too.
    if (ImGui::CollapsingHeader("Theme palette",
                                ImGuiTreeNodeFlags_DefaultOpen)) {
        Theme cur = CurrentTheme();
        if (ImGui::BeginCombo("palette", ThemeName(cur))) {
            for (int i = 0; i < (int)Theme::kCount; ++i) {
                const Theme t = static_cast<Theme>(i);
                const bool selected = (t == cur);
                if (ImGui::Selectable(ThemeName(t), selected)) {
                    SetTheme(t);
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (ImGui::Button("cycle")) CycleTheme();
        ImGui::TextDisabled("ctrl+shift+t cycles \xc2\xb7 saved with [save]");
    }

    if (ImGui::CollapsingHeader("EnergyField (snow/starfield)",
                                ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::SliderInt("count",      &g_tune.field_count, 20, kEnergyParticleCap)) {
            g_energy_seeded = false;
        }
        ImGui::SliderFloat("intensity",   &g_tune.field_intensity,  0.0f, 1.5f);
        ImGui::SliderFloat("speed min",   &g_tune.field_speed_min,  0.0f, 60.0f);
        ImGui::SliderFloat("speed max",   &g_tune.field_speed_max,  0.0f, 80.0f);
        ImGui::SliderFloat("y wobble",    &g_tune.field_wobble_amp, 0.0f, 60.0f);
        ImGui::SliderFloat("size min",    &g_tune.field_size_min,   0.4f,  4.0f);
        ImGui::SliderFloat("size max",    &g_tune.field_size_max,   0.4f,  6.0f);
        ImGui::SliderFloat("life decay",  &g_tune.field_life_decay, 0.02f, 0.6f);
        if (ImGui::Button("reseed##field")) g_energy_seeded = false;
    }

    if (ImGui::CollapsingHeader("Lissajous corner gizmo",
                                ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::SliderFloat("size",        &g_tune.lissa_size,       60.0f, 400.0f);
        ImGui::SliderInt  ("a",           &g_tune.lissa_a,           1,    8);
        ImGui::SliderInt  ("b",           &g_tune.lissa_b,           1,    8);
        ImGui::SliderFloat("speed",       &g_tune.lissa_speed,      0.05f, 3.5f);
        ImGui::SliderInt  ("trail",       &g_tune.lissa_trail,      10, kLissajousTrail);
        ImGui::SliderFloat("margin x",    &g_tune.lissa_margin_x,   0.0f, 200.0f);
        ImGui::SliderFloat("margin y",    &g_tune.lissa_margin_y,   0.0f, 200.0f);
        ImGui::SliderFloat("dot radius",  &g_tune.lissa_dot_radius, 0.5f,   8.0f);
        ImGui::SliderFloat("line width",  &g_tune.lissa_line_width, 0.5f,   4.0f);
    }

    if (ImGui::CollapsingHeader("Splash text")) {
        ImGui::SliderFloat("title pt",    &g_tune.splash_title_pt,  24.0f, 140.0f);
    }

    if (ImGui::CollapsingHeader("Font scales", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::TextDisabled("multiplier applied to every DrawTextF call");
        ImGui::SliderFloat("display##fs", &g_font_display_scale, 0.5f, 2.5f, "%.2fx");
        ImGui::SliderFloat("body##fs",    &g_font_body_scale,    0.5f, 2.5f, "%.2fx");
        ImGui::SliderFloat("label##fs",   &g_font_label_scale,   0.5f, 2.5f, "%.2fx");
        ImGui::SliderFloat("micro##fs",   &g_font_micro_scale,   0.5f, 2.5f, "%.2fx");
        if (ImGui::Button("1.0x##font_reset")) {
            g_font_display_scale = g_font_body_scale =
            g_font_label_scale   = g_font_micro_scale = 1.0f;
        }
        ImGui::SameLine();
        if (ImGui::Button("1.25x##font_125")) {
            g_font_display_scale = g_font_body_scale =
            g_font_label_scale   = g_font_micro_scale = 1.25f;
        }
        ImGui::SameLine();
        if (ImGui::Button("1.5x##font_150")) {
            g_font_display_scale = g_font_body_scale =
            g_font_label_scale   = g_font_micro_scale = 1.5f;
        }
    }

    ImGui::Separator();
    if (ImGui::Button("save")) {
        SaveTune();
    }
    ImGui::SameLine();
    if (ImGui::Button("reload")) {
        g_tune_loaded = false;
        LoadTune();
        g_energy_seeded = false;
    }
    ImGui::SameLine();
    if (ImGui::Button("reset defaults")) {
        ResetTuneDefaults();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("close = discard");

    ImGui::End();
}

}  // namespace (file-local)

// ─── PUBLIC ENTRY ─────────────────────────────────────────────────────
void Render(LauncherUI& lu) {
    LoadFromSettings(State());
    LoadTune();

    // Ctrl+` toggles the dev tune panel. Pre-empt any other handler
    // by running before route dispatch, but only react when the chord
    // is actually pressed (not held) so it doesn't latch.
    {
        ImGuiIO& io = ImGui::GetIO();
        const bool ctrl = io.KeyCtrl;
        if (ctrl && ImGui::IsKeyPressed(ImGuiKey_GraveAccent, false)) {
            g_tune.panel_open = !g_tune.panel_open;
        }
        // Ctrl+Shift+T cycles the palette without opening the panel.
        if (ctrl && io.KeyShift &&
            ImGui::IsKeyPressed(ImGuiKey_T, false)) {
            CycleTheme();
            // Persist immediately so the choice survives restart even
            // if the user never opens the panel + clicks save.
            const std::string path = SettingsPath();
            if (!path.empty()) {
                WriteInt(path, "theme_index", (int)CurrentTheme());
            }
        }
    }

    // Fill the SDL viewport with a single chromeless ImGui window.
    // Mirrors how the legacy DockSpace begins (FM2K_LauncherUI.cpp:820).
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::SetNextWindowViewport(vp->ID);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar
                           | ImGuiWindowFlags_NoCollapse
                           | ImGuiWindowFlags_NoResize
                           | ImGuiWindowFlags_NoMove
                           | ImGuiWindowFlags_NoBringToFrontOnFocus
                           | ImGuiWindowFlags_NoNavFocus
                           | ImGuiWindowFlags_NoDocking
                           | ImGuiWindowFlags_NoBackground
                           // No vertical scrollbar — InvisibleButton
                           // placements would otherwise push the
                           // calculated content size past viewport and
                           // trigger ImGui's auto-scroll.
                           | ImGuiWindowFlags_NoScrollbar
                           | ImGuiWindowFlags_NoScrollWithMouse;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, kNerv.bg);

    ImGui::Begin("##fm2k.shell.root", nullptr, flags);
    ImGui::PopStyleVar(3);

    // Region rectangles. Render at native window pixels.
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImVec2 avail = ImGui::GetContentRegionAvail();

    // Compute per-frame UI scale. Take the MIN of width-ratio and
    // height-ratio so neither dimension overflows the canvas:
    //
    //   * Width-only would make a 1920×900 window scale 3× — but the
    //     window is only 900 px tall, not enough room for sidebar +
    //     content at 3× sizes. Result: sidebar's bottom-anchored nav
    //     reservations would eat the chip column. The user's "resizing
    //     width changes sidebar height" bug.
    //   * Height-only would underuse horizontal pixels on wide
    //     monitors (3440×1440 ultrawide → 3.2× scale based on 1440H,
    //     wastes the extra width).
    //
    // min(w-ratio, h-ratio) clamps to whatever the smaller axis
    // allows. At 1280×900 (default) = min(2, 2) = 2.0 (unchanged).
    g_ui_scale = std::max(0.5f,
                          std::min(avail.x / kBaseDesignW,
                                   avail.y / kBaseDesignH));

    // Enforce a minimum visual canvas — at very small windows we
    // letterbox the layout into the top-left rather than letting it
    // crash through the chrome edges.
    if (avail.x < kCanvasMinW) avail.x = kCanvasMinW;
    if (avail.y < kCanvasMinH) avail.y = kCanvasMinH;

    // First-render route timer init — `route_entered_at == 0` means we
    // haven't anchored the timeline yet for the default route. Every
    // subsequent transition stamps it via `ImGui::GetTime()`.
    auto& s = State();
    if (s.route_entered_at == 0.0) {
        s.route_entered_at = ImGui::GetTime();
    }

    // Titlebar — height scales with ui_scale.
    const float titlebar_h = S(kTitlebarH);
    DrawShellTitlebar(lu, origin, avail.x);

    // The body region sits below the titlebar.
    const ImVec2 body_origin(origin.x, origin.y + titlebar_h);
    const ImVec2 body_avail (avail.x,  avail.y  - titlebar_h);

    // Route dispatch — Splash/Login/Setup are full-viewport pre-Hub
    // routes that bypass the chrome entirely. Hub renders the rail /
    // topbar / right roster / status under the titlebar.
    if (s.route == ShellRoute::Splash) {
        RenderSplashV2(lu, body_origin, body_avail);
    }
    else if (s.route == ShellRoute::Login) {
        RenderLoginV2(lu, body_origin, body_avail);
    }
    else if (s.route == ShellRoute::Setup) {
        RenderSetupV2(lu, body_origin, body_avail);
    }
    else if (s.route == ShellRoute::Completion) {
        RenderCompletionV2(lu, body_origin, body_avail);
    }
    else {
        // Right roster is room-presence context — only meaningful while
        // the user is sitting in a room's lobby. On Browse/Rankings/etc
        // OR on the home dashboard, it's dead weight and we'd rather
        // hand the 154px back to the main column.
        // Right roster (user list) shows whenever we're sitting in a
        // room's lobby. Used to depend on a separate s.in_room flag —
        // that flag became redundant once the home dashboard moved
        // to Browse, so the condition is just "active room + Lobby".
        const bool show_roster = (s.hub_view == HubView::Lobby) &&
                                 (s.active_room_index >= 0);
        const float rail_w   = S(kRailW);
        const float right_w  = S(kRightW);
        const float top_h    = S(kTopH);
        const float status_h = S(kStatusH);
        const float roster_w = show_roster ? right_w : 0.0f;
        const float main_w   = body_avail.x - rail_w - roster_w;
        const float main_h   = body_avail.y - top_h - status_h;
        const std::vector<ActiveRoom> rooms = BuildActiveRooms(lu);

        DrawLeftRail(lu, body_origin, body_avail.y, rooms);
        DrawTopBar  (lu, ImVec2(body_origin.x + rail_w, body_origin.y),
                     main_w + roster_w, rooms);
        if (show_roster) {
            DrawRightRoster(lu,
                            ImVec2(body_origin.x + rail_w + main_w, body_origin.y + top_h),
                            ImVec2(roster_w, main_h), rooms);
        }
        DrawStatusBar(lu,
                      ImVec2(body_origin.x + rail_w, body_origin.y + body_avail.y - status_h),
                      main_w + roster_w);
        DrawMainContent(lu,
                        ImVec2(body_origin.x + rail_w, body_origin.y + top_h),
                        ImVec2(main_w, main_h), rooms);

        // M3 popovers / modals — only relevant in Hub. Order matters:
        // user-mini-menu is non-blocking (hover popover); challenge +
        // connection-lost modals are click-blocking (BeginPopupModal).
        // All call ImGui's popup machinery so render them every frame;
        // ImGui only shows the popup body when actually open.
        RenderUserMiniMenu(lu);
        RenderMatchHoverCard(lu);
        RenderChallengeIncomingModal(lu);
        RenderChallengeOutgoingModal(lu);
        RenderConnectionLostModal(lu);
        RenderDesyncModal(lu);

        // Match flow overlays — driven by LauncherState. Connecting
        // covers the entire viewport (handshake/booting); InGame puts
        // a top banner over the chrome.
        const LauncherState ls = lu.launcher_state();
        if (ls == LauncherState::Connecting) {
            RenderMatchFlowConnecting(lu);
        } else if (ls == LauncherState::InGame) {
            RenderInGameBanner(lu);
        }
    }

    // M4 cross-fade route transitions — overlay a black rect that fades
    // from full to transparent over 180ms whenever the route changes.
    // Channel id mixes transition_seq so each route change starts a
    // fresh tween. ImAnim crossfade policy lets it ride a re-target
    // smoothly even if a transition fires mid-fade.
    {
        char fade_id[40];
        std::snprintf(fade_id, sizeof(fade_id),
                      "shell.route.fade.%d", s.transition_seq);
        const float fade_alpha = iam_tween_float(
            ImHashStr(fade_id), ImHashStr("a"),
            /*target*/0.0f, /*dur*/0.18f,
            iam_ease_desc{ iam_ease_out_cubic, 0,0,0,0 },
            iam_policy_crossfade,
            ImGui::GetIO().DeltaTime,
            /*init*/1.0f);
        if (fade_alpha > 0.001f) {
            ImDrawList* dl_top = ImGui::GetWindowDrawList();
            const ImU32 col = IM_COL32(0, 0, 0, (int)(255.0f * fade_alpha));
            dl_top->AddRectFilled(origin,
                                  ImVec2(origin.x + avail.x,
                                         origin.y + avail.y), col);
        }
    }

    ImGui::Dummy(avail);  // reserve the space so ImGui's auto-cursor is sane
    ImGui::End();
    ImGui::PopStyleColor();

    // Tune panel — drawn outside the chromeless root so it's a normal
    // ImGui window (movable / resizable). Toggled by Ctrl+` above.
    RenderTunePanel();
}

void PushIncomingChat(const std::string& room_id,
                      const std::string& user_id,
                      const std::string& nick,
                      const std::string& text,
                      int64_t            ts_unix_seconds,
                      bool               system) {
    ChatLine line;
    line.ts      = FormatChatTs(ts_unix_seconds);
    line.user_id = user_id;
    line.nick    = nick;
    line.text    = text;
    line.system  = system;
    PushRoomChat(room_id, std::move(line));
}

}  // namespace fm2k::shell
