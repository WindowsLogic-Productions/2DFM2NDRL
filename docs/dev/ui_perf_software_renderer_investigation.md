# launcher UI perf investigation (PARKED 2026-06-17)

Status: paused mid-diagnosis. The fix shipped in v0.2.77-bleeding but we have NOT
confirmed it addresses the actual reporter, because we can't reproduce yet.

## symptom

A user on an Intel Pentium Gold 8505 reported the launcher UI ran terribly and
tanked the actual game's framerate after the game launched. A prior idle-throttle
(7061d21, 60->15fps) didn't help. The dev's own ThinkPad i5 M520 (weak 2010 Intel
HD) does NOT reproduce -- it was always fine.

## what shipped in 0.2.77-bleeding

Event-driven repaint in SDL_AppIterate (FM2K_RollbackClient.cpp). Instead of
rebuilding + presenting the whole ImGui UI every frame and merely lowering the
fps when idle, idle/backgrounded frames now SKIP the entire frame (no NewFrame/
Render/present) and leave the last image on the backbuffer. Tiers:
ACTIVE (vsync-paced) / IDLE_VISIBLE (park, 250ms safety net) / UNFOCUSED_VISIBLE /
BACKGROUND_GAME (game has foreground -> stop painting, 2Hz safety net, IPC +
spec-relay keep ticking ~30Hz) / MINIMIZED. Also fixed the g_last_input_activity_ns=0
boot-into-idle bug, added per-tier transition logging, and an opt-in
FM2K_MINIMIZE_ON_LAUNCH=1 (default off).

## leading hypothesis (UNCONFIRMED)

The deciding variable is NOT raw CPU weakness, it's which SDL renderer the
launcher picked. On an accelerated backend (direct3d11/9) the GPU eats the ImGui
draw list every frame regardless of CPU, so even the old full-redraw path is
cheap -- which is why the M520 never showed the problem. The reporter most likely
fell back to SDL's SOFTWARE renderer (blacklisted/broken D3D driver, RDP,
headless, etc.), which rasterizes the whole UI on the CPU every frame: that pegs a
weak CPU and starves the game. Our render-skip fix should help that case a lot
(idle parks, backgrounded stops), but we have not proven it end to end.

renderer selection + logging: FM2K_RollbackClient.cpp
- SDL_CreateRenderer(window_, nullptr) ~line 2691 (nullptr name -> honors the
  SDL_RENDER_DRIVER env hint; otherwise picks best available)
- vsync probe + vsync_available_ flag ~2704-2719 (drives the soft fps cap)
- startup log "Renderer: '<name>', vsync=<on/off>" ~2720

## repro recipe (next time)

Force the worst case on any machine. From cmd in the launcher's install dir:

    set FM2K_DEV_MODE=1            # keeps the console visible so logs show
    set SDL_RENDER_DRIVER=software # force CPU rasterization of the whole UI
    FM2K_RollbackLauncher.exe

Watch the console: "Renderer: 'software', vsync=off (software cap)" plus the
"UI tier -> ..." transitions. Sit idle on a panel (watch CPU), then alt-tab a
running game to the foreground (watch CPU + game fps). A/B the OLD 0.2.76 the same
way: it should peg under software where 0.2.77 parks. (FM2K_DEV_MODE=1 alone, with
the normal renderer, is how you read your actual "Renderer:" line -- it's almost
certainly direct3d* with vsync on, which is why no repro.)

## open / next steps

1. Get the reporter's actual "Renderer:" line. Have them run with FM2K_DEV_MODE=1
   and screenshot the console, OR pull their uploaded launcher log. If it says
   "software" -> hypothesis confirmed, fix is aimed right.
2. If it says direct3d* and it's STILL slow -> different cause. Look at: ImGui
   draw-list size (huge tables/lists rendered fully), the present/blit path on
   that specific iGPU, or vsync double-buffering stalls.
3. Consider a fallback/override: if software renderer is the cause, we could
   detect it at init and (a) warn the user, and/or (b) force an even harder idle
   floor under software. Possibly let the user pin a renderer via a setting.
4. Confirm the BACKGROUND_GAME render-skip actually recovers game fps under
   software (the headline regression).

## related code

- FM2K_RollbackClient.cpp: SDL_AppIterate tier machine, SDL_AppEvent (g_ui_dirty),
  SDL_AppInit (activity-clock seed), Render() (NewFrame moved in), SetState()
  (FM2K_MINIMIZE_ON_LAUNCH).
- FM2K_LauncherUI.cpp: LauncherUI::WantsContinuousRedraw() (scanning spinner +
  input-binder windows keep painting while idle).
