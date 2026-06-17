// input_binder_gamepads.cpp -- SDL3 gamepad discovery/lifecycle.
// RefreshGamepadList + GamepadNameAt, promoted to external linkage so the
// core lifecycle + the ui device dropdown can both call them.
#include "input_binder.h"
#include "input_binder_internal.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <vector>

namespace FM2KInputBinder {
// ---------------------------------------------------------------------------
// Gamepad management
// ---------------------------------------------------------------------------

void RefreshGamepadList() {
    // Reentrancy guard. SDL_PumpEvents below dispatches Win32 messages
    // into the wndproc subclass (modal pump), which can tick code that
    // calls RefreshGamepads() again -- the reentrant call mutates
    // g_gamepad_handles / g_non_gamepad_ids while THIS frame iterates
    // them, invalidating iterators: AV inside unordered_map internals
    // (hash<unsigned> _M_cget), observed twice on spectator instances
    // under 20% loss (2026-06-11, FM2KHook+0x38d944). Skipping the
    // nested refresh is always safe -- the outer one finishes the scan.
    static bool s_refresh_in_progress = false;
    if (s_refresh_in_progress) return;
    s_refresh_in_progress = true;
    struct RefreshGuard {
        bool* flag;
        ~RefreshGuard() { *flag = false; }
    } guard{&s_refresh_in_progress};

    g_gamepad_ids.clear();
    // Pump events first — a freshly-plugged stick may not show up in
    // SDL_GetGamepads / SDL_GetJoysticks until SDL has processed its
    // own JOYSTICK_ADDED event.
    SDL_PumpEvents();

    // Pass 1: SDL-known gamepads (have a built-in mapping).
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    if (ids) {
        for (int i = 0; i < count; ++i) {
            SDL_JoystickID jid = ids[i];
            g_gamepad_ids.push_back(jid);
            if (g_gamepad_handles.find(jid) == g_gamepad_handles.end()) {
                SDL_Gamepad* gp = SDL_OpenGamepad(jid);
                if (gp) {
                    g_gamepad_handles[jid] = gp;
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "InputBinder: opened gamepad jid=%u name='%s'",
                        (unsigned)jid,
                        SDL_GetGamepadName(gp) ? SDL_GetGamepadName(gp) : "?");
                }
            }
        }
        SDL_free(ids);
    }

    // Pass 2: walk ALL joysticks. Some sticks (Qanba Obsidian in
    // generic-HID modes, third-party PS3 clones, etc.) don't appear
    // in SDL_GetGamepads but CAN be opened as gamepads via SDL3's
    // synthetic mapping. Mirroring revolve_input_sdl3's pattern
    // here — without it, only XInput-recognized devices showed up
    // in our binder UI.
    int joy_count = 0;
    SDL_JoystickID* joys = SDL_GetJoysticks(&joy_count);
    std::unordered_set<SDL_JoystickID> present_joysticks;
    if (joys) {
        for (int i = 0; i < joy_count; ++i) {
            SDL_JoystickID jid = joys[i];
            present_joysticks.insert(jid);
            // Skip if pass 1 already opened it, or if we already examined it
            // and found it is not a gamepad. The reject-cache (below) is what
            // stops the per-refresh SDL_OpenJoystick+SDL_CloseJoystick on a
            // non-gamepad stick (vJoy etc.) -- the ~46ms/sec hitch = "#63 95fps".
            if (g_gamepad_handles.find(jid) != g_gamepad_handles.end()) continue;
            if (g_non_gamepad_ids.find(jid) != g_non_gamepad_ids.end()) continue;
            // SDL_IsGamepad sometimes returns false right after a
            // joystick-added event before SDL loads the device's
            // mapping. Pump + retry once before giving up.
            bool is_gp = SDL_IsGamepad(jid);
            if (!is_gp) { SDL_PumpEvents(); is_gp = SDL_IsGamepad(jid); }
            if (!is_gp) {
                // Diagnostic — name + GUID for sticks we couldn't
                // promote to a gamepad. Helps the user paste this
                // info if they need a custom mapping added.
                SDL_Joystick* j = SDL_OpenJoystick(jid);
                if (j) {
                    char guid[64] = {};
                    SDL_GUIDToString(SDL_GetJoystickGUID(j), guid, sizeof(guid));
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "InputBinder: joystick jid=%u name='%s' guid=%s "
                        "(no SDL gamepad mapping — won't appear in binder)",
                        (unsigned)jid,
                        SDL_GetJoystickName(j) ? SDL_GetJoystickName(j) : "?",
                        guid);
                    SDL_CloseJoystick(j);
                }
                // Remember it's not a gamepad so we never re-open/close it
                // on subsequent refreshes (the #63 per-second hitch).
                g_non_gamepad_ids.insert(jid);
                continue;
            }
            SDL_Gamepad* gp = SDL_OpenGamepad(jid);
            if (!gp) { SDL_PumpEvents(); gp = SDL_OpenGamepad(jid); }
            if (gp) {
                g_gamepad_ids.push_back(jid);
                g_gamepad_handles[jid] = gp;
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "InputBinder: promoted joystick jid=%u to gamepad — name='%s'",
                    (unsigned)jid,
                    SDL_GetGamepadName(gp) ? SDL_GetGamepadName(gp) : "?");
            }
        }
        SDL_free(joys);
    }

    // Forget rejected sticks that were unplugged, so a re-plug gets
    // examined once more (and isn't skipped forever by the cache above).
    for (auto it = g_non_gamepad_ids.begin(); it != g_non_gamepad_ids.end();) {
        if (present_joysticks.find(*it) == present_joysticks.end())
            it = g_non_gamepad_ids.erase(it);
        else
            ++it;
    }

    // Drop any handles that disappeared.
    for (auto it = g_gamepad_handles.begin(); it != g_gamepad_handles.end();) {
        bool still_present = std::find(g_gamepad_ids.begin(), g_gamepad_ids.end(),
                                       it->first) != g_gamepad_ids.end();
        if (!still_present) {
            if (it->second) SDL_CloseGamepad(it->second);
            it = g_gamepad_handles.erase(it);
        } else {
            ++it;
        }
    }
}

SDL_Gamepad* GamepadAt(int idx) {
    // idx == -1 means "first connected".
    if (idx < 0) {
        if (g_gamepad_ids.empty()) return nullptr;
        auto it = g_gamepad_handles.find(g_gamepad_ids.front());
        return it == g_gamepad_handles.end() ? nullptr : it->second;
    }
    if (idx >= (int)g_gamepad_ids.size()) return nullptr;
    auto it = g_gamepad_handles.find(g_gamepad_ids[idx]);
    return it == g_gamepad_handles.end() ? nullptr : it->second;
}

const char* GamepadNameAt(int idx) {
    SDL_Gamepad* gp = GamepadAt(idx);
    if (!gp) return "(no gamepad)";
    const char* n = SDL_GetGamepadName(gp);
    return n ? n : "(unknown)";
}
}  // namespace FM2KInputBinder
