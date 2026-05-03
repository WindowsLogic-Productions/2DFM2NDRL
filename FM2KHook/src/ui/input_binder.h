// input_binder.h
// Minimal ImGui-based input binder for FM2K's 11-bit input format.
// Drop-in for either the launcher (SDL3 + ImGui) or the hook DLL's overlay.
// Depends only on <SDL3/SDL.h>, <imgui.h>, and the C++ standard library.

#pragma once

#include <cstddef>
#include <cstdint>

namespace FM2KInputBinder {

// FM2K's 11-bit input layout (mask 0x7FF). Standard 2DFM convention is
// 6 attack buttons (A..F) plus a Start button — same layout 2D Fighter
// Maker 2002 ships in its KEYINPUT / JOYINPUT dialog.
enum class Bit : uint8_t {
    LEFT  = 0,   // 0x001
    RIGHT = 1,   // 0x002
    UP    = 2,   // 0x004
    DOWN  = 3,   // 0x008
    A     = 4,   // 0x010 (button1)
    B     = 5,   // 0x020 (button2)
    C     = 6,   // 0x040 (button3)
    D     = 7,   // 0x080 (button4)
    E     = 8,   // 0x100 (button5)
    F     = 9,   // 0x200 (button6)
    START = 10,  // 0x400 (button7 / start)
    COUNT
};

// One slot per FM2K bit. A slot is either keyboard, gamepad button,
// gamepad axis, or empty -- mutually exclusive for simplicity.
struct Binding {
    enum class Source : uint8_t { NONE, KEYBOARD, GAMEPAD_BUTTON, GAMEPAD_AXIS };

    Source source        = Source::NONE;
    int    code          = 0;   // SDL_Scancode | SDL_GamepadButton | SDL_GamepadAxis
    int    axis_dir      = 0;   // +1 / -1 for GAMEPAD_AXIS, ignored otherwise
    int    gamepad_index = -1;  // index into SDL_GetGamepads(); -1 = first connected
};

struct PlayerBindings {
    Binding bits[static_cast<size_t>(Bit::COUNT)];
};

// Initialize: opens any visible gamepads and loads bindings from
// $FM2K_INPUT_CONFIG_PATH (or "fm2k_inputs.ini" in CWD). Falls back to
// defaults silently if no file exists. Safe to call once at startup.
void Init();

// Tear down opened gamepad handles. Optional.
void Shutdown();

// Render the binder window for one player slot (0 or 1). Returns true if
// any binding was modified this frame (caller may auto-Save).
bool RenderWindow(int player_slot, bool* p_open = nullptr);

// Sample current state of all bindings for player_slot, returning the
// 11-bit FM2K input mask. Call once per frame from your input source.
//
// Sample()      uses SDL3 — for the LAUNCHER (SDL3 event pump runs).
// Sample_Win32() uses GetAsyncKeyState + XInput — for the HOOK DLL where
//                 SDL3 isn't event-pumped. Both honor the same Bindings()
//                 config so launcher-bound keys work identically in-game.
uint16_t Sample(int player_slot);
uint16_t Sample_Win32(int player_slot);

// Persistence. Save() writes to "fm2k_inputs.ini" (or the path used by Load).
bool Save();
bool Load();

// Direct access (for diagnostic UI, etc.).
PlayerBindings& Bindings(int player_slot);

}  // namespace FM2KInputBinder
