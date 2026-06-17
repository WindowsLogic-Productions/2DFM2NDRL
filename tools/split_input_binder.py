#!/usr/bin/env python3
"""One-shot 5-way splitter for FM2KHook/src/ui/input_binder.cpp (1592 lines).

Pure behaviour-preserving move into:
  input_binder.cpp           core: shared state defs + defaults + lifecycle
  input_binder_gamepads.cpp  RefreshGamepadList + GamepadNameAt
  input_binder_profiles.cpp  paths + INI persistence + per-game profiles
  input_binder_sample.cpp    SDL3 + Win32/XInput engine-facing sampling
  input_binder_ui.cpp        ImGui render + capture FSM (the Slint-replaceable TU)

Shared state externed via input_binder_internal.h (already written).
The anon-ns state (lines 31..667) is promoted to external in core; TU-local
helpers stay wrapped in per-file anonymous namespaces. Line ranges are 1-based
inclusive, verified against the read of the original.
"""
import pathlib

SRC = pathlib.Path("FM2KHook/src/ui/input_binder.cpp")
L = SRC.read_text().splitlines(keepends=True)
# Idempotency guard: this splitter READS then OVERWRITES SRC, so re-running it
# on an already-split core would slice garbage. Bail unless SRC is the original
# monolith (the ui RenderWindow body only exists in the unsplit file).
if len(L) < 1500 or not any("bool RenderWindow(int player_slot" in ln for ln in L):
    raise SystemExit("input_binder.cpp is not the original monolith "
                     "(already split?). Restore it from git before re-running.")
def R(a, b):  # 1-based inclusive
    return "".join(L[a-1:b])

NS_OPEN  = "namespace FM2KInputBinder {\n"
NS_CLOSE = "}  // namespace FM2KInputBinder\n"
ANON_OPEN  = "namespace {\n"
ANON_CLOSE = "}  // anonymous namespace\n"

# ============================================================================
# core -- input_binder.cpp
# ============================================================================
core = []
core.append('// input_binder.cpp -- CORE: shared state + defaults + lifecycle.\n')
core.append('// Split from the original monolith; the binding store + gamepad handle\n')
core.append('// tables + INI-key table live here at external linkage (see\n')
core.append('// input_binder_internal.h). ENGINE-AGNOSTIC, render-agnostic.\n')
core.append('#include "input_binder.h"\n')
core.append('#include "input_binder_internal.h"\n')
core.append('#include <SDL3/SDL.h>\n')
core.append('#include <cstddef>\n')
core.append('#include <string>\n\n')
core.append(NS_OPEN)
core.append("\n// ---- shared state (external linkage; declared in input_binder_internal.h) ----\n")
core.append("PlayerBindings g_players[kPlayers];\n")
core.append("std::vector<SDL_JoystickID> g_gamepad_ids;\n")
core.append("std::unordered_map<SDL_JoystickID, SDL_Gamepad*> g_gamepad_handles;\n")
core.append("std::unordered_set<SDL_JoystickID> g_non_gamepad_ids;\n")
core.append("std::string g_config_path;\n")
core.append("bool g_initialized = false;\n\n")
core.append(R(348, 362))          # Labels banner + kBitNames[] definition (external)
core.append("\n")
# Defaults at EXTERNAL linkage (the ui calls ApplyDefaults/Fill* for its reset +
# per-pad buttons). Strip `static` off the two Fill* helpers so the external
# declarations in input_binder_internal.h match.
defaults = R(68, 198)
defaults = defaults.replace("static void FillAltAsStickDirections",
                            "void FillAltAsStickDirections")
defaults = defaults.replace("static void FillPrimaryAsGamepad",
                            "void FillPrimaryAsGamepad")
core.append(defaults)
core.append("\n")
core.append(R(669, 742))          # Public API banner + Bindings/Init/Shutdown/RefreshGamepads
core.append(NS_CLOSE)
SRC.write_text("".join(core))

# ============================================================================
# gamepads -- input_binder_gamepads.cpp
# ============================================================================
gp = []
gp.append('// input_binder_gamepads.cpp -- SDL3 gamepad discovery/lifecycle.\n')
gp.append('// RefreshGamepadList + GamepadNameAt, promoted to external linkage so the\n')
gp.append('// core lifecycle + the ui device dropdown can both call them.\n')
gp.append('#include "input_binder.h"\n')
gp.append('#include "input_binder_internal.h"\n')
gp.append('#include <SDL3/SDL.h>\n')
gp.append('#include <algorithm>\n')
gp.append('#include <vector>\n\n')
gp.append(NS_OPEN)
gp.append(R(200, 346))            # Gamepads banner + RefreshGamepadList + GamepadNameAt
gp.append(NS_CLOSE)
pathlib.Path("FM2KHook/src/ui/input_binder_gamepads.cpp").write_text("".join(gp))

# ============================================================================
# profiles -- input_binder_profiles.cpp
# ============================================================================
pr = []
pr.append('// input_binder_profiles.cpp -- INI persistence + per-game profile routing.\n')
pr.append('// Path helpers + Save/Load + SetGameProfile/Fork/Delete. DefaultConfigPath\n')
pr.append('// is external (core Init calls it); the rest are TU-local.\n')
pr.append('#include "input_binder.h"\n')
pr.append('#include "input_binder_internal.h"\n')
pr.append('#include <algorithm>\n')
pr.append('#include <cctype>\n')
pr.append('#include <cstdio>\n')
pr.append('#include <cstdlib>\n')
pr.append('#include <cstring>\n')
pr.append('#include <string>\n')
pr.append('#ifdef _WIN32\n#  include <windows.h>\n#endif\n\n')
pr.append(NS_OPEN)
pr.append('std::string g_active_game = "";  // external -- ui reads it (override checkbox)\n\n')
pr.append(ANON_OPEN)
pr.append(R(513, 598))            # DefaultProfileBaseDir..FileExists (path helpers)
pr.append(R(610, 665))            # Trim + WriteBinding + ParseBinding
pr.append(ANON_CLOSE)
pr.append("\n")
pr.append(R(602, 608))            # DefaultConfigPath (external)
pr.append("\n")
pr.append("// Resolve the active config path against per-game profile + disk state.\n")
pr.append("static void RefreshActivePath() {\n    g_config_path = DefaultConfigPath();\n}\n\n")
pr.append(R(751, 844))            # Save + Load
pr.append("\n")
pr.append(R(1517, 1591))          # Profile-mgmt banner + SetGameProfile..CurrentConfigPath
pr.append(NS_CLOSE)
pathlib.Path("FM2KHook/src/ui/input_binder_profiles.cpp").write_text("".join(pr))

# ============================================================================
# sample -- input_binder_sample.cpp
# ============================================================================
sm = []
sm.append('// input_binder_sample.cpp -- engine-facing input read (NOT ImGui).\n')
sm.append('// Sample() (SDL3, launcher) + Sample_Win32() (GetAsyncKeyState + XInput,\n')
sm.append('// hook DLL). Static helpers keep internal linkage.\n')
sm.append('#include "input_binder.h"\n')
sm.append('#include "input_binder_internal.h"\n')
sm.append('#include <SDL3/SDL.h>\n')
sm.append('#include <cstring>\n')
sm.append('#ifdef _WIN32\n')
sm.append('#  ifndef WIN32_LEAN_AND_MEAN\n#    define WIN32_LEAN_AND_MEAN\n#  endif\n')
sm.append('#  include <windows.h>\n#  include <xinput.h>\n#endif\n\n')
sm.append(NS_OPEN)
sm.append(R(846, 1098))           # SampleOne_SDL + Sample + scancode/xinput + Sample_Win32
sm.append(NS_CLOSE)
pathlib.Path("FM2KHook/src/ui/input_binder_sample.cpp").write_text("".join(sm))

# ============================================================================
# ui -- input_binder_ui.cpp  (the Slint-replaceable render TU)
# ============================================================================
ui = []
ui.append('// input_binder_ui.cpp -- ImGui render layer + capture state machine.\n')
ui.append('// THE ONLY TU a future Slint rewrite replaces: RenderBody/RenderWindow +\n')
ui.append('// the bind-capture FSM (AnyInputHeld/PollCapture) + label/conflict helpers.\n')
ui.append('// All model/state access goes through input_binder_internal.h.\n')
ui.append('#include "input_binder.h"\n')
ui.append('#include "input_binder_internal.h"\n')
ui.append('#include <SDL3/SDL.h>\n')
ui.append('#include <imgui.h>\n')
ui.append('#include <cstdio>\n')
ui.append('#include <cstring>\n')
ui.append('#include <string>\n\n')
ui.append(NS_OPEN)
ui.append(ANON_OPEN)
ui.append(R(39, 47))              # CaptureCtx + g_capture (ui-local)
ui.append("\n")
ui.append(R(363, 398))            # BindingLabel + BindingsConflict
ui.append(R(400, 502))            # Capture banner + AnyInputHeld + PollCapture
ui.append(ANON_CLOSE)
ui.append("\n")
ui.append(R(1104, 1515))          # RenderBody + RenderWindow
ui.append(NS_CLOSE)
pathlib.Path("FM2KHook/src/ui/input_binder_ui.cpp").write_text("".join(ui))

print("split done:")
for f in ("input_binder.cpp", "input_binder_gamepads.cpp",
          "input_binder_profiles.cpp", "input_binder_sample.cpp",
          "input_binder_ui.cpp"):
    p = pathlib.Path("FM2KHook/src/ui") / f
    print(f"  {f:30s} {sum(1 for _ in p.open())} lines")
