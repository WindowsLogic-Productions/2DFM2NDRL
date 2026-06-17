#pragma once
// imgui_overlay.cpp shared state, externed so the ImGui debug-panel render TU
// (imgui_overlay_render.cpp) can share it with the D3D9/lifecycle core. Pure
// linkage move -- definitions live in imgui_overlay.cpp. The render TU is the
// Slint-replaceable piece: it draws the debug panel; core owns the present hook
// + device lifecycle + window subclass + hotkeys.
#include <d3d9.h>          // D3DVIEWPORT9
#include "savestate.h"     // StateSnapshot

// ---- state shared between core (imgui_overlay.cpp) and the render panel ----
extern bool         g_overlay_visible;       // F9 debug-window toggle
extern D3DVIEWPORT9 g_game_viewport;         // computed game rect (cnc-ddraw aware)
extern bool         g_show_game_rect_debug;  // panel checkbox -> rect overlay

// F10 savestate-roundtrip test result (core CheckOverlayHotkey writes; the
// panel reads + displays). Kept together so the panel and the hotkey handler
// agree on the last test's outcome.
extern bool         g_last_test_ran;
extern bool         g_last_test_passed;
extern char         g_last_test_diff[1024];
extern StateSnapshot g_last_snapshot_before;
extern StateSnapshot g_last_snapshot_after;

// ImGui debug panel (imgui_overlay_render.cpp), called from Hook_EndScene.
void RenderDebugOverlay();
