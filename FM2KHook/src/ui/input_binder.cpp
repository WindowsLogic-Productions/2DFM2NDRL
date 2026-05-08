// input_binder.cpp
// See input_binder.h. Self-contained: SDL3 + ImGui + std lib only.
// Sample_Win32 also pulls in the Win32 + XInput headers (Windows-only).

#include "input_binder.h"

#include <SDL3/SDL.h>
#include <imgui.h>

#include <algorithm>
#include <array>
#include <cctype>
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
    bool alt           = false;  // false = primary slot, true = alt slot
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

void ApplyDefaultsP2(PlayerBindings& pb) {
    // P2 defaults: numpad layout so two players can share a single keyboard
    // without conflicts. Same shape as P1 (left/right/up/down + 6 attack
    // buttons + start) but mapped to the right side of a 104-key layout.
    //
    // If a second gamepad is plugged in, the user can rebind P2's slots
    // to it via the binder UI; the gamepad_index field defaults to 1
    // here so the FIRST gamepad-bound row a user adds picks up gamepad
    // index 1 (P1 picks 0). This matches the user-facing convention:
    // P1 = primary device, P2 = second device / keyboard fallback.
    auto kb = [&](Bit b, SDL_Scancode sc) {
        Binding& s = pb.bits[(size_t)b];
        s.source = Binding::Source::KEYBOARD;
        s.code = (int)sc;
        s.axis_dir = 0;
        s.gamepad_index = 1;  // default routes future gamepad bindings to pad #2
    };
    kb(Bit::LEFT,  SDL_SCANCODE_KP_4);
    kb(Bit::RIGHT, SDL_SCANCODE_KP_6);
    kb(Bit::UP,    SDL_SCANCODE_KP_8);
    kb(Bit::DOWN,  SDL_SCANCODE_KP_2);
    // 6 attack buttons + Start. UIOJKL is the canonical "right hand"
    // layout for P2 keyboard share — mirrors P1's ZXCV/ASD (Z X C
    // lower row, A S D upper row) on the home-row keys around U/J.
    kb(Bit::A,     SDL_SCANCODE_J);
    kb(Bit::B,     SDL_SCANCODE_K);
    kb(Bit::C,     SDL_SCANCODE_L);
    kb(Bit::D,     SDL_SCANCODE_U);
    kb(Bit::E,     SDL_SCANCODE_I);
    kb(Bit::F,     SDL_SCANCODE_O);
    kb(Bit::START, SDL_SCANCODE_KP_ENTER);
}

void ApplyDefaults(int player) {
    PlayerBindings& pb = g_players[player];
    for (auto& s : pb.bits)     s = Binding{};
    for (auto& s : pb.bits_alt) s = Binding{};
    if (player == 0)      ApplyDefaultsP1(pb);
    else if (player == 1) ApplyDefaultsP2(pb);
    // Default gamepad layout in the alt slots so a connected pad
    // "just works" without rebinding. Directionals get the LEFT
    // analog stick (matches most fighting-game muscle memory); the
    // dpad is intentionally LEFT EMPTY so a user who wants stick+dpad
    // simultaneously (CXL pattern) can drop the dpad in the primary
    // slot without overwriting a keyboard binding they want to keep.
    // Buttons map to the standard XInput-style face/shoulder layout.
    const int gp_idx = (player == 0) ? 0 : 1;
    auto axis = [&](Bit b, SDL_GamepadAxis a, int dir) {
        Binding& s = pb.bits_alt[(size_t)b];
        s.source = Binding::Source::GAMEPAD_AXIS;
        s.code = (int)a;
        s.axis_dir = dir;
        s.gamepad_index = gp_idx;
    };
    auto btn = [&](Bit b, SDL_GamepadButton gb) {
        Binding& s = pb.bits_alt[(size_t)b];
        s.source = Binding::Source::GAMEPAD_BUTTON;
        s.code = (int)gb;
        s.axis_dir = 0;
        s.gamepad_index = gp_idx;
    };
    axis(Bit::LEFT,  SDL_GAMEPAD_AXIS_LEFTX, -1);
    axis(Bit::RIGHT, SDL_GAMEPAD_AXIS_LEFTX, +1);
    axis(Bit::UP,    SDL_GAMEPAD_AXIS_LEFTY, -1);
    axis(Bit::DOWN,  SDL_GAMEPAD_AXIS_LEFTY, +1);
    // Standard XInput face-button layout: A=south (X on PS), B=east
    // (○), C=west (□), D=north (△), E=L1, F=R1, START=Start.
    btn(Bit::A,     SDL_GAMEPAD_BUTTON_SOUTH);
    btn(Bit::B,     SDL_GAMEPAD_BUTTON_EAST);
    btn(Bit::C,     SDL_GAMEPAD_BUTTON_WEST);
    btn(Bit::D,     SDL_GAMEPAD_BUTTON_NORTH);
    btn(Bit::E,     SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
    btn(Bit::F,     SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
    btn(Bit::START, SDL_GAMEPAD_BUTTON_START);
}

// ---------------------------------------------------------------------------
// Gamepad management
// ---------------------------------------------------------------------------

void RefreshGamepadList() {
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
    if (joys) {
        for (int i = 0; i < joy_count; ++i) {
            SDL_JoystickID jid = joys[i];
            // Skip if pass 1 already opened it.
            if (g_gamepad_handles.find(jid) != g_gamepad_handles.end()) continue;
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
    SDL_PumpEvents();  // refresh polled state
    int nkeys = 0;
    const bool* ks = SDL_GetKeyboardState(&nkeys);
    if (ks) {
        for (int i = 0; i < nkeys; ++i) if (ks[i]) return true;
    }
    // Tighter threshold for the "is something held?" check than for
    // the actual capture: PS3 / Qanba sticks frequently park an axis
    // at full +/- 32767 on plug-in (A3 reported as 32767 on cold
    // boot per user repro). With the capture threshold (16384/50%),
    // those stuck axes pin AnyInputHeld() to true forever and the
    // bind state machine never arms. Use 90% threshold for the held-
    // check — only a deliberately-pushed stick passes — and the
    // tighter 50% threshold still applies for capture itself, so
    // sloppy stick presses still bind cleanly.
    constexpr int kAxisHeldThreshold = 28000;  // ~85% of int16 range
    for (auto& kv : g_gamepad_handles) {
        SDL_Gamepad* gp = kv.second;
        if (!gp) continue;
        for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; ++b) {
            if (SDL_GetGamepadButton(gp, (SDL_GamepadButton)b)) return true;
        }
        for (int a = 0; a < SDL_GAMEPAD_AXIS_COUNT; ++a) {
            Sint16 v = SDL_GetGamepadAxis(gp, (SDL_GamepadAxis)a);
            if (v > kAxisHeldThreshold || v < -kAxisHeldThreshold) return true;
        }
    }
    return false;
}

// Returns true and fills `out` if a fresh press was captured.
// Returns true when a binding was captured. `cancelled` is kept in
// the API for the caller's switch but we no longer treat any specific
// key as a built-in cancel — ESC is bindable now (Pokemon CC and other
// 2DFM games map it to pause). The Cancel button next to the row is
// the explicit out-of-capture path.
bool PollCapture(Binding& out, bool& cancelled) {
    cancelled = false;
    SDL_PumpEvents();  // refresh polled state before reading
    int nkeys = 0;
    const bool* ks = SDL_GetKeyboardState(&nkeys);

    if (ks) {
        for (int sc = 0; sc < nkeys; ++sc) {
            if (ks[sc]) {
                out.source = Binding::Source::KEYBOARD;
                out.code = sc;
                out.axis_dir = 0;
                out.gamepad_index = -1;
                return true;
            }
        }
    }

    // Gamepads. Diagnostic-first: dump every non-zero button/axis
    // we see from any opened gamepad on each poll. If the user clicks
    // Bind and reports "no input registered", the launcher log will
    // tell us whether SDL is seeing the press at all (gamepad isn't
    // really opened) or if our threshold logic is the gate.
    static uint32_t s_diag_emitted = 0;
    for (size_t i = 0; i < g_gamepad_ids.size(); ++i) {
        SDL_Gamepad* gp = GamepadAt((int)i);
        if (!gp) continue;
        for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; ++b) {
            if (SDL_GetGamepadButton(gp, (SDL_GamepadButton)b)) {
                if (s_diag_emitted < 60) {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "InputBinder: poll saw gamepad %zu button %d held",
                        i, b);
                    ++s_diag_emitted;
                }
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
                if (s_diag_emitted < 60) {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "InputBinder: poll saw gamepad %zu axis %d v=%d "
                        "(threshold=%d)",
                        i, a, (int)v, (int)kAxisSampleThreshold);
                    ++s_diag_emitted;
                }
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

// Per-game profile state. When set, Load() and Save() route to
// fm2k_inputs_<game>.ini under %APPDATA%\FM2K_Rollback\; otherwise the
// default fm2k_inputs.ini.
static std::string g_active_game = "";

std::string DefaultProfileBaseDir() {
    if (const char* env = std::getenv("FM2K_INPUT_CONFIG_PATH")) {
        if (env[0]) {
            // env override — derive the base dir by stripping the filename.
            std::string s = env;
            size_t slash = s.find_last_of("/\\");
            return (slash == std::string::npos) ? "." : s.substr(0, slash);
        }
    }
#ifdef _WIN32
    if (const char* appdata = std::getenv("APPDATA")) {
        if (appdata[0]) {
            std::string dir = std::string(appdata) + "\\FM2K_Rollback";
            CreateDirectoryA(dir.c_str(), nullptr);
            return dir;
        }
    }
#endif
    return ".";
}

std::string SanitizeProfileName(const char* name) {
    // Filenames go on disk — strip ONLY the Windows-forbidden chars
    // and ASCII control bytes. Letting non-ASCII through preserves
    // UTF-8 sequences for games shipped by Japanese authors (e.g.
    // ＣＰＷ.exe → "ＣＰＷ" profile name). Original sanitizer treated
    // every byte 0x80+ as "not isalnum" and replaced with '_', which
    // turned full-width filenames into rows of underscores in the
    // launcher UI ("Use override for ＣＰＷ" → "Use override for ___").
    //
    // Win32 NTFS forbids: < > : " / \ | ? * and 0x00-0x1F. Everything
    // else (including the full Unicode BMP and beyond as UTF-8 bytes)
    // is legal in a filename via the W-API.
    std::string out;
    if (!name) return out;
    for (const char* p = name; *p; ++p) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x20) {
            out.push_back('_');                  // control char
            continue;
        }
        switch (c) {
            case '<': case '>': case ':': case '"':
            case '/': case '\\': case '|': case '?': case '*':
                out.push_back('_');
                continue;
        }
        out.push_back((char)c);
    }
    while (!out.empty() && (out.back() == '.' || out.back() == ' ')) out.pop_back();
    return out;
}

std::string DefaultProfilePath() {
    // Anchor in %APPDATA%\FM2K_Rollback so the launcher EXE and the
    // injected hook DLL — which live in different working directories
    // — resolve to THE SAME path. CWD-relative was bugged: launcher
    // saved at e.g. C:\games\fm2k_inputs.ini, hook stat'd
    // C:\games\2dfm\wanwan\fm2k_inputs.ini. File never found in-game.
    return DefaultProfileBaseDir() +
#ifdef _WIN32
           std::string("\\")
#else
           std::string("/")
#endif
           + "fm2k_inputs.ini";
}

std::string GameProfilePath() {
    if (g_active_game.empty()) return DefaultProfilePath();
    return DefaultProfileBaseDir() +
#ifdef _WIN32
           std::string("\\")
#else
           std::string("/")
#endif
           + "fm2k_inputs_" + g_active_game + ".ini";
}

bool FileExists(const std::string& p) {
    if (p.empty()) return false;
    FILE* f = std::fopen(p.c_str(), "rb");
    if (!f) return false;
    std::fclose(f);
    return true;
}

// Resolve which file to read/write right now. Per-game wins if both the
// game profile is set AND the file exists on disk; otherwise default.
std::string DefaultConfigPath() {
    if (!g_active_game.empty()) {
        const std::string p = GameProfilePath();
        if (FileExists(p)) return p;
    }
    return DefaultProfilePath();
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

    // Hints — must be set BEFORE SDL_InitSubSystem(GAMEPAD).
    // HIDAPI brings the first-party DS4/DS3/Switch-Pro drivers in
    // SDL3, which handle PS4 sticks (Qanba Obsidian in PS4 mode is a
    // re-labelled DS4) and PS3 sticks correctly. RAWINPUT gives us
    // XInput-style controllers (Qanba Obsidian in PC mode shows up as
    // an XInput device). Without these, sticks fall back to platform-
    // generic joystick paths that often don't ship with a SDL gamepad
    // mapping → SDL_GetGamepads() returns nothing and the binder UI
    // shows zero devices.
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI,         "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS3,     "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS4,     "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5,     "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_SWITCH,  "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_RAWINPUT,       "1");

    // Make sure SDL gamepad subsystem is up. No-op if already initialized.
    if (!SDL_WasInit(SDL_INIT_GAMEPAD)) SDL_InitSubSystem(SDL_INIT_GAMEPAD);

    // Critical: without this the polled state behind SDL_GetGamepadButton
    // never refreshes — clicking Bind and pressing a button looked like
    // the binder was ignoring controller inputs entirely. Enabled
    // unconditionally; mirrors revolve_input_sdl3 (sdl3_gamepad_manager
    // line 128).
    SDL_SetGamepadEventsEnabled(true);

    // Built-in mapping fallbacks for sticks SDL3 doesn't ship a
    // mapping for. PS3 controllers in particular: HIDAPI driver can't
    // always identify the controller flavor (Sony first-party vs
    // clone) and falls through to a generic HID joystick — which has
    // axes/buttons but no gamepad mapping, so SDL_GetGamepads()
    // doesn't list it. Adding mappings ahead of time covers that.
    // Lifted verbatim from revolve_input_sdl3 (BBBR's input layer).
    static const char* kBuiltinMappings[] = {
        "030000004c0500006802000000010000,PS3 Controller,a:b14,b:b13,y:b12,x:b15,back:b0,guide:b16,start:b3,leftstick:b1,rightstick:b2,leftshoulder:b10,rightshoulder:b11,lefttrigger:b8,righttrigger:b9,leftx:a0,lefty:a1,rightx:a2,righty:a3,dpdown:b6,dpleft:b7,dpright:b5,dpup:b4,",
        "030000004c0500006802000000000000,PS3 Controller,a:b14,b:b13,y:b12,x:b15,back:b0,guide:b16,start:b3,leftstick:b1,rightstick:b2,leftshoulder:b10,rightshoulder:b11,lefttrigger:b8,righttrigger:b9,leftx:a0,lefty:a1,rightx:a2,righty:a3,dpdown:b6,dpleft:b7,dpright:b5,dpup:b4,",
        "030000004c0500006802000000020000,PS3 Controller,a:b14,b:b13,y:b12,x:b15,back:b0,guide:b16,start:b3,leftstick:b1,rightstick:b2,leftshoulder:b10,rightshoulder:b11,lefttrigger:b8,righttrigger:b9,leftx:a0,lefty:a1,rightx:a2,righty:a3,dpdown:b6,dpleft:b7,dpright:b5,dpup:b4,",
    };
    for (const char* m : kBuiltinMappings) SDL_AddGamepadMapping(m);

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

// Refresh the cached config path against the current per-game profile
// state. Called whenever SetGameProfile / fork / delete changes which
// file should be active.
static void RefreshActivePath() {
    g_config_path = DefaultConfigPath();
}

bool Save() {
    // Per-game profile routing rule: write to <game>.ini ONLY if that
    // file already exists on disk (user explicitly forked it via the
    // "Use override for X" checkbox). Otherwise write to the default
    // profile. Without this gate, every Save() while a game is
    // selected silently re-creates the per-game file even after the
    // user unchecked override — the checkbox would auto-re-check
    // itself on the next render after any binding tweak.
    const bool route_to_game =
        !g_active_game.empty() && FileExists(GameProfilePath());
    const std::string path = route_to_game
        ? GameProfilePath()
        : DefaultProfilePath();
    g_config_path = path;
    FILE* f = std::fopen(path.c_str(), "w");
    if (!f) return false;
    std::fprintf(f, "; FM2K input bindings\n");
    for (int p = 0; p < kPlayers; ++p) {
        std::fprintf(f, "[Player%d]\n", p);
        for (size_t i = 0; i < (size_t)Bit::COUNT; ++i) {
            WriteBinding(f, kBitNames[i], g_players[p].bits[i]);
            // Alt slot — emit only when set so legacy single-source
            // configs round-trip without picking up a noisy ".alt = NONE"
            // for every bit.
            if (g_players[p].bits_alt[i].source != Binding::Source::NONE) {
                char alt_key[64];
                std::snprintf(alt_key, sizeof(alt_key), "%s.alt", kBitNames[i]);
                WriteBinding(f, alt_key, g_players[p].bits_alt[i]);
            }
        }
        std::fprintf(f, "\n");
    }
    std::fclose(f);
    return true;
}

bool Load() {
    RefreshActivePath();  // resolves per-game vs default based on disk state
    FILE* f = std::fopen(g_config_path.c_str(), "r");
    if (!f) return false;

    // The config file exists → clear alt slots before parsing so missing
    // ".alt" keys mean "empty alt", not "XInput defaults from Init". Without
    // this, every pre-v0.2.16 config (no .alt keys ever written) silently
    // gets its alt slots populated with the auto-defaults — which OR with
    // the user's custom primary bindings and fire wrong bits when they
    // press a face button. Fresh installs (no config file) skip this branch
    // and keep the Init-time defaults; that's the intended path for new
    // users wanting "pad just works out of the box".
    for (int p = 0; p < kPlayers; ++p) {
        for (auto& b : g_players[p].bits_alt) b = Binding{};
    }

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
            // ".alt" suffix → secondary slot for the same bit.
            // Old configs without this key leave bits_alt at NONE
            // (set by ApplyDefaults' loop pre-load), which is fine.
            char alt_key[64];
            std::snprintf(alt_key, sizeof(alt_key), "%s.alt", kBitNames[i]);
            if (key == alt_key) {
                ParseBinding(val, g_players[section].bits_alt[i]);
                break;
            }
        }
    }
    std::fclose(f);
    return true;
}

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
                    SHORT v = 0;
                    switch (b.code) {
                        case SDL_GAMEPAD_AXIS_LEFTX:        v = st->Gamepad.sThumbLX; break;
                        case SDL_GAMEPAD_AXIS_LEFTY:        v = (SHORT)-st->Gamepad.sThumbLY; break;
                        case SDL_GAMEPAD_AXIS_RIGHTX:       v = st->Gamepad.sThumbRX; break;
                        case SDL_GAMEPAD_AXIS_RIGHTY:       v = (SHORT)-st->Gamepad.sThumbRY; break;
                        case SDL_GAMEPAD_AXIS_LEFT_TRIGGER: v = (SHORT)(st->Gamepad.bLeftTrigger * 128); break;
                        case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER:v = (SHORT)(st->Gamepad.bRightTrigger * 128); break;
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
    return mask & 0x7FF;
#endif
}

// Render just the binder body (no ImGui::Begin/End). Use this when
// embedding the binder inside another container — e.g. a tab page in
// the launcher's consolidated Settings window. Returns true if any
// binding changed this frame so the caller can call Save().
bool RenderBody(int player_slot) {
    if (!g_initialized) Init();
    if (player_slot < 0 || player_slot >= kPlayers) return false;

    bool changed = false;

    static double s_last_refresh = 0.0;
    double now = ImGui::GetTime();
    if (now - s_last_refresh > 1.5) { RefreshGamepadList(); s_last_refresh = now; }

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

    // ------------------------------------------------------------------
    // Per-player primary device picker.
    //
    // Without this, "P1 controller goes to player 2 too" is the default
    // experience — every gamepad-bound row stores its own gamepad_index,
    // and ApplyDefaultsP1/P2 leave gamepad bindings empty so the user
    // adds them one by one through capture, all picking up index 0.
    //
    // The dropdown below selects ONE primary device for this player slot
    // and propagates that index across all of this player's gamepad-
    // sourced bindings in one shot. Keyboard rows are untouched. Future
    // SDL3 multi-keyboard support (per the SDL_KeyboardEvent.which
    // field) will add a "primary keyboard" option to the same dropdown
    // — keyboard bindings on a player become device-scoped instead of
    // OS-global GetAsyncKeyState polling.
    // ------------------------------------------------------------------
    {
        // Discover this player's CURRENT primary gamepad index by looking
        // at the first gamepad-sourced binding across BOTH primary and
        // alt slots — alt now defaults to gamepad on a fresh init, so
        // even a brand-new "keyboard primary" config has the gamepad
        // index present in the alt array.
        int current_idx = -1;  // -1 = keyboard only
        for (size_t i = 0; i < (size_t)Bit::COUNT && current_idx < 0; ++i) {
            for (const Binding* arr : { pb.bits + 0, pb.bits_alt + 0 }) {
                const Binding& b = arr[i];
                if (b.source == Binding::Source::GAMEPAD_BUTTON ||
                    b.source == Binding::Source::GAMEPAD_AXIS) {
                    current_idx = b.gamepad_index;
                    break;
                }
            }
        }
        // If no gamepad bindings exist on this player, surface the slot's
        // intended default index (P1=0, P2=1) so the dropdown still reads
        // sensibly. The actual binding-side index updates only when the
        // user picks a row in the dropdown AND a gamepad binding exists.
        const int default_idx = (player_slot == 0) ? 0 : 1;
        if (current_idx < 0) current_idx = default_idx;
        if (current_idx >= (int)g_gamepad_ids.size()) {
            current_idx = g_gamepad_ids.empty() ? -1 : 0;
        }

        // Build the combo preview string. -1 = keyboard only.
        char preview[160];
        if (current_idx < 0) {
            std::snprintf(preview, sizeof(preview), "Keyboard only");
        } else {
            std::snprintf(preview, sizeof(preview), "[%d] %s",
                          current_idx, GamepadNameAt(current_idx));
        }

        ImGui::Spacing();
        ImGui::TextUnformatted(player_slot == 0
                                ? "Player 1 device:"
                                : "Player 2 device:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1.0f);
        const char* combo_id = (player_slot == 0) ? "##p1_dev" : "##p2_dev";
        if (ImGui::BeginCombo(combo_id, preview)) {
            // Keyboard only entry — clears gamepad bindings on this player.
            bool sel = (current_idx < 0);
            if (ImGui::Selectable("Keyboard only", sel)) {
                // Convert any gamepad-bound rows back to NONE in BOTH
                // slots so the user explicitly re-binds via capture if
                // they want gamepad input on this player. Keep keyboard
                // rows but reset their gamepad_index hint.
                auto clear_gp = [](Binding& b) {
                    if (b.source == Binding::Source::GAMEPAD_BUTTON ||
                        b.source == Binding::Source::GAMEPAD_AXIS) {
                        b = Binding{};
                    }
                    b.gamepad_index = -1;
                };
                for (auto& b : pb.bits)     clear_gp(b);
                for (auto& b : pb.bits_alt) clear_gp(b);
                changed = true;
            }
            // One row per available SDL gamepad.
            for (int i = 0; i < (int)g_gamepad_ids.size(); ++i) {
                char row[160];
                std::snprintf(row, sizeof(row), "[%d] %s",
                              i, GamepadNameAt(i));
                bool sel_i = (current_idx == i);
                if (ImGui::Selectable(row, sel_i)) {
                    // Propagate to all of this player's gamepad-sourced
                    // bindings (both slots) AND to the per-player
                    // default index used by future captures.
                    for (auto& b : pb.bits)     b.gamepad_index = i;
                    for (auto& b : pb.bits_alt) b.gamepad_index = i;
                    changed = true;
                }
                if (sel_i) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::TextDisabled(
            "Routes this player's gamepad bindings to the chosen device. "
            "Keyboard rows are unaffected. Multi-keyboard support is a "
            "follow-up — when SDL_KeyboardID lands the dropdown will "
            "include per-keyboard entries.");
    }

    // Per-game profile toggle. Shows the active game name (if a game is
    // selected in the launcher) and lets the user fork/delete a per-game
    // override without leaving the binder. With no game selected the
    // checkbox is disabled and we silently edit the default profile.
    if (!g_active_game.empty()) {
        bool override_active = HasGameProfile();
        const std::string label = "Use override for \"" + g_active_game + "\"";
        if (ImGui::Checkbox(label.c_str(), &override_active)) {
            if (override_active) {
                ForkDefaultToGameProfile();
            } else {
                DeleteGameProfile();
            }
            changed = true;
        }
        ImGui::SameLine();
        ImGui::TextDisabled(override_active ? "(per-game)" : "(default)");
    } else {
        ImGui::TextDisabled("Editing default profile (no game selected)");
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

    // Live gamepad-state debug panel. Shows per-frame what SDL is
    // reporting from each opened device. If you press a button and
    // it doesn't change here, SDL isn't seeing the input at all
    // (HIDAPI driver issue, controller in wrong mode, etc.) — vs.
    // if it DOES change here but Bind doesn't catch it, the bug is
    // in the binder's capture state machine.
    SDL_PumpEvents();
    if (ImGui::CollapsingHeader("Live gamepad state (debug)",
                                ImGuiTreeNodeFlags_DefaultOpen))
    {
        if (g_gamepad_handles.empty()) {
            ImGui::TextDisabled("(no gamepads opened)");
        }
        size_t idx = 0;
        for (auto& kv : g_gamepad_handles) {
            SDL_Gamepad* gp = kv.second;
            if (!gp) continue;
            const char* name = SDL_GetGamepadName(gp);
            ImGui::Text("[%zu] jid=%u %s",
                idx, (unsigned)kv.first, name ? name : "?");
            // Buttons — print the pressed-button names compactly.
            std::string pressed;
            for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; ++b) {
                if (SDL_GetGamepadButton(gp, (SDL_GamepadButton)b)) {
                    if (!pressed.empty()) pressed += " ";
                    pressed += "B" + std::to_string(b);
                }
            }
            ImGui::Text("  buttons: %s",
                pressed.empty() ? "(none)" : pressed.c_str());
            // Axes — show all six even when zero so you can watch
            // them move in real time.
            char axline[256] = {};
            char* p = axline;
            char* end = axline + sizeof(axline);
            for (int a = 0; a < SDL_GAMEPAD_AXIS_COUNT && p < end - 16; ++a) {
                Sint16 v = SDL_GetGamepadAxis(gp, (SDL_GamepadAxis)a);
                int n = std::snprintf(p, end - p, "A%d=%6d ", a, (int)v);
                if (n > 0) p += n;
            }
            ImGui::Text("  axes: %s", axline);
            ++idx;
        }
        ImGui::TextDisabled(
            "If buttons stay (none) when you press: SDL isn't seeing "
            "input. Wrong controller mode or HIDAPI driver issue.");

        // Capture-state diagnostics. Tells us which gate the bind
        // state machine is stuck behind when a press isn't being
        // captured.
        const bool any_held = AnyInputHeld();
        ImGui::Text("Capture state: active=%d armed=%d player=%d bit=%d",
            g_capture.active ? 1 : 0,
            g_capture.armed  ? 1 : 0,
            g_capture.player,
            g_capture.bit);
        ImGui::Text("AnyInputHeld() = %d", any_held ? 1 : 0);
        if (g_capture.active && !g_capture.armed && any_held) {
            ImGui::TextColored(ImVec4(0.95f, 0.65f, 0.20f, 1.0f),
                "Waiting for full release before sampling — release "
                "ALL keys/sticks/buttons. If a stick axis is sitting "
                "past 50%% threshold at idle (deadzone drift), arm "
                "will never trigger.");
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Refresh Pads")) {
        RefreshGamepadList();
    }

    ImGui::Separator();

    // Compute conflicts within this player. Each slot (primary + alt)
    // can independently conflict with another bit's primary or alt, so
    // we walk the cross-product per-slot. A bit's row goes red when
    // EITHER slot collides with anything.
    bool conflict_pri[(size_t)Bit::COUNT] = {};
    bool conflict_alt[(size_t)Bit::COUNT] = {};
    auto check_pair = [&](size_t i, bool i_alt, size_t j, bool j_alt) {
        const Binding& a = i_alt ? pb.bits_alt[i] : pb.bits[i];
        const Binding& b = j_alt ? pb.bits_alt[j] : pb.bits[j];
        if (BindingsConflict(a, b)) {
            (i_alt ? conflict_alt : conflict_pri)[i] = true;
            (j_alt ? conflict_alt : conflict_pri)[j] = true;
        }
    };
    for (size_t i = 0; i < (size_t)Bit::COUNT; ++i) {
        // Same-bit primary vs. alt isn't a conflict — that's the WHOLE
        // POINT (stick + dpad on the same direction). Skip i==j entirely
        // and only flag cross-bit collisions.
        for (size_t j = i + 1; j < (size_t)Bit::COUNT; ++j) {
            check_pair(i, false, j, false);
            check_pair(i, false, j, true);
            check_pair(i, true,  j, false);
            check_pair(i, true,  j, true);
        }
    }

    // Capture handling -- only act on rows owned by this player+bit.
    if (g_capture.active && g_capture.player == player_slot) {
        if (!g_capture.armed) {
            // Wait for full release before sampling so the click that
            // started the capture isn't read back.
            if (!AnyInputHeld()) {
                g_capture.armed = true;
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "InputBinder: armed for player=%d bit=%d — waiting for press",
                    g_capture.player, g_capture.bit);
            }
        } else {
            Binding cap{};
            bool cancel = false;
            if (PollCapture(cap, cancel)) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "InputBinder: captured src=%d code=%d axis_dir=%d gp=%d "
                    "for player=%d bit=%d",
                    (int)cap.source, cap.code, cap.axis_dir, cap.gamepad_index,
                    g_capture.player, g_capture.bit);
                if (!cancel) {
                    if (g_capture.alt) pb.bits_alt[g_capture.bit] = cap;
                    else               pb.bits[g_capture.bit]     = cap;
                    changed = true;
                }
                g_capture.active = false;
                g_capture.armed = false;
                g_capture.player = -1;
                g_capture.bit = -1;
                g_capture.alt = false;
            }
        }
    }

    // Bindings table — primary AND alt slot per bit. Both slots OR
    // together at sample time so a single direction can fire from
    // EITHER source. CXL-style stick+dpad: drop the dpad in primary
    // and the stick axis in alt (or vice-versa); both work in-game.
    // Typical user keeps primary = keyboard, alt = gamepad.
    if (ImGui::BeginTable("bindings", 5,
            ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg |
            ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Bit",     ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableSetupColumn("Primary", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("",        ImGuiTableColumnFlags_WidthFixed, 130.0f);
        ImGui::TableSetupColumn("Alt",     ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("",        ImGuiTableColumnFlags_WidthFixed, 130.0f);
        ImGui::TableHeadersRow();

        // Per-slot row renderer. col_label/col_btns are the table column
        // indices for the slot's text + buttons; alt_slot picks which
        // member of pb to read/write.
        auto render_slot = [&](size_t i, bool alt_slot,
                               int col_label, int col_btns) {
            const Binding& cur = alt_slot ? pb.bits_alt[i] : pb.bits[i];
            const bool conflict = (alt_slot ? conflict_alt : conflict_pri)[i];
            const bool waiting = g_capture.active &&
                                 g_capture.player == player_slot &&
                                 g_capture.bit == (int)i &&
                                 g_capture.alt == alt_slot;

            ImGui::TableSetColumnIndex(col_label);
            if (waiting) {
                ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.2f, 1.0f),
                                   "Press a key/button... (Cancel to abort)");
            } else if (conflict) {
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s",
                                   BindingLabel(cur).c_str());
            } else {
                ImGui::TextUnformatted(BindingLabel(cur).c_str());
            }

            ImGui::TableSetColumnIndex(col_btns);
            ImGui::PushID(alt_slot ? "a" : "p");
            if (waiting) {
                if (ImGui::Button("Cancel", ImVec2(60, 0))) {
                    g_capture = CaptureCtx{};
                }
            } else {
                if (ImGui::Button("Bind", ImVec2(60, 0))) {
                    g_capture.active = true;
                    g_capture.armed = false;
                    g_capture.player = player_slot;
                    g_capture.bit = (int)i;
                    g_capture.alt = alt_slot;
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Clear", ImVec2(60, 0))) {
                if (alt_slot) pb.bits_alt[i] = Binding{};
                else          pb.bits[i]     = Binding{};
                changed = true;
            }
            ImGui::PopID();
        };

        for (size_t i = 0; i < (size_t)Bit::COUNT; ++i) {
            ImGui::TableNextRow();
            ImGui::PushID((int)i);

            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(kBitNames[i]);

            render_slot(i, /*alt=*/false, /*col_label=*/1, /*col_btns=*/2);
            render_slot(i, /*alt=*/true,  /*col_label=*/3, /*col_btns=*/4);

            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    ImGui::Separator();
    ImGui::Text("Sampled mask: 0x%03X", (unsigned)Sample(player_slot));

    return changed;
}

bool RenderWindow(int player_slot, bool* p_open) {
    if (player_slot < 0 || player_slot >= kPlayers) return false;
    char title[64];
    std::snprintf(title, sizeof(title),
                  "FM2K Input Binder - Player %d", player_slot + 1);
    ImGui::SetNextWindowSize(ImVec2(420, 360), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(title, p_open)) { ImGui::End(); return false; }
    bool changed = RenderBody(player_slot);
    ImGui::End();
    return changed;
}

// ---------------------------------------------------------------------------
// Per-game profile management
// ---------------------------------------------------------------------------

void SetGameProfile(const char* exe_basename) {
    const std::string sanitized = SanitizeProfileName(exe_basename);
    if (g_active_game == sanitized) return;
    g_active_game = sanitized;
    RefreshActivePath();
}

bool HasGameProfile() {
    if (g_active_game.empty()) return false;
    return FileExists(GameProfilePath());
}

bool ForkDefaultToGameProfile() {
    if (g_active_game.empty()) return false;
    const std::string dst = GameProfilePath();
    if (FileExists(dst)) {
        // Already exists — caller can choose to overwrite via Save().
        // Treat fork-of-already-existing as a no-op success so the UI
        // checkbox toggle works idempotently.
        RefreshActivePath();
        return true;
    }
    const std::string src = DefaultProfilePath();
    FILE* in = std::fopen(src.c_str(), "rb");
    FILE* out = std::fopen(dst.c_str(), "wb");
    if (!out) {
        if (in) std::fclose(in);
        return false;
    }
    if (in) {
        char buf[4096];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), in)) > 0) {
            std::fwrite(buf, 1, n, out);
        }
        std::fclose(in);
    } else {
        // No default file yet — write the current in-memory bindings as the
        // seed of the per-game profile. Otherwise the per-game file would
        // be empty and Load() would silently leave defaults in place.
        std::fclose(out);
        const std::string saved_active = g_active_game;
        g_config_path = dst;  // route Save() to the per-game file
        const bool ok = Save();
        g_active_game = saved_active;
        RefreshActivePath();
        return ok;
    }
    std::fclose(out);
    RefreshActivePath();
    return true;
}

bool DeleteGameProfile() {
    if (g_active_game.empty()) return false;
    const std::string p = GameProfilePath();
    if (!FileExists(p)) {
        RefreshActivePath();
        return false;
    }
    std::remove(p.c_str());
    RefreshActivePath();
    Load();  // pull bindings back from default
    return true;
}

const char* CurrentConfigPath() {
    if (g_config_path.empty()) g_config_path = DefaultConfigPath();
    return g_config_path.c_str();
}

}  // namespace FM2KInputBinder
