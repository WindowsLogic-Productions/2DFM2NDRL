#!/usr/bin/env python3
"""One-shot launcher reorg: move the flat root launcher files into a launcher/
subtree mirroring FM2KHook/src, then fix CMakeLists.txt (source paths +
global include dirs). Behavior-preserving: include resolution is by basename via
the global include_directories(), so NO #include statements change.

Strategy verified safe: hook includes only version_local.h from root (stays);
FM2K_Integration.h's "hook deps" are comments; no basename collisions.
"""
import pathlib, subprocess, sys, re

ROOT = pathlib.Path(".")

# basename -> launcher subdir. Headers + cpp alike (headers resolve via include path).
MAP = {
  "core": [
    "FM2K_RollbackClient.cpp", "launcher_callbacks.cpp", "launcher_init.cpp",
    "launcher_frame.cpp", "launcher_cli.cpp", "launcher_cli.h",
    "process_manager.cpp", "session_control.cpp",
    "FM2K_GameInstance.cpp", "FM2K_GameInstance_ipc.cpp", "FM2K_GameInstance.h",
    "FM2K_Integration.h", "FM2K_Launcher_decl.h", "FM2K_LauncherUI_decl.h",
    "FM95_Integration.h", "FM2K_SharedMemory.h",
  ],
  "ui": [
    "FM2K_LauncherUI.cpp",
    "launcher_ui_settings_io.cpp", "launcher_ui_debug.cpp", "launcher_ui_display.cpp",
    "launcher_ui_menubar.cpp", "launcher_ui_settings.cpp", "launcher_ui_gameselect.cpp",
    "launcher_ui_netcfg.cpp", "launcher_ui_input.cpp", "launcher_ui_notify.cpp",
    "launcher_ui_hub.cpp", "launcher_ui_hub_match.cpp", "launcher_ui_hub_events.cpp",
    "launcher_ui_hub_panel.cpp",
    "launcher_ui_internal.h", "launcher_ui_hubstate.h", "launcher_ui_hub_internal.h",
  ],
  "hub": [
    "FM2K_HubClient.cpp", "FM2K_HubClient_json.cpp", "FM2K_HubClient_outbound.cpp",
    "FM2K_HubClient_transport.cpp", "FM2K_HubClient_dispatch.cpp",
    "FM2K_HubClient.h", "FM2K_HubClient_internal.h",
  ],
  "session": [
    "OnlineSession.cpp", "OnlineSession.h", "LocalSession.cpp", "LocalSession.h",
    "LocalNetworkAdapter.cpp", "LocalNetworkAdapter.h", "ISession.h",
  ],
  "session/examples": [
    "LocalSessionExample.cpp", "OnlineSession_gekkonet_sdl2_example.cpp",
  ],
  "discovery": [
    "game_discovery.cpp", "game_discovery_cache.cpp",
    "game_discovery.h", "game_discovery_internal.h",
  ],
  "game": [
    "FM2K_GameIni.cpp", "FM2K_GameIni.h", "FM2K_KgtParser.cpp", "FM2K_KgtParser.h",
    "FM2K_Locale.cpp", "FM2K_Locale.h", "FM2K_Utf8Path.h",
  ],
  "net": [
    "FM2K_PortMapper.cpp", "FM2K_PortMapper.h", "FM2K_UploadQueue.cpp", "FM2K_UploadQueue.h",
    "FM2K_DiscordAuth.cpp", "FM2K_DiscordAuth.h", "FM2K_Updater.cpp", "FM2K_Updater.h",
  ],
  "render": [
    "FM2K_CncDDraw.cpp", "FM2K_CncDDraw.h", "FM2K_DDrawRedirect.cpp", "FM2K_DDrawRedirect.h",
  ],
}

# basename -> new repo-relative path (for CMake source-list rewrite)
new_path = {}
for sub, files in MAP.items():
    for f in files:
        new_path[f] = f"launcher/{sub}/{f}"

# Pre-flight: every listed file must exist at root.
missing = [f for files in MAP.values() for f in files if not (ROOT / f).exists()]
if missing:
    sys.exit(f"ABORT: missing files at root: {missing}")

# 1) git mv into the subtree.
for sub, files in MAP.items():
    (ROOT / "launcher" / sub).mkdir(parents=True, exist_ok=True)
    for f in files:
        subprocess.run(["git", "mv", f, f"launcher/{sub}/{f}"], check=True)

# 2) Rewrite CMakeLists.txt.
cml = pathlib.Path("CMakeLists.txt")
lines = cml.read_text().splitlines(keepends=True)
out = []
in_inc = False
for ln in lines:
    # 2a) add launcher subdirs to the global include_directories() block.
    if ln.strip().startswith("include_directories("):
        in_inc = True
        out.append(ln)
        continue
    if in_inc and ln.strip() == "${CMAKE_CURRENT_SOURCE_DIR}":
        out.append(ln)
        for sub in ("core", "ui", "hub", "session", "discovery", "game", "net", "render"):
            out.append(f"    ${{CMAKE_CURRENT_SOURCE_DIR}}/launcher/{sub}\n")
        continue
    if in_inc and ln.strip() == ")":
        in_inc = False
        out.append(ln)
        continue
    # 2b) rewrite launcher source-list entries (bare basename -> launcher/<sub>/<basename>).
    stripped = ln.strip()
    tok = stripped.split("#", 1)[0].strip()  # path token before any comment
    if tok in new_path:
        indent = ln[:len(ln) - len(ln.lstrip())]
        comment = ""
        if "#" in ln:
            comment = "  #" + ln.split("#", 1)[1].rstrip("\n")
        out.append(f"{indent}{new_path[tok]}{comment}\n")
        continue
    out.append(ln)
cml.write_text("".join(out))

print("reorg done. moved:")
for sub, files in MAP.items():
    print(f"  launcher/{sub}/  ({len(files)} files)")
print("CMake: source paths rewritten + 8 launcher include dirs added.")
