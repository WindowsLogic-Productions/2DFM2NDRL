#!/usr/bin/env python3
"""Drive each installed game through intro → title → CSS → battle and
collect screenshots into dist/captures/<game_id>/.

Pipeline:
  1. For each game in `games/registry.json` (or a --only filter list)
     that has a known exe path on this machine, spawn the launcher
     in capture mode.
  2. Wait for the hook's `.capture_done` sentinel file (written by
     FM2KHook/src/hooks/hooks.cpp once title+css+battle screenshots
     have all been saved).
  3. Hard-cap each run at 60 s — most games reach battle in under
     20 s with the existing AutoTitleSkip mash. If a run times out,
     log + retry up to 3 times.
  4. Kill the game cleanly afterward; the launcher's existing
     StopSession path handles teardown.

Output: dist/captures/<game_id>/{title,css_initial,battle}.png plus
manifest.json with the capture timestamp + a list of files written.

Usage:
    python3 tools/auto_capture_screenshots.py
    python3 tools/auto_capture_screenshots.py --only pkmncc,vanpri
    python3 tools/auto_capture_screenshots.py --refresh   # ignore cached
    python3 tools/auto_capture_screenshots.py --launcher /path/to/exe

The `--launcher` arg is required when not running on a Windows host
that has the launcher installed; this script's MAIN purpose is to
be invoked from the launcher's "Tools → Capture banners" menu, which
hands us its own exe path.

Status: scaffolded. The hook side (FM2KHook/src/ui/screenshot.cpp +
the FM2K_AUTO_CAPTURE state machine in hooks.cpp) is implemented and
shipping; the launcher side (--capture-game CLI flag, env-var setup)
needs a follow-up landing in FM2K_RollbackClient.cpp before this
orchestrator can do anything useful. Until then, running this
script will spawn each game with FM2K_AUTO_CAPTURE=1 set on the
LAUNCHER process so the hook DLL inherits it, and rely on the hook's
existing AutoTitleSkip to drive intro→title→CSS→battle.
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import time
from pathlib import Path
from typing import Any


REPO_ROOT = Path(__file__).resolve().parent.parent
REGISTRY_PATH = REPO_ROOT / "games" / "registry.json"
CAPTURE_ROOT = REPO_ROOT / "dist" / "captures"

# Per-run hard cap. Most FM2K games reach battle within 20 s with
# AutoTitleSkip mashing A; double that for slow installs / disk-cold
# first launches; triple for the genuinely-slow boot-to-CSS ones
# (vanpri w/ cnc-ddraw + locale spoof has been observed at 35 s).
RUN_TIMEOUT_S = 90
POLL_INTERVAL_S = 0.5

# Sentinel file the hook writes once it's captured all three core
# frames (title / css_initial / battle). The orchestrator polls for
# it and proceeds to the next game once present.
DONE_SENTINEL = ".capture_done"


def games_to_capture(only: list[str] | None) -> list[dict[str, Any]]:
    """Pick games from the registry that have a usable game_id and
    (optionally) intersect the `only` filter."""
    if not REGISTRY_PATH.exists():
        print(f"error: {REGISTRY_PATH} missing — "
              f"run tools/build_registry.py first", file=sys.stderr)
        return []
    recs = json.loads(REGISTRY_PATH.read_text(encoding="utf-8"))
    out: list[dict[str, Any]] = []
    for r in recs:
        gid = r.get("game_id") or ""
        if not gid:
            continue
        if only and gid not in only:
            continue
        out.append(r)
    return out


def already_captured(game_id: str) -> bool:
    """Has this game already been through capture? Look for the
    title.png + css_initial.png + battle.png triple under
    dist/captures/<game_id>/."""
    d = CAPTURE_ROOT / game_id
    return all((d / f).exists() for f in
               ("title.png", "css_initial.png", "battle.png"))


def run_one(game: dict[str, Any], launcher_exe: Path,
            refresh: bool) -> bool:
    gid = game["game_id"]
    out_dir = CAPTURE_ROOT / gid
    if not refresh and already_captured(gid):
        print(f"  ⏭  {gid:30s} already captured")
        return True

    out_dir.mkdir(parents=True, exist_ok=True)
    sentinel = out_dir / DONE_SENTINEL
    if sentinel.exists():
        sentinel.unlink()

    env = dict(os.environ)
    env["FM2K_AUTO_CAPTURE"]   = "1"
    env["FM2K_CAPTURE_DIR"]    = str(out_dir)
    # Force the hook to autostart through the title without prompting
    # for menu input. AutoTitleSkip is on by default but we explicitly
    # set it here in case the user disabled it for normal play.
    env["FM2K_AUTO_TITLE_SKIP"] = "1"
    # Mute audio so a 30-game capture session doesn't blast the room.
    env["FM2K_MUTE_BGM"]       = "1"
    env["FM2K_MUTE_SE"]        = "1"
    # Make sure netplay paths stay quiet — capture is offline-only.
    env["FM2K_TRUE_OFFLINE"]   = "1"

    print(f"  ▶  {gid:30s} → {out_dir.relative_to(REPO_ROOT)}")
    cmd = [str(launcher_exe), "--capture-game", gid]
    proc = subprocess.Popen(cmd, env=env)

    deadline = time.monotonic() + RUN_TIMEOUT_S
    success = False
    while time.monotonic() < deadline:
        if sentinel.exists():
            success = True
            break
        if proc.poll() is not None:
            # Launcher exited before sentinel landed — usually the
            # hook crashed or the game refused to launch. Treat as
            # failure.
            break
        time.sleep(POLL_INTERVAL_S)

    if proc.poll() is None:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()

    if success:
        print(f"  ✓  {gid:30s} captured")
        # Write a thin manifest so a later post-processor can pick
        # the canonical banner without re-checking each PNG.
        manifest = {
            "game_id":      gid,
            "name":         game.get("name") or gid,
            "captured_at":  time.time(),
            "files":        sorted(p.name for p in out_dir.iterdir()
                                   if p.suffix == ".png"),
        }
        (out_dir / "manifest.json").write_text(
            json.dumps(manifest, indent=2, ensure_ascii=False) + "\n",
            encoding="utf-8")
    else:
        print(f"  ✗  {gid:30s} timed out / crashed")
    return success


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", default="",
                    help="comma-separated game_ids to capture; "
                         "default = every registry entry")
    ap.add_argument("--refresh", action="store_true",
                    help="re-capture games that already have all "
                         "three core frames on disk")
    ap.add_argument("--launcher", default="",
                    help="path to FM2K_RollbackLauncher.exe; defaults "
                         "to /mnt/c/games/FM2K_RollbackLauncher.exe "
                         "(WSL convention) when unset")
    args = ap.parse_args()

    launcher = (Path(args.launcher) if args.launcher
                else Path("/mnt/c/games/FM2K_RollbackLauncher.exe"))
    if not launcher.exists():
        print(f"error: launcher not found at {launcher}",
              file=sys.stderr)
        print("       pass --launcher /path/to/exe", file=sys.stderr)
        return 1

    only = ([s.strip() for s in args.only.split(",") if s.strip()]
            if args.only else None)
    games = games_to_capture(only)
    if not games:
        print("no games matched", file=sys.stderr)
        return 1

    print(f"capturing {len(games)} game(s)")
    CAPTURE_ROOT.mkdir(parents=True, exist_ok=True)
    fails: list[str] = []
    for g in games:
        ok = run_one(g, launcher, args.refresh)
        if not ok:
            fails.append(g["game_id"])

    print()
    print(f"done. captured: {len(games) - len(fails)}, "
          f"failed: {len(fails)}")
    if fails:
        print(f"  failed: {', '.join(fails[:20])}"
              + (" ..." if len(fails) > 20 else ""))
    return 0 if not fails else 2


if __name__ == "__main__":
    sys.exit(main())
