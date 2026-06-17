#!/usr/bin/env python3
"""One-shot splitter for FM2KHook/src/ui/imgui_overlay.cpp (1080 lines).

Pure behaviour-preserving move: extracts the ImGui debug PANEL
(RenderDebugOverlay + its DebugAddrs table) into imgui_overlay_render.cpp -- the
Slint-replaceable UI -- leaving the D3D9 present hook + device lifecycle +
window subclass + hotkeys in imgui_overlay.cpp (core). Shared state externed via
imgui_overlay_internal.h (already written).

Ranges (1-based inclusive), verified against the read:
  DebugAddrs           106-126
  RenderDebugOverlay   248-515
The 8 statics RenderDebugOverlay reads are de-static'd in core.
"""
import pathlib

SRC = pathlib.Path("FM2KHook/src/ui/imgui_overlay.cpp")
L = SRC.read_text().splitlines(keepends=True)
if len(L) < 1000 or not any("void RenderDebugOverlay()" in ln for ln in L):
    raise SystemExit("imgui_overlay.cpp is not the original monolith "
                     "(already split?). Restore from git before re-running.")
def R(a, b):
    return "".join(L[a-1:b])

# ---- render TU: imgui_overlay_render.cpp ----
render = []
render.append('// imgui_overlay_render.cpp -- ImGui DEBUG PANEL (RenderDebugOverlay).\n')
render.append('// Split from imgui_overlay.cpp; THIS is the Slint-replaceable UI. Reads\n')
render.append('// shared overlay state via imgui_overlay_internal.h; called once per frame\n')
render.append('// from Hook_EndScene (core). Engine-agnostic.\n')
render.append('#include "imgui_overlay.h"\n')
render.append('#include "imgui_overlay_internal.h"\n')
render.append('#include "fc_hud.h"\n')
render.append('#include "netplay.h"          // Netplay_IsConnected/IsActive/GetInput/GetCSSInput\n')
render.append('#include "globals.h"          // g_frame_counter, g_player_index\n')
render.append('#include "savestate.h"        // SaveState_* roundtrip test\n')
render.append('#include "../hooks/per_game_patches.h"\n')
render.append('#include <imgui.h>\n')
render.append('#include <cstdio>\n')
render.append('#include <cstdlib>\n')
render.append('#include <cstring>\n\n')
render.append(R(106, 126))   # DebugAddrs table
render.append("\n")
render.append(R(248, 515))   # RenderDebugOverlay
pathlib.Path("FM2KHook/src/ui/imgui_overlay_render.cpp").write_text("".join(render))

# ---- core: imgui_overlay.cpp (original minus the two moved ranges) ----
core = []
core += L[0:105]             # 1-105 (includes + state + fwd decls), de-static below
# 106-126 DebugAddrs -> moved
core += L[126:247]           # 127-247 (LoadCncDdraw + ComputeCncDdraw, stay)
# 248-515 RenderDebugOverlay -> moved
core += L[515:1080]          # 516-end
core_txt = "".join(core)

# Insert the internal-header include right after imgui_overlay.h.
core_txt = core_txt.replace(
    '#include "imgui_overlay.h"\n',
    '#include "imgui_overlay.h"\n#include "imgui_overlay_internal.h"\n', 1)

# De-static the 8 statics the render panel now shares (external linkage).
for decl in (
    "static bool g_overlay_visible = false;",
    "static D3DVIEWPORT9 g_game_viewport = { 0, 0, 0, 0, 0.0f, 1.0f };",
    "static bool g_show_game_rect_debug = false;",
    "static bool g_last_test_ran = false;",
    "static bool g_last_test_passed = false;",
    'static char g_last_test_diff[1024] = "";',
    "static StateSnapshot g_last_snapshot_before;",
    "static StateSnapshot g_last_snapshot_after;",
):
    assert decl in core_txt, f"missing decl to de-static: {decl}"
    core_txt = core_txt.replace(decl, decl[len("static "):], 1)

SRC.write_text(core_txt)

print("split done:")
for f in ("imgui_overlay.cpp", "imgui_overlay_render.cpp"):
    p = pathlib.Path("FM2KHook/src/ui") / f
    print(f"  {f:28s} {sum(1 for _ in p.open())} lines")
