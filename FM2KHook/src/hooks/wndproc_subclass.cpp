#include "wndproc_subclass.h"
#include "../netplay/control_channel.h"
#include "../core/globals.h"   // FM2K::kIsFM2K / kIsFM95, g_spectator_*
#include <SDL3/SDL_log.h>
#include <windows.h>
#include <cstring>
#include <cstdio>

namespace FM2KWndProc {

// Win32 timer ID for our modal-loop pump. Picked to be high so we
// don't collide with anything FM2K's own WindowProc might register.
constexpr UINT_PTR kModalPumpTimerId = 0x46324B01;  // 'F2K\x01'

// Modal pump cadence in ms. 5ms is well under PING_TIMEOUT_MS, so a
// drag of any realistic length stays inside our recv-deadline budget,
// and we still drain queued UDP packets fast enough that GekkoNet's
// own peer-timeout (5s) never trips during the drag.
constexpr UINT     kModalPumpInterval = 5;

static HWND    g_hwnd            = nullptr;
static WNDPROC g_orig_wndproc    = nullptr;
static bool    g_in_modal_loop   = false;

// Original window title captured at install time. We append " [FF]" when
// the user toggles fast-forward via F12 in spectator mode and restore the
// plain title on toggle off. ANSI titles only — FM2K windows are ASCII.
static char    g_original_title[256] = {};

static void UpdateSpectatorTitle() {
    if (!g_hwnd || !g_spectator_mode) return;
    char buf[320] = {};
    if (g_spectator_ff_user) {
        std::snprintf(buf, sizeof(buf), "%s [FF]", g_original_title);
    } else {
        std::snprintf(buf, sizeof(buf), "%s", g_original_title);
    }
    SetWindowTextA(g_hwnd, buf);
}

static LRESULT CALLBACK SubclassProc(HWND hwnd, UINT msg,
                                     WPARAM wparam, LPARAM lparam) {
    switch (msg) {
        // -- Alt / F10 menu mode suppression --------------------------
        // FM2K has no application menu, so DefWindowProc's default
        // "menu active" mode is pure friction: tapping Alt freezes
        // input until the user presses Esc, F10 does the same.
        // Swallow at the source.
        case WM_SYSCOMMAND:
            if ((wparam & 0xFFF0) == SC_KEYMENU) return 0;
            break;
        case WM_SYSKEYDOWN:
        case WM_SYSKEYUP:
            if (wparam == VK_MENU || wparam == VK_F10) return 0;
            break;

        // -- F12 fast-forward toggle (spectator mode only) ----------------
        // Spectators sit at whatever delay the network gave them; F12
        // requests one-burst catchup-to-live. Toggle off to drop back to
        // 1x. Title-bar shows "[FF]" while active. Swallowed before
        // game-input layer so it doesn't accidentally drive any FM2K
        // CSS/title menu (none of our games bind F12, but we're polite).
        case WM_KEYDOWN:
            if (wparam == VK_F12 && g_spectator_mode) {
                g_spectator_ff_user = !g_spectator_ff_user;
                UpdateSpectatorTitle();
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Spectator: F12 fast-forward %s",
                            g_spectator_ff_user ? "ON" : "OFF");
                return 0;
            }
            break;
        case WM_KEYUP:
            if (wparam == VK_F12 && g_spectator_mode) return 0;
            break;
        case WM_SYSCHAR:
            // Suppresses the "ding" Alt+letter would otherwise emit.
            return 0;

        // -- Modal title-drag pump ------------------------------------
        // DefWindowProc's SC_MOVE / SC_SIZE modal loop blocks
        // DispatchMessage on the main thread. The MM-timer in
        // control_channel.cpp keeps OUR pings flowing outbound, but
        // the receive side and timeout housekeeping are still tied
        // to ControlChannel_Poll(). Driving Poll() from a fast
        // WM_TIMER inside the modal loop is the canonical Win32
        // recipe for "keep the network alive while the user drags
        // the title bar". Same trick covers WM_INITMENU / system-
        // menu open, since those also fire ENTERSIZEMOVE-equivalent
        // modal loops via WM_ENTERMENULOOP — handled below.
        case WM_ENTERSIZEMOVE:
        case WM_ENTERMENULOOP:
            if (!g_in_modal_loop) {
                g_in_modal_loop = true;
                SetTimer(hwnd, kModalPumpTimerId, kModalPumpInterval, nullptr);
            }
            break;
        case WM_EXITSIZEMOVE:
        case WM_EXITMENULOOP:
            if (g_in_modal_loop) {
                KillTimer(hwnd, kModalPumpTimerId);
                g_in_modal_loop = false;
            }
            break;
        case WM_TIMER:
            if (wparam == kModalPumpTimerId) {
                // Drive networking only — no game-sim tick from here.
                // The sim itself naturally pauses while the user drags
                // (FM2K's render is single-threaded and the trampoline
                // hasn't been audited for reentrance from WM_TIMER).
                // What matters for the disconnect bug is the network
                // I/O, which is reentrant-safe.
                ControlChannel_Poll();
                return 0;
            }
            break;

        case WM_NCDESTROY:
            // Belt-and-suspenders timer cleanup; subclass uninstall
            // happens via Uninstall() from the trampoline shutdown
            // path, but if the window is destroyed without going
            // through that we still want the timer dead.
            KillTimer(hwnd, kModalPumpTimerId);
            g_in_modal_loop = false;
            break;

        // -- Menu Item 2320 redirect ---------------------------------
        // FM2K's window menu has a "Full Screen" entry whose handler
        // (HandleMainMenuCommand case 2320 @ 0x4177ae) toggles
        // g_graphics_mode and reinits DirectDraw — same fight with
        // cnc-ddraw as the F4 / Alt+Enter keyboard paths. When
        // cnc-ddraw is loaded (i.e. our IAT redirect succeeded and
        // 2DFMD.dll is present in the process), translate the menu
        // command into a synthetic VK_F4 keypress so cnc-ddraw's
        // WH_KEYBOARD hook (keyboard.c:57) fires its own
        // util_toggle_fullscreen() instead. PostMessage rather than
        // SendInput because we want this to look like a normal queued
        // keystroke that the message pump dispatches (which is what
        // WH_KEYBOARD intercepts).
        //
        // wparam high word holds the source flag (0=menu, 1=accelerator,
        // > 0 if from a control); we only redirect menu invocations.
        case WM_COMMAND: {
            const WORD id   = LOWORD(wparam);
            const WORD from = HIWORD(wparam);
            if (id == 2320 && from == 0 && lparam == 0 &&
                GetModuleHandleA("2DFMD.dll") != nullptr)
            {
                PostMessageA(hwnd, WM_KEYDOWN, VK_F4, 0);
                PostMessageA(hwnd, WM_KEYUP,   VK_F4, 0xC0000000);
                return 0;
            }
            break;
        }
    }
    return CallWindowProc(g_orig_wndproc, hwnd, msg, wparam, lparam);
}

// Find the KGT2KGAME window owned by THIS process. FindWindowA is
// process-blind and returns the first match globally — when two
// instances are running on the same PC, instance B would otherwise
// pick up instance A's HWND and SetWindowLongPtr would fail with
// ERROR_ACCESS_DENIED (cross-process subclass not allowed).
struct FindOwnWindowCtx {
    DWORD pid;
    HWND  result;
};
static BOOL CALLBACK FindOwnWindowProc(HWND hwnd, LPARAM lparam) {
    auto* ctx = reinterpret_cast<FindOwnWindowCtx*>(lparam);
    DWORD owner_pid = 0;
    GetWindowThreadProcessId(hwnd, &owner_pid);
    if (owner_pid != ctx->pid) return TRUE;
    char cls[32] = {0};
    if (GetClassNameA(hwnd, cls, sizeof(cls)) == 0) return TRUE;
    // FM2K uses "KGT2KGAME"; FM95/CPW uses "KGT95GAME". Same hook DLL
    // source builds twice (FM2KHook.dll + FM95Hook.dll); the engine
    // constants pick the right one at compile time.
    const char* expect_cls = FM2K::kIsFM95 ? "KGT95GAME" : "KGT2KGAME";
    if (lstrcmpA(cls, expect_cls) != 0) return TRUE;
    ctx->result = hwnd;
    return FALSE;  // stop enumeration
}

void TryInstall() {
    if (g_hwnd != nullptr) return;       // already installed
    FindOwnWindowCtx ctx{ GetCurrentProcessId(), nullptr };
    EnumWindows(FindOwnWindowProc, reinterpret_cast<LPARAM>(&ctx));
    HWND hwnd = ctx.result;
    if (hwnd == nullptr) return;          // window not yet up; try again later
    WNDPROC prev = (WNDPROC)SetWindowLongPtrA(
        hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(SubclassProc));
    if (prev == nullptr) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "WndProcSubclass: SetWindowLongPtr failed (err=%lu)",
                    GetLastError());
        return;
    }
    g_hwnd         = hwnd;
    g_orig_wndproc = prev;

    // Snapshot the original title so the F12 spectator FF toggle can
    // append/strip "[FF]" without losing the game's chosen title text.
    GetWindowTextA(hwnd, g_original_title, sizeof(g_original_title) - 1);

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "WndProcSubclass: installed on hwnd=%p prev_wndproc=%p "
                "(Alt-mute + modal-pump)",
                hwnd, (void*)prev);
}

void Uninstall() {
    if (g_hwnd == nullptr) return;
    if (g_in_modal_loop) {
        KillTimer(g_hwnd, kModalPumpTimerId);
        g_in_modal_loop = false;
    }
    SetWindowLongPtrA(g_hwnd, GWLP_WNDPROC,
                      reinterpret_cast<LONG_PTR>(g_orig_wndproc));
    g_hwnd         = nullptr;
    g_orig_wndproc = nullptr;
}

}  // namespace FM2KWndProc
