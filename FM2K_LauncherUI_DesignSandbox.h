// FM2K_LauncherUI_DesignSandbox.h — opt-in, throwaway test bed for porting
// the claude.ai/design FM2K rollback hub mockups (NERV vocabulary) into
// real ImGui + ImAnim. Lives outside the main LauncherUI class so it can
// be deleted in one shot when the port is settled. Not part of the
// shipping launcher UX yet — gated behind View → Design Sandbox.
#pragma once

namespace fm2k::sandbox {

// Adds the NERV-direction design fonts (VT323 / Silkscreen / DotGothic16
// / Press Start 2P) to the ImGui atlas. MUST be called BEFORE the SDL
// renderer backend is initialized (i.e. before ImGui_ImplSDLRenderer3_Init)
// — once a frame renders, the atlas is locked. Path-searches a few
// candidate locations, silently no-ops on missing fonts (sandbox falls
// back to whatever's available).
void LoadDesignFonts();

// Renders the View-menu toggle entry. Call inside the View submenu.
void RenderViewMenuItems();

// Renders any open sandbox panels. Call once per frame from the
// launcher's main render path, AFTER the dockspace Begin/End so the
// sandbox panels float above the docked layout.
void RenderPanels();

}  // namespace fm2k::sandbox
