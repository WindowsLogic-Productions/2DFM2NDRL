// input_binder_sample.cpp -- engine-facing input read (NOT ImGui).
// Sample() (SDL3, launcher) + Sample_Win32() (GetAsyncKeyState + XInput,
// hook DLL). Static helpers keep internal linkage.
#include "input_binder.h"
#include "input_binder_internal.h"
#include <SDL3/SDL.h>
#include <cstring>
#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <xinput.h>
#endif

namespace FM2KInputBinder {
// Single-binding sampler used by Sample() — pulled out of the per-bit
// switch so the caller can OR primary + alt slots through the same code
// path. Empty / NONE bindings return false (no contribution to mask).
static bool SampleOne_SDL(const Binding& b, const bool* ks, int nkeys) {
    switch (b.source) {
        case Binding::Source::NONE:
            return false;
        case Binding::Source::KEYBOARD:
            return ks && b.code >= 0 && b.code < nkeys && ks[b.code];
        case Binding::Source::GAMEPAD_BUTTON: {
            SDL_Gamepad* gp = GamepadAt(b.gamepad_index);
            return gp && SDL_GetGamepadButton(gp, (SDL_GamepadButton)b.code);
        }
        case Binding::Source::GAMEPAD_AXIS: {
            SDL_Gamepad* gp = GamepadAt(b.gamepad_index);
            if (!gp) return false;
            Sint16 v = SDL_GetGamepadAxis(gp, (SDL_GamepadAxis)b.code);
            return (b.axis_dir < 0) ? (v < -kAxisSampleThreshold)
                                    : (v >  kAxisSampleThreshold);
        }
    }
    return false;
}

uint16_t Sample(int player_slot) {
    if (player_slot < 0 || player_slot >= kPlayers) return 0;
    const PlayerBindings& pb = g_players[player_slot];

    int nkeys = 0;
    const bool* ks = SDL_GetKeyboardState(&nkeys);

    uint16_t mask = 0;
    for (size_t i = 0; i < (size_t)Bit::COUNT; ++i) {
        const bool pressed = SampleOne_SDL(pb.bits[i],     ks, nkeys)
                          || SampleOne_SDL(pb.bits_alt[i], ks, nkeys);
        if (pressed) mask |= (uint16_t)(1u << i);
    }
    return mask & kFullInputMask;
}

// SDL3 scancode → Windows VK lookup. Covers the keys fighting-game players
// actually bind: letters, digits, arrows, modifiers, F-keys, space, enter,
// tab, escape, backspace, basic punctuation. Anything else returns 0.
//
// SDL3 scancodes are USB HID position codes. We hand-roll a switch instead
// of MapVirtualKey because MapVirtualKey wants Windows scancodes (BIOS set 1)
// not HID scancodes — same-shape mapping for letters but diverges on
// extended keys (arrows etc.).
#ifdef _WIN32
static int Sdl3ScancodeToVk(int sc) {
    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z) {
        return 'A' + (sc - SDL_SCANCODE_A);
    }
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9) {
        return '1' + (sc - SDL_SCANCODE_1);
    }
    if (sc == SDL_SCANCODE_0) return '0';
    if (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F12) {
        return VK_F1 + (sc - SDL_SCANCODE_F1);
    }
    switch (sc) {
        case SDL_SCANCODE_RETURN:    return VK_RETURN;
        case SDL_SCANCODE_ESCAPE:    return VK_ESCAPE;
        case SDL_SCANCODE_BACKSPACE: return VK_BACK;
        case SDL_SCANCODE_TAB:       return VK_TAB;
        case SDL_SCANCODE_SPACE:     return VK_SPACE;
        case SDL_SCANCODE_LEFT:      return VK_LEFT;
        case SDL_SCANCODE_RIGHT:     return VK_RIGHT;
        case SDL_SCANCODE_UP:        return VK_UP;
        case SDL_SCANCODE_DOWN:      return VK_DOWN;
        case SDL_SCANCODE_LCTRL:     return VK_LCONTROL;
        case SDL_SCANCODE_RCTRL:     return VK_RCONTROL;
        case SDL_SCANCODE_LSHIFT:    return VK_LSHIFT;
        case SDL_SCANCODE_RSHIFT:    return VK_RSHIFT;
        case SDL_SCANCODE_LALT:      return VK_LMENU;
        case SDL_SCANCODE_RALT:      return VK_RMENU;
        case SDL_SCANCODE_INSERT:    return VK_INSERT;
        case SDL_SCANCODE_DELETE:    return VK_DELETE;
        case SDL_SCANCODE_HOME:      return VK_HOME;
        case SDL_SCANCODE_END:       return VK_END;
        case SDL_SCANCODE_PAGEUP:    return VK_PRIOR;
        case SDL_SCANCODE_PAGEDOWN:  return VK_NEXT;
        case SDL_SCANCODE_GRAVE:     return VK_OEM_3;
        case SDL_SCANCODE_MINUS:     return VK_OEM_MINUS;
        case SDL_SCANCODE_EQUALS:    return VK_OEM_PLUS;
        case SDL_SCANCODE_LEFTBRACKET:  return VK_OEM_4;
        case SDL_SCANCODE_RIGHTBRACKET: return VK_OEM_6;
        case SDL_SCANCODE_BACKSLASH: return VK_OEM_5;
        case SDL_SCANCODE_SEMICOLON: return VK_OEM_1;
        case SDL_SCANCODE_APOSTROPHE:return VK_OEM_7;
        case SDL_SCANCODE_COMMA:     return VK_OEM_COMMA;
        case SDL_SCANCODE_PERIOD:    return VK_OEM_PERIOD;
        case SDL_SCANCODE_SLASH:     return VK_OEM_2;
        case SDL_SCANCODE_KP_0:      return VK_NUMPAD0;
        case SDL_SCANCODE_KP_1:      return VK_NUMPAD1;
        case SDL_SCANCODE_KP_2:      return VK_NUMPAD2;
        case SDL_SCANCODE_KP_3:      return VK_NUMPAD3;
        case SDL_SCANCODE_KP_4:      return VK_NUMPAD4;
        case SDL_SCANCODE_KP_5:      return VK_NUMPAD5;
        case SDL_SCANCODE_KP_6:      return VK_NUMPAD6;
        case SDL_SCANCODE_KP_7:      return VK_NUMPAD7;
        case SDL_SCANCODE_KP_8:      return VK_NUMPAD8;
        case SDL_SCANCODE_KP_9:      return VK_NUMPAD9;
        case SDL_SCANCODE_KP_ENTER:  return VK_RETURN;
        case SDL_SCANCODE_KP_PLUS:   return VK_ADD;
        case SDL_SCANCODE_KP_MINUS:  return VK_SUBTRACT;
        case SDL_SCANCODE_KP_MULTIPLY: return VK_MULTIPLY;
        case SDL_SCANCODE_KP_DIVIDE: return VK_DIVIDE;
        default:                     return 0;
    }
}
#endif

// SDL3 gamepad button enum → XInput button bit. We bind via SDL3 names
// in the launcher (so the user picks "South" / "East"), then resolve
// to XInput at sample-time inside the hook DLL. Covers the standard
// 360/X1 button layout.
#ifdef _WIN32
static WORD SdlGamepadButtonToXInputBit(int b) {
    switch (b) {
        case SDL_GAMEPAD_BUTTON_SOUTH:           return XINPUT_GAMEPAD_A;
        case SDL_GAMEPAD_BUTTON_EAST:            return XINPUT_GAMEPAD_B;
        case SDL_GAMEPAD_BUTTON_WEST:            return XINPUT_GAMEPAD_X;
        case SDL_GAMEPAD_BUTTON_NORTH:           return XINPUT_GAMEPAD_Y;
        case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:   return XINPUT_GAMEPAD_LEFT_SHOULDER;
        case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER:  return XINPUT_GAMEPAD_RIGHT_SHOULDER;
        case SDL_GAMEPAD_BUTTON_BACK:            return XINPUT_GAMEPAD_BACK;
        case SDL_GAMEPAD_BUTTON_START:           return XINPUT_GAMEPAD_START;
        case SDL_GAMEPAD_BUTTON_LEFT_STICK:      return XINPUT_GAMEPAD_LEFT_THUMB;
        case SDL_GAMEPAD_BUTTON_RIGHT_STICK:     return XINPUT_GAMEPAD_RIGHT_THUMB;
        case SDL_GAMEPAD_BUTTON_DPAD_UP:         return XINPUT_GAMEPAD_DPAD_UP;
        case SDL_GAMEPAD_BUTTON_DPAD_DOWN:       return XINPUT_GAMEPAD_DPAD_DOWN;
        case SDL_GAMEPAD_BUTTON_DPAD_LEFT:       return XINPUT_GAMEPAD_DPAD_LEFT;
        case SDL_GAMEPAD_BUTTON_DPAD_RIGHT:      return XINPUT_GAMEPAD_DPAD_RIGHT;
        default:                                 return 0;
    }
}
#endif

// Win32-native sampler — for use INSIDE the hook DLL where SDL3 isn't
// event-pumped. Uses GetAsyncKeyState (keyboard) + XInputGetState
// (gamepad). Honors the same g_players bindings the launcher's Sample()
// reads, so launcher-bound keys behave identically in-game.
//
// Caller MUST gate on focus (e.g. GetForegroundWindow == game window) —
// GetAsyncKeyState is global and would read keys pressed in other windows
// otherwise. Same gating Input_CaptureLocal already has.
uint16_t Sample_Win32(int player_slot) {
#ifndef _WIN32
    (void)player_slot;
    return 0;
#else
    if (player_slot < 0 || player_slot >= kPlayers) return 0;
    const PlayerBindings& pb = g_players[player_slot];

    // SDL3 gamepad polling for DInput / HIDAPI sticks (PS3, PS4 in
    // PS3 mode, Qanba in PS3/PS4 modes, generic HID controllers).
    // XInput-only sticks fall through to the XInput path below.
    // Init() opens the devices; we just need to pump events here so
    // the polled state behind SDL_GetGamepadButton stays fresh inside
    // the hooked game process (the game's main loop doesn't call
    // SDL_PollEvent on our behalf).
    if (g_initialized && !g_gamepad_handles.empty()) {
        SDL_PumpEvents();
    }
    auto sdl_gp = [&](int idx) -> SDL_Gamepad* {
        if (idx < 0 || idx >= (int)g_gamepad_ids.size()) {
            return g_gamepad_ids.empty()
                ? nullptr
                : g_gamepad_handles.count(g_gamepad_ids.front())
                  ? g_gamepad_handles[g_gamepad_ids.front()] : nullptr;
        }
        auto it = g_gamepad_handles.find(g_gamepad_ids[idx]);
        return it == g_gamepad_handles.end() ? nullptr : it->second;
    };

    // XInput is checked once per call; fetched lazily by gamepad index.
    // Most players use a single controller so this is one XInputGetState
    // call per frame in practice.
    XINPUT_STATE xs[4];
    bool         xs_ok[4] = {false, false, false, false};
    auto get_xinput = [&](int idx) -> const XINPUT_STATE* {
        if (idx < 0 || idx >= 4) idx = 0;  // -1 / OOB → controller 0
        if (!xs_ok[idx]) {
            ZeroMemory(&xs[idx], sizeof(xs[idx]));
            xs_ok[idx] = (XInputGetState((DWORD)idx, &xs[idx]) == ERROR_SUCCESS);
        }
        return xs_ok[idx] ? &xs[idx] : nullptr;
    };

    // Per-binding sampler. Defined as a lambda so it captures sdl_gp /
    // get_xinput without re-threading them through a static helper.
    // Called twice per bit (primary + alt) and OR'd, so a single
    // direction can fire from stick AND dpad simultaneously.
    auto sample_one = [&](const Binding& b) -> bool {
        switch (b.source) {
            case Binding::Source::NONE:
                return false;
            case Binding::Source::KEYBOARD: {
                int vk = Sdl3ScancodeToVk(b.code);
                return vk != 0 && (GetAsyncKeyState(vk) & 0x8000) != 0;
            }
            case Binding::Source::GAMEPAD_BUTTON: {
                if (SDL_Gamepad* gp = sdl_gp(b.gamepad_index)) {
                    if (SDL_GetGamepadButton(gp, (SDL_GamepadButton)b.code) != 0) {
                        return true;
                    }
                }
                if (const XINPUT_STATE* st = get_xinput(b.gamepad_index)) {
                    WORD xbit = SdlGamepadButtonToXInputBit(b.code);
                    return xbit != 0 && (st->Gamepad.wButtons & xbit) != 0;
                }
                return false;
            }
            case Binding::Source::GAMEPAD_AXIS: {
                if (SDL_Gamepad* gp = sdl_gp(b.gamepad_index)) {
                    Sint16 v = SDL_GetGamepadAxis(gp, (SDL_GamepadAxis)b.code);
                    bool ok = (b.axis_dir < 0) ? (v < -kAxisSampleThreshold)
                                               : (v >  kAxisSampleThreshold);
                    if (ok) return true;
                }
                if (const XINPUT_STATE* st = get_xinput(b.gamepad_index)) {
                    // Compute in int, not SHORT: negating sThumbL/RY at full
                    // deflection (-32768) overflows a SHORT back to -32768,
                    // which flips full-down into full-up (and vice versa).
                    int v = 0;
                    switch (b.code) {
                        case SDL_GAMEPAD_AXIS_LEFTX:        v = st->Gamepad.sThumbLX; break;
                        case SDL_GAMEPAD_AXIS_LEFTY:        v = -(int)st->Gamepad.sThumbLY; break;
                        case SDL_GAMEPAD_AXIS_RIGHTX:       v = st->Gamepad.sThumbRX; break;
                        case SDL_GAMEPAD_AXIS_RIGHTY:       v = -(int)st->Gamepad.sThumbRY; break;
                        case SDL_GAMEPAD_AXIS_LEFT_TRIGGER: v = st->Gamepad.bLeftTrigger * 128; break;
                        case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER:v = st->Gamepad.bRightTrigger * 128; break;
                        default: break;
                    }
                    return (b.axis_dir < 0) ? (v < -kAxisSampleThreshold)
                                            : (v >  kAxisSampleThreshold);
                }
                return false;
            }
        }
        return false;
    };

    uint16_t mask = 0;
    for (size_t i = 0; i < (size_t)Bit::COUNT; ++i) {
        if (sample_one(pb.bits[i]) || sample_one(pb.bits_alt[i])) {
            mask |= (uint16_t)(1u << i);
        }
    }
    return mask & kFullInputMask;
#endif
}
}  // namespace FM2KInputBinder
