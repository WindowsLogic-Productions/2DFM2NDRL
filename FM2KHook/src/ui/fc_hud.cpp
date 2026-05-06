// fc_hud — see header for design rationale.

#include "fc_hud.h"
#include "shared_mem.h"   // FM2KSharedMemData::ui_*_nick (launcher-populated)
#include "../netplay/netplay.h"  // Netplay_PopChatMessage / ChatEntry

#include <imgui.h>
#include <windows.h>      // GetTickCount for Slice D system-msg fade

#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>

namespace fc_hud {

namespace {

struct State {
    char     p1_name[64] = {};
    char     p2_name[64] = {};
    int      fps         = 0;
    uint32_t ping_ms     = 0;
    int      delay       = 0;
    bool     connected   = false;

    // ─── Slice C/D pulled from shared mem each frame ─────────────
    // Score box visible only when `score_seq > 0` (zero = never
    // published, hide). System message visible when `sys_seq > 0`
    // AND tick < expiry; faded out in the last 500 ms before expiry.
    uint32_t score_seq    = 0;
    uint16_t score_p1     = 0;
    uint16_t score_p2     = 0;
    uint16_t spectators   = 0;
    uint32_t sys_seq      = 0;
    uint32_t sys_expiry   = 0;
    char     sys_message[160] = {};
};

// Single mutex protects all state. Setters are called from various
// threads (netplay, hub, render) at low frequency (frame-rate or
// slower); Render acquires + copies once per frame.
std::mutex g_mtx;
State      g_state{};

// Slice E — chat display ring. fc_hud owns this list; the source is
// Netplay's existing ChatEntry ring (control-channel CHAT messages
// + locally-sent echoes via Netplay_SendChatMessage). We pop on
// every Render to drain, append into our local ring, evict the
// oldest when over kChatLines. Cap matches Fightcade's visible
// count.
constexpr size_t kChatLines = 6;

struct ChatLine {
    bool        from_remote;
    DWORD       arrived_tick;     // local GetTickCount on push
    std::string text;             // ChatEntry.text is 24 bytes; copy for safety
};

std::deque<ChatLine> g_chat_lines;
DWORD                g_chat_last_arrived_tick = 0;

}  // namespace

void Init() {
    std::lock_guard<std::mutex> lk(g_mtx);
    // Default labels so the bar still reads sensibly before any
    // netplay metadata has been pushed in. SetPlayerNames overwrites.
    if (!g_state.p1_name[0]) std::snprintf(g_state.p1_name, sizeof(g_state.p1_name), "P1");
    if (!g_state.p2_name[0]) std::snprintf(g_state.p2_name, sizeof(g_state.p2_name), "P2");
}

void Shutdown() {
    // No resources to free; placeholder so callers can pair with Init().
}

void SetPlayerNames(const char* p1, const char* p2) {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (p1) std::snprintf(g_state.p1_name, sizeof(g_state.p1_name), "%s", p1);
    if (p2) std::snprintf(g_state.p2_name, sizeof(g_state.p2_name), "%s", p2);
}

void SetStats(int fps, uint32_t ping_ms, int delay) {
    std::lock_guard<std::mutex> lk(g_mtx);
    g_state.fps     = fps;
    g_state.ping_ms = ping_ms;
    g_state.delay   = delay;
}

void SetConnected(bool connected) {
    std::lock_guard<std::mutex> lk(g_mtx);
    g_state.connected = connected;
}

void Render(int rect_x, int rect_y, int rect_w, int rect_h) {
    if (rect_w <= 0 || rect_h <= 0) return;

    // Pull all shared-mem-fed state (nicks, scores, spectator count,
    // system message) under one lock. The launcher populates `ui_*_nick`
    // at K::MatchStart and clears on disconnect (FM2K_LauncherUI.cpp
    // :3194-3196 + the equivalent clears at :3376/:3416). HUD-specific
    // fields land here too — score box and system message are gated
    // on their `_seq > 0` so default-zero shared mem renders nothing
    // until the launcher (or hook) publishes for the first time.
    if (FM2KSharedMemData* sm = GetSharedMemory()) {
        std::lock_guard<std::mutex> lk(g_mtx);
        if (sm->ui_my_nick[0]) {
            std::snprintf(g_state.p1_name, sizeof(g_state.p1_name),
                          "%s", sm->ui_my_nick);
        }
        if (sm->ui_peer_nick[0]) {
            std::snprintf(g_state.p2_name, sizeof(g_state.p2_name),
                          "%s", sm->ui_peer_nick);
        }
        g_state.score_seq  = sm->hud_score_seq;
        g_state.score_p1   = sm->hud_score_p1;
        g_state.score_p2   = sm->hud_score_p2;
        g_state.spectators = sm->hud_spectator_count;
        g_state.sys_seq    = sm->hud_system_message_seq;
        g_state.sys_expiry = sm->hud_system_message_expiry_tick;
        std::snprintf(g_state.sys_message, sizeof(g_state.sys_message),
                      "%s", sm->hud_system_message);
    }

    // Snapshot under lock so the layout below works against a
    // consistent state even if a setter races us mid-frame.
    State s;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        s = g_state;
    }

    ImDrawList* dl   = ImGui::GetForegroundDrawList();
    ImFont*     font = ImGui::GetFont();

    // Layout in normalized coords scaled to a logical 260-unit-tall
    // canvas — same idiom Fightcade uses (vid_overlay.cpp:1031), so
    // tweaking constants below to adjust look-and-feel translates
    // 1:1 across game resolutions (FM2K 480, FM95 240, larger
    // upscales).
    const float frame_scale = (float)rect_h / 260.0f;

    // Top bar geometry. ~16 logical units tall — slim like Fightcade,
    // but we don't shrink under a floor so very-small windows stay
    // legible. Player labels and the right-aligned stats both live
    // inside the bar with vertical-center alignment.
    float bar_h = 16.0f * frame_scale;
    if (bar_h < 18.0f) bar_h = 18.0f;

    const ImVec2 bar_a((float)rect_x, (float)rect_y);
    const ImVec2 bar_b((float)(rect_x + rect_w), (float)rect_y + bar_h);
    // Semi-transparent dark fill. 180/255 alpha keeps the underlying
    // game pixels faintly visible — same density as Fightcade's bar.
    dl->AddRectFilled(bar_a, bar_b, IM_COL32(0, 0, 0, 180));
    // 1-pixel bottom edge so the bar's lower border is crisp against
    // active gameplay.
    dl->AddLine(ImVec2(bar_a.x, bar_b.y - 0.5f),
                ImVec2(bar_b.x, bar_b.y - 0.5f),
                IM_COL32(60, 60, 60, 200), 1.0f);

    // Type sizing. Default ImGui font is ~13px at scale 1; we rescale
    // to ~70% of bar height so the text reads inside the bar without
    // crowding the top/bottom edges.
    const float font_size = bar_h * 0.7f;
    const float pad       = bar_h * 0.45f;
    const float text_y    = (float)rect_y + (bar_h - font_size) * 0.5f;

    // Left side — local player.
    char left_buf[128];
    std::snprintf(left_buf, sizeof(left_buf), "P1: %s",
                  s.p1_name[0] ? s.p1_name : "P1");
    dl->AddText(font, font_size,
                ImVec2((float)rect_x + pad, text_y),
                IM_COL32_WHITE, left_buf);

    // Center score box (Slice C). Only renders when scores have
    // actually been published — hides on offline / pre-match. Layout:
    // a compact pill `<P1 W>-<P2 W>` between the two name labels,
    // centered on the bar's vertical midline.
    if (s.score_seq > 0) {
        char score_buf[16];
        std::snprintf(score_buf, sizeof(score_buf),
                      "%u - %u", (unsigned)s.score_p1, (unsigned)s.score_p2);
        const ImVec2 score_size = font->CalcTextSizeA(font_size, FLT_MAX,
                                                      0.0f, score_buf);
        const float box_pad_x = font_size * 0.6f;
        const float box_w = score_size.x + box_pad_x * 2.0f;
        const float box_x = (float)rect_x + (float)rect_w * 0.5f - box_w * 0.5f;
        dl->AddRectFilled(
            ImVec2(box_x, (float)rect_y + bar_h * 0.15f),
            ImVec2(box_x + box_w, (float)rect_y + bar_h * 0.85f),
            IM_COL32(50, 50, 50, 220), bar_h * 0.15f);
        dl->AddText(font, font_size,
                    ImVec2(box_x + box_pad_x, text_y),
                    IM_COL32_WHITE, score_buf);
    }

    // Right side — opponent + live stats. Spectator count tucks in
    // before the ping number when non-zero (Fightcade-style). Offline
    // collapses to just FPS so the bar still confirms the HUD is alive.
    char right_buf[224];
    if (s.connected) {
        if (s.spectators > 0) {
            std::snprintf(right_buf, sizeof(right_buf),
                "%s :P2    %u spec    ping %u ms  delay %d  fps %d",
                s.p2_name[0] ? s.p2_name : "P2",
                (unsigned)s.spectators,
                (unsigned)s.ping_ms, s.delay, s.fps);
        } else {
            std::snprintf(right_buf, sizeof(right_buf),
                "%s :P2    ping %u ms  delay %d  fps %d",
                s.p2_name[0] ? s.p2_name : "P2",
                (unsigned)s.ping_ms, s.delay, s.fps);
        }
    } else {
        std::snprintf(right_buf, sizeof(right_buf), "fps %d", s.fps);
    }

    const ImVec2 right_size = font->CalcTextSizeA(font_size, FLT_MAX,
                                                  0.0f, right_buf);
    dl->AddText(font, font_size,
                ImVec2((float)(rect_x + rect_w) - pad - right_size.x, text_y),
                IM_COL32_WHITE, right_buf);

    // ─── Slice E: chat history (bottom-left) ───────────────────────
    // Drain Netplay's chat ring into our local list every frame —
    // Netplay_PushChatMessage is called for both received CHAT
    // packets (from_remote=true) and local echoes from
    // Netplay_SendChatMessage. Nobody else pops, so we own the
    // drain; lines stay visible in our local ring after the
    // netplay-side ring forgets them.
    {
        ChatEntry e;
        while (Netplay_PopChatMessage(&e)) {
            ChatLine ln;
            ln.from_remote  = e.from_remote;
            ln.arrived_tick = GetTickCount();
            // ChatEntry.text is char[24], guaranteed NUL-terminated
            // by the producer. Copy into std::string so the display
            // layer doesn't depend on the source buffer's lifetime.
            ln.text = e.text;
            g_chat_lines.push_back(std::move(ln));
            while (g_chat_lines.size() > kChatLines) g_chat_lines.pop_front();
            g_chat_last_arrived_tick = GetTickCount();
        }
    }

    if (!g_chat_lines.empty()) {
        // Fade entire chat block out after 30 s of silence; full
        // alpha during the first 28 s, fading the last 2 s.
        const DWORD now = GetTickCount();
        const DWORD silence_ms = now - g_chat_last_arrived_tick;
        float chat_alpha = 1.0f;
        if (silence_ms > 28000) {
            const float fade = (float)(silence_ms - 28000) / 2000.0f;
            chat_alpha = (fade >= 1.0f) ? 0.0f : (1.0f - fade);
        }
        if (chat_alpha > 0.005f) {
            const float chat_font = bar_h * 0.85f;
            const float line_h    = chat_font * 1.25f;
            const float chat_pad  = bar_h * 0.4f;
            // Container box bottom-left of game rect, sized to fit
            // current line count (max kChatLines).
            const size_t n = g_chat_lines.size();
            const float box_h = line_h * (float)n + chat_pad * 0.6f;
            // Wide enough for ~36 chars at this font size; ChatEntry
            // truncates to 23 chars so this fits comfortably.
            const float box_w = chat_font * 26.0f;
            const float box_x = (float)rect_x + chat_pad;
            const float box_y = (float)(rect_y + rect_h)
                              - chat_pad - box_h;

            // Background — same idiom as the top bar's tinted fill,
            // ramped by the silence fade.
            dl->AddRectFilled(
                ImVec2(box_x, box_y),
                ImVec2(box_x + box_w, box_y + box_h),
                IM_COL32(0, 0, 0, (int)(170.0f * chat_alpha)));

            // Render lines top-down inside the box. Local messages
            // colored P1-blue, remote P2-orange — matches the tone
            // most fighting-game chrome uses.
            const ImU32 col_local  = IM_COL32(120, 180, 255,
                                              (int)(255.0f * chat_alpha));
            const ImU32 col_remote = IM_COL32(255, 200, 110,
                                              (int)(255.0f * chat_alpha));
            const ImU32 col_text   = IM_COL32(255, 255, 255,
                                              (int)(255.0f * chat_alpha));
            float ty = box_y + chat_pad * 0.3f;
            for (const auto& ln : g_chat_lines) {
                const char* nick =
                    ln.from_remote
                        ? (s.p2_name[0] ? s.p2_name : "P2")
                        : (s.p1_name[0] ? s.p1_name : "P1");
                char nick_buf[80];
                std::snprintf(nick_buf, sizeof(nick_buf), "%s: ", nick);
                const ImU32 nick_col = ln.from_remote ? col_remote : col_local;
                const float nick_x = box_x + chat_pad * 0.5f;
                dl->AddText(font, chat_font, ImVec2(nick_x, ty),
                            nick_col, nick_buf);
                const ImVec2 nick_size = font->CalcTextSizeA(
                    chat_font, FLT_MAX, 0.0f, nick_buf);
                dl->AddText(font, chat_font,
                            ImVec2(nick_x + nick_size.x, ty),
                            col_text, ln.text.c_str());
                ty += line_h;
            }
        }
    }

    // ─── Slice D: centered system message ──────────────────────────
    // Renders centered on the rect (vertically near the top third),
    // with a fade-out in the last 500 ms before expiry. Skipped when
    // never published (seq=0) or already expired.
    if (s.sys_seq > 0 && s.sys_message[0]) {
        const DWORD now = GetTickCount();
        // Branchless "remaining" using 32-bit modular subtraction so
        // wraparound at the 49-day tick boundary doesn't false-fire.
        const int32_t remaining = (int32_t)(s.sys_expiry - now);
        if (remaining > 0) {
            float alpha = 1.0f;
            if (remaining < 500) {
                alpha = (float)remaining / 500.0f;
            }
            const float msg_font = bar_h * 1.6f;  // bigger than top-bar text
            const ImVec2 msg_size = font->CalcTextSizeA(
                msg_font, FLT_MAX, 0.0f, s.sys_message);
            const float msg_pad_x = msg_font * 0.7f;
            const float msg_pad_y = msg_font * 0.3f;
            const float box_w = msg_size.x + msg_pad_x * 2.0f;
            const float box_h = msg_size.y + msg_pad_y * 2.0f;
            const float box_x = (float)rect_x + (float)rect_w * 0.5f - box_w * 0.5f;
            const float box_y = (float)rect_y + (float)rect_h * 0.20f;
            const ImU32 bg_col = IM_COL32(0, 0, 0,
                                          (int)(180.0f * alpha));
            const ImU32 fg_col = IM_COL32(255, 255, 255,
                                          (int)(255.0f * alpha));
            dl->AddRectFilled(ImVec2(box_x, box_y),
                              ImVec2(box_x + box_w, box_y + box_h),
                              bg_col, msg_font * 0.2f);
            dl->AddText(font, msg_font,
                        ImVec2(box_x + msg_pad_x, box_y + msg_pad_y),
                        fg_col, s.sys_message);
        }
    }
}

}  // namespace fc_hud
