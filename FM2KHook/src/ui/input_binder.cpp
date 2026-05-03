// input_binder.cpp
// See input_binder.h. Self-contained: SDL3 + ImGui + std lib only.
// Sample_Win32 also pulls in the Win32 + XInput headers (Windows-only).

#include "input_binder.h"

#include <SDL3/SDL.h>
#include <imgui.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <xinput.h>
#endif

namespace FM2KInputBinder {

namespace {

constexpr int kPlayers = 2;
constexpr float kAxisBindThreshold = 0.45f;
constexpr int   kAxisSampleThreshold = 16384;  // ~50% of int16 range

PlayerBindings g_players[kPlayers];

// Capture state -- only one row at a time across both players.
struct CaptureCtx {
    bool active        = false;
    int  player        = -1;
    int  bit           = -1;
    bool armed         = false;  // wait for all buttons released first
};
CaptureCtx g_capture;

// SDL gamepad handles we've opened. Keyed by joystick instance id.
struct GamepadEntry {
    SDL_Gamepad* handle = nullptr;
    int          index  = -1;  // index into the cached id array
};
std::vector<SDL_JoystickID> g_gamepad_ids;
std::unordered_map<SDL_JoystickID, SDL_Gamepad*> g_gamepad_handles;

std::string g_config_path;
bool g_initialized = false;

// ---------------------------------------------------------------------------
// Defaults
// ---------------------------------------------------------------------------

void ApplyDefaultsP1(PlayerBindings& pb) {
    auto kb = [&](Bit b, SDL_Scancode sc) {
        Binding& s = pb.bits[(size_t)b];
        s.source = Binding::Source::KEYBOARD;
        s.code = (int)sc;
        s.axis_dir = 0;
        s.gamepad_index = -1;
    };
    kb(Bit::LEFT,  SDL_SCANCODE_LEFT);
    kb(Bit::RIGHT, SDL_SCANCODE_RIGHT);
    kb(Bit::UP,    SDL_SCANCODE_UP);
    kb(Bit::DOWN,  SDL_SCANCODE_DOWN);
    // 6 attack buttons + Start, matching 2D Fighter Maker 2002's KEYINPUT
    // layout convention. Z X C V on the lower row, A S D on the upper row,
    // Enter for Start — same defaults LilithPort and most FM2K-era setups
    // shipped with so existing players don't have to relearn their bind.
    kb(Bit::A,     SDL_SCANCODE_Z);
    kb(Bit::B,     SDL_SCANCODE_X);
    kb(Bit::C,     SDL_SCANCODE_C);
    kb(Bit::D,     SDL_SCANCODE_A);
    kb(Bit::E,     SDL_SCANCODE_S);
    kb(Bit::F,     SDL_SCANCODE_D);
    kb(Bit::START, SDL_SCANCODE_RETURN);
}

void ApplyDefaults(int player) {
    PlayerBindings& pb = g_players[player];
    for (auto& s : pb.bits) s = Binding{};
    if (player == 0) ApplyDefaultsP1(pb);
}

// ---------------------------------------------------------------------------
// Gamepad management
// ---------------------------------------------------------------------------

void RefreshGamepadList() {
    g_gamepad_ids.clear();
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    if (ids) {
        for (int i = 0; i < count; ++i) {
            SDL_JoystickID jid = ids[i];
            g_gamepad_ids.push_back(jid);
            if (g_gamepad_handles.find(jid) == g_gamepad_handles.end()) {
                SDL_Gamepad* gp = SDL_OpenGamepad(jid);
                if (gp) g_gamepad_handles[jid] = gp;
            }
        }
        SDL_free(ids);
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

// ---------------------------------------------------------------------------
// Display helpers
// ---------------------------------------------------------------------------

// Wire / display name per FM2K bit. These names also serve as the INI keys
// in fm2k_inputs.ini, so renaming them is a wire-format break — old configs
// with "BTN5" / "BTN6" / "BTN7" lines will silently fail to load and fall
// back to defaults. That's an acceptable one-time cost for matching the
// 2DFM convention; the binder ships in v1 so very few configs exist.
const char* kBitNames[(size_t)Bit::COUNT] = {
    "LEFT", "RIGHT", "UP", "DOWN", "A", "B", "C", "D", "E", "F", "START"
};

std::string BindingLabel(const Binding& b) {
    switch (b.source) {
        case Binding::Source::NONE:
            return "<unbound>";
        case Binding::Source::KEYBOARD: {
            const char* n = SDL_GetScancodeName((SDL_Scancode)b.code);
            if (!n || !*n) return "Key " + std::to_string(b.code);
            return std::string("Key: ") + n;
        }
        case Binding::Source::GAMEPAD_BUTTON: {
            const char* n = SDL_GetGamepadStringForButton((SDL_GamepadButton)b.code);
            char buf[64];
            std::snprintf(buf, sizeof(buf), "Pad%d Btn:%s", b.gamepad_index,
                          (n && *n) ? n : std::to_string(b.code).c_str());
            return buf;
        }
        case Binding::Source::GAMEPAD_AXIS: {
            const char* n = SDL_GetGamepadStringForAxis((SDL_GamepadAxis)b.code);
            char buf[64];
            std::snprintf(buf, sizeof(buf), "Pad%d Axis:%s%s", b.gamepad_index,
                          (n && *n) ? n : std::to_string(b.code).c_str(),
                          b.axis_dir < 0 ? "-" : "+");
            return buf;
        }
    }
    return "?";
}

bool BindingsConflict(const Binding& a, const Binding& b) {
    if (a.source != b.source || a.source == Binding::Source::NONE) return false;
    if (a.source == Binding::Source::KEYBOARD) return a.code == b.code;
    // Gamepad sources also need same gamepad index.
    if (a.gamepad_index != b.gamepad_index) return false;
    if (a.source == Binding::Source::GAMEPAD_BUTTON) return a.code == b.code;
    return a.code == b.code && a.axis_dir == b.axis_dir;
}

// ---------------------------------------------------------------------------
// Capture: poll for a fresh press and write it into the target slot.
// ---------------------------------------------------------------------------

bool AnyInputHeld() {
    int nkeys = 0;
    const bool* ks = SDL_GetKeyboardState(&nkeys);
    if (ks) {
        for (int i = 0; i < nkeys; ++i) if (ks[i]) return true;
    }
    for (auto& kv : g_gamepad_handles) {
        SDL_Gamepad* gp = kv.second;
        if (!gp) continue;
        for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; ++b) {
            if (SDL_GetGamepadButton(gp, (SDL_GamepadButton)b)) return true;
        }
        for (int a = 0; a < SDL_GAMEPAD_AXIS_COUNT; ++a) {
            Sint16 v = SDL_GetGamepadAxis(gp, (SDL_GamepadAxis)a);
            if (v > kAxisSampleThreshold || v < -kAxisSampleThreshold) return true;
        }
    }
    return false;
}

// Returns true and fills `out` if a fresh press was captured.
// Returns true with `cancelled = true` if ESC was hit.
bool PollCapture(Binding& out, bool& cancelled) {
    cancelled = false;
    int nkeys = 0;
    const bool* ks = SDL_GetKeyboardState(&nkeys);

    // ESC always cancels.
    if (ks && SDL_SCANCODE_ESCAPE < nkeys && ks[SDL_SCANCODE_ESCAPE]) {
        cancelled = true;
        return true;
    }

    if (ks) {
        for (int sc = 0; sc < nkeys; ++sc) {
            if (sc == SDL_SCANCODE_ESCAPE) continue;
            if (ks[sc]) {
                out.source = Binding::Source::KEYBOARD;
                out.code = sc;
                out.axis_dir = 0;
                out.gamepad_index = -1;
                return true;
            }
        }
    }

    // Gamepads.
    for (size_t i = 0; i < g_gamepad_ids.size(); ++i) {
        SDL_Gamepad* gp = GamepadAt((int)i);
        if (!gp) continue;
        for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; ++b) {
            if (SDL_GetGamepadButton(gp, (SDL_GamepadButton)b)) {
                out.source = Binding::Source::GAMEPAD_BUTTON;
                out.code = b;
                out.axis_dir = 0;
                out.gamepad_index = (int)i;
                return true;
            }
        }
        for (int a = 0; a < SDL_GAMEPAD_AXIS_COUNT; ++a) {
            Sint16 v = SDL_GetGamepadAxis(gp, (SDL_GamepadAxis)a);
            if (v > kAxisSampleThreshold || v < -kAxisSampleThreshold) {
                out.source = Binding::Source::GAMEPAD_AXIS;
                out.code = a;
                out.axis_dir = (v > 0) ? 1 : -1;
                out.gamepad_index = (int)i;
                return true;
            }
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// INI persistence
// ---------------------------------------------------------------------------

std::string DefaultConfigPath() {
    if (const char* env = std::getenv("FM2K_INPUT_CONFIG_PATH")) {
        if (env[0]) return env;
    }
#ifdef _WIN32
    // Anchor in %APPDATA%\FM2K_Rollback so the launcher EXE and the
    // injected hook DLL — which live in different working directories
    // — resolve to THE SAME path. CWD-relative was bugged: launcher
    // saved at e.g. C:\games\fm2k_inputs.ini, hook stat'd
    // C:\games\2dfm\wanwan\fm2k_inputs.ini. File never found in-game.
    if (const char* appdata = std::getenv("APPDATA")) {
        if (appdata[0]) {
            std::string dir = std::string(appdata) + "\\FM2K_Rollback";
            CreateDirectoryA(dir.c_str(), nullptr);  // ok if it already exists
            return dir + "\\fm2k_inputs.ini";
        }
    }
#endif
    return "fm2k_inputs.ini";
}

void Trim(std::string& s) {
    size_t a = 0;
    while (a < s.size() && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
    size_t b = s.size();
    while (b > a && (s[b-1] == ' ' || s[b-1] == '\t' || s[b-1] == '\r' || s[b-1] == '\n')) --b;
    s = s.substr(a, b - a);
}

void WriteBinding(FILE* f, const char* key, const Binding& b) {
    switch (b.source) {
        case Binding::Source::NONE:
            std::fprintf(f, "%s=none\n", key);
            break;
        case Binding::Source::KEYBOARD:
            std::fprintf(f, "%s=kbd:%d\n", key, b.code);
            break;
        case Binding::Source::GAMEPAD_BUTTON:
            std::fprintf(f, "%s=padbtn:%d:%d\n", key, b.gamepad_index, b.code);
            break;
        case Binding::Source::GAMEPAD_AXIS:
            std::fprintf(f, "%s=padaxis:%d:%d:%d\n", key,
                         b.gamepad_index, b.code, b.axis_dir);
            break;
    }
}

bool ParseBinding(const std::string& v, Binding& out) {
    out = Binding{};
    if (v == "none" || v.empty()) return true;
    // Split on ':' lazily.
    auto next = [&](size_t& pos) -> std::string {
        size_t e = v.find(':', pos);
        std::string s = v.substr(pos, e == std::string::npos ? std::string::npos : e - pos);
        pos = (e == std::string::npos) ? v.size() : e + 1;
        return s;
    };
    size_t p = 0;
    std::string tag = next(p);
    if (tag == "kbd") {
        out.source = Binding::Source::KEYBOARD;
        out.code = std::atoi(next(p).c_str());
        return true;
    } else if (tag == "padbtn") {
        out.source = Binding::Source::GAMEPAD_BUTTON;
        out.gamepad_index = std::atoi(next(p).c_str());
        out.code = std::atoi(next(p).c_str());
        return true;
    } else if (tag == "padaxis") {
        out.source = Binding::Source::GAMEPAD_AXIS;
        out.gamepad_index = std::atoi(next(p).c_str());
        out.code = std::atoi(next(p).c_str());
        out.axis_dir = std::atoi(next(p).c_str());
        return true;
    }
    return false;
}

}  // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

PlayerBindings& Bindings(int player_slot) {
    if (player_slot < 0) player_slot = 0;
    if (player_slot >= kPlayers) player_slot = kPlayers - 1;
    return g_players[player_slot];
}

void Init() {
    if (g_initialized) return;
    g_initialized = true;
    g_config_path = DefaultConfigPath();
    // Make sure SDL gamepad subsystem is up. No-op if already initialized.
    if (!SDL_WasInit(SDL_INIT_GAMEPAD)) SDL_InitSubSystem(SDL_INIT_GAMEPAD);
    RefreshGamepadList();
    // Defaults first, then overlay with file contents if present.
    for (int p = 0; p < kPlayers; ++p) ApplyDefaults(p);
    Load();
}

void Shutdown() {
    for (auto& kv : g_gamepad_handles) {
        if (kv.second) SDL_CloseGamepad(kv.second);
    }
    g_gamepad_handles.clear();
    g_gamepad_ids.clear();
    g_initialized = false;
}

bool Save() {
    if (g_config_path.empty()) g_config_path = DefaultConfigPath();
    FILE* f = std::fopen(g_config_path.c_str(), "w");
    if (!f) return false;
    std::fprintf(f, "; FM2K input bindings\n");
    for (int p = 0; p < kPlayers; ++p) {
        std::fprintf(f, "[Player%d]\n", p);
        for (size_t i = 0; i < (size_t)Bit::COUNT; ++i) {
            WriteBinding(f, kBitNames[i], g_players[p].bits[i]);
        }
        std::fprintf(f, "\n");
    }
    std::fclose(f);
    return true;
}

bool Load() {
    if (g_config_path.empty()) g_config_path = DefaultConfigPath();
    FILE* f = std::fopen(g_config_path.c_str(), "r");
    if (!f) return false;

    int section = -1;  // -1 = none, 0/1 = player slot
    char line[512];
    while (std::fgets(line, sizeof(line), f)) {
        std::string s = line;
        Trim(s);
        if (s.empty() || s[0] == ';' || s[0] == '#') continue;
        if (s.front() == '[' && s.back() == ']') {
            std::string sec = s.substr(1, s.size() - 2);
            if (sec == "Player0") section = 0;
            else if (sec == "Player1") section = 1;
            else section = -1;
            continue;
        }
        if (section < 0) continue;

        size_t eq = s.find('=');
        if (eq == std::string::npos) continue;
        std::string key = s.substr(0, eq);
        std::string val = s.substr(eq + 1);
        Trim(key);
        Trim(val);

        for (size_t i = 0; i < (size_t)Bit::COUNT; ++i) {
            if (key == kBitNames[i]) {
                ParseBinding(val, g_players[section].bits[i]);
                break;
            }
        }
    }
    std::fclose(f);
    return true;
}

uint16_t Sample(int player_slot) {
    if (player_slot < 0 || player_slot >= kPlayers) return 0;
    const PlayerBindings& pb = g_players[player_slot];

    int nkeys = 0;
    const bool* ks = SDL_GetKeyboardState(&nkeys);

    uint16_t mask = 0;
    for (size_t i = 0; i < (size_t)Bit::COUNT; ++i) {
        const Binding& b = pb.bits[i];
        bool pressed = false;
        switch (b.source) {
            case Binding::Source::NONE:
                break;
            case Binding::Source::KEYBOARD:
                if (ks && b.code >= 0 && b.code < nkeys) pressed = ks[b.code];
                break;
            case Binding::Source::GAMEPAD_BUTTON: {
                SDL_Gamepad* gp = GamepadAt(b.gamepad_index);
                if (gp) pressed = SDL_GetGamepadButton(gp, (SDL_GamepadButton)b.code);
                break;
            }
            case Binding::Source::GAMEPAD_AXIS: {
                SDL_Gamepad* gp = GamepadAt(b.gamepad_index);
                if (gp) {
                    Sint16 v = SDL_GetGamepadAxis(gp, (SDL_GamepadAxis)b.code);
                    if (b.axis_dir < 0) pressed = (v < -kAxisSampleThreshold);
                    else                pressed = (v >  kAxisSampleThreshold);
                }
                break;
            }
        }
        if (pressed) mask |= (uint16_t)(1u << i);
    }
    return mask & 0x7FF;
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

    uint16_t mask = 0;
    for (size_t i = 0; i < (size_t)Bit::COUNT; ++i) {
        const Binding& b = pb.bits[i];
        bool pressed = false;
        switch (b.source) {
            case Binding::Source::NONE:
                break;
            case Binding::Source::KEYBOARD: {
                int vk = Sdl3ScancodeToVk(b.code);
                if (vk != 0) {
                    pressed = (GetAsyncKeyState(vk) & 0x8000) != 0;
                }
                break;
            }
            case Binding::Source::GAMEPAD_BUTTON: {
                if (const XINPUT_STATE* st = get_xinput(b.gamepad_index)) {
                    WORD xbit = SdlGamepadButtonToXInputBit(b.code);
                    pressed = xbit != 0 && (st->Gamepad.wButtons & xbit) != 0;
                }
                break;
            }
            case Binding::Source::GAMEPAD_AXIS: {
                if (const XINPUT_STATE* st = get_xinput(b.gamepad_index)) {
                    SHORT v = 0;
                    switch (b.code) {
                        case SDL_GAMEPAD_AXIS_LEFTX:        v = st->Gamepad.sThumbLX; break;
                        case SDL_GAMEPAD_AXIS_LEFTY:        v = (SHORT)-st->Gamepad.sThumbLY; break;  // invert Y to match SDL3 sign
                        case SDL_GAMEPAD_AXIS_RIGHTX:       v = st->Gamepad.sThumbRX; break;
                        case SDL_GAMEPAD_AXIS_RIGHTY:       v = (SHORT)-st->Gamepad.sThumbRY; break;
                        case SDL_GAMEPAD_AXIS_LEFT_TRIGGER: v = (SHORT)(st->Gamepad.bLeftTrigger * 128); break;
                        case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER:v = (SHORT)(st->Gamepad.bRightTrigger * 128); break;
                        default: break;
                    }
                    if (b.axis_dir < 0) pressed = (v < -kAxisSampleThreshold);
                    else                pressed = (v >  kAxisSampleThreshold);
                }
                break;
            }
        }
        if (pressed) mask |= (uint16_t)(1u << i);
    }
    return mask & 0x7FF;
#endif
}

bool RenderWindow(int player_slot, bool* p_open) {
    if (!g_initialized) Init();
    if (player_slot < 0 || player_slot >= kPlayers) return false;

    bool changed = false;

    // Cheap periodic gamepad refresh -- handles hot-plug.
    static double s_last_refresh = 0.0;
    double now = ImGui::GetTime();
    if (now - s_last_refresh > 1.5) { RefreshGamepadList(); s_last_refresh = now; }

    char title[64];
    std::snprintf(title, sizeof(title), "FM2K Input Binder - Player %d", player_slot);

    ImGui::SetNextWindowSize(ImVec2(420, 360), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(title, p_open)) { ImGui::End(); return false; }

    PlayerBindings& pb = g_players[player_slot];

    // Header / utility row
    ImGui::Text("Gamepads detected: %d", (int)g_gamepad_ids.size());
    if (!g_gamepad_ids.empty()) {
        ImGui::Indent();
        for (size_t i = 0; i < g_gamepad_ids.size(); ++i) {
            ImGui::Text("  [%zu] %s", i, GamepadNameAt((int)i));
        }
        ImGui::Unindent();
    }

    if (ImGui::Button("Save")) {
        if (Save()) changed = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Reload")) {
        for (int p = 0; p < kPlayers; ++p) ApplyDefaults(p);
        Load();
        changed = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset Defaults")) {
        ApplyDefaults(player_slot);
        changed = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Refresh Pads")) {
        RefreshGamepadList();
    }

    ImGui::Separator();

    // Compute conflicts within this player.
    bool conflict[(size_t)Bit::COUNT] = {};
    for (size_t i = 0; i < (size_t)Bit::COUNT; ++i) {
        for (size_t j = i + 1; j < (size_t)Bit::COUNT; ++j) {
            if (BindingsConflict(pb.bits[i], pb.bits[j])) {
                conflict[i] = conflict[j] = true;
            }
        }
    }

    // Capture handling -- only act on rows owned by this player+bit.
    if (g_capture.active && g_capture.player == player_slot) {
        if (!g_capture.armed) {
            // Wait for full release before sampling so the click that
            // started the capture isn't read back.
            if (!AnyInputHeld()) g_capture.armed = true;
        } else {
            Binding cap{};
            bool cancel = false;
            if (PollCapture(cap, cancel)) {
                if (!cancel) {
                    pb.bits[g_capture.bit] = cap;
                    changed = true;
                }
                g_capture.active = false;
                g_capture.armed = false;
                g_capture.player = -1;
                g_capture.bit = -1;
            }
        }
    }

    // Bindings table
    if (ImGui::BeginTable("bindings", 3,
            ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg |
            ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Bit",   ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableSetupColumn("Bound", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("",      ImGuiTableColumnFlags_WidthFixed, 130.0f);
        ImGui::TableHeadersRow();

        for (size_t i = 0; i < (size_t)Bit::COUNT; ++i) {
            ImGui::TableNextRow();
            ImGui::PushID((int)i);

            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(kBitNames[i]);

            ImGui::TableSetColumnIndex(1);
            const bool waiting = g_capture.active &&
                                 g_capture.player == player_slot &&
                                 g_capture.bit == (int)i;
            if (waiting) {
                ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.2f, 1.0f),
                                   "Press a key/button (ESC to cancel)...");
            } else if (conflict[i]) {
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s",
                                   BindingLabel(pb.bits[i]).c_str());
            } else {
                ImGui::TextUnformatted(BindingLabel(pb.bits[i]).c_str());
            }

            ImGui::TableSetColumnIndex(2);
            if (waiting) {
                if (ImGui::Button("Cancel", ImVec2(60, 0))) {
                    g_capture.active = false;
                    g_capture.armed = false;
                    g_capture.player = -1;
                    g_capture.bit = -1;
                }
            } else {
                if (ImGui::Button("Bind", ImVec2(60, 0))) {
                    g_capture.active = true;
                    g_capture.armed = false;  // wait for release
                    g_capture.player = player_slot;
                    g_capture.bit = (int)i;
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Clear", ImVec2(60, 0))) {
                pb.bits[i] = Binding{};
                changed = true;
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    ImGui::Separator();
    ImGui::Text("Sampled mask: 0x%03X", (unsigned)Sample(player_slot));

    ImGui::End();
    return changed;
}

}  // namespace FM2KInputBinder
