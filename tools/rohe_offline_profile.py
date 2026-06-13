#!/usr/bin/env python3
"""Robot Heroes offline per-stage profiler.

Drives the launcher's --offline path (pure FM2K_TRUE_OFFLINE native loop, no
rollback) into battle on each stage index, idles to capture stable
[OFFLINE-SECT] (pgi/update/render ms) + [FRAMETIME] (work/frame ms) averages
from the hook's 300-frame perf instrument, then prints a table sorted by
per-frame work.

This is the automated version of Yamada's manual repro: it surfaces which
stages are slow (the heavy 100-300MB ones) and, via the section split,
whether the cost is in the sim (update -> our Hook_GameRand/sound) or the
render path (RenderFrameWithSnapshot / cnc-ddraw / giant stage sprites) --
WITHOUT touching the netcode path.

Usage:
  python3 tools/rohe_offline_profile.py                 # sweep stages 0..9
  python3 tools/rohe_offline_profile.py --stages 3,4    # just these indices
  python3 tools/rohe_offline_profile.py --secs 15       # idle longer per stage
"""
from __future__ import annotations
import argparse, re, subprocess, time
from pathlib import Path

LAUNCHER = Path("/mnt/c/games/FM2K_RollbackLauncher.exe")
GAME     = Path("/mnt/d/games/fm2k/RobotHeroes/Game/RoHe_0_7_1.exe")
LOG      = Path("/mnt/d/games/fm2k/RobotHeroes/Game/logs/FM2K_P1_Debug.log")
GAME_IMG = "RoHe_0_7_1.exe"

SECT_RE  = re.compile(r"\[OFFLINE-SECT\] n=\d+ pgi=([\d.]+)ms update=([\d.]+)ms\(max ([\d.]+)\) render=([\d.]+)ms\(max ([\d.]+)\)")
FRAME_RE = re.compile(r"\[FRAMETIME\].*work avg=([\d.]+)ms max=([\d.]+)ms \| frame avg=([\d.]+)ms max=([\d.]+)ms")
STAGE_RE = re.compile(r'\[STAGE-TABLE\] idx=(\d+) name="([^"]*)"')
# The engine LoadStageFile's the picked stage; the CreateFileA hook logs its
# bare filename. That gives us the index->name map for free, per run.
LOADED_STAGE_RE = re.compile(r"CreateFileA\[\d+\]: '([^']*\.stage)'")


def to_win(p: Path) -> str:
    """/mnt/c/foo/bar -> C:\\foo\\bar. cmd.exe wants BACKSLASHES; quoted
    forward-slash paths ("C:/games/x.exe") silently fail to launch."""
    s = str(p)
    if s.startswith("/mnt/") and len(s) > 6 and s[6] == "/":
        s = s[5].upper() + ":" + s[6:]
    return s.replace("/", "\\")


def kill_strays():
    for img in ("FM2K_RollbackLauncher.exe", GAME_IMG):
        subprocess.run(["taskkill.exe", "/F", "/T", "/IM", img],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def profile_stage(idx: int, secs: float, dump_stages: bool):
    """Launch RoHe offline on stage `idx`, wait for >=2 OFFLINE-SECT samples,
    kill, return (sect_dict, frame_dict, stage_names)."""
    kill_strays()
    time.sleep(1.0)
    if LOG.exists():
        LOG.unlink()
    env = {
        # DEV_MODE keeps the launcher from NUL'ing its diagnostics; harmless to
        # the game and matches the known-good manual launch.
        "FM2K_DEV_MODE": "1",
        "FM2K_PERF_PROFILE": "1",
        "FM2K_BTB_STAGE": str(idx),
    }
    if dump_stages:
        env["FM2K_DUMP_STAGES"] = "1"
    sets = "&& ".join(f"set {k}={v}" for k, v in env.items())
    # Quote only if the path has spaces (matches replay_selftest); a quoted
    # path with no spaces is fine, but keep it identical to the proven pattern.
    def q(s): return f'"{s}"' if " " in s else s
    inner = f'{sets}&& {q(to_win(LAUNCHER))} --offline {q(to_win(GAME))}'
    proc = subprocess.Popen(["cmd.exe", "/C", inner],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    # Heavy stages are 100-300MB and load slowly, then need 300+ battle frames
    # per [OFFLINE-SECT] emit (and the sim itself may be at half speed). Budget
    # generously; accept 1 sample if a 2nd doesn't arrive in time.
    deadline = time.time() + max(secs, 10.0) + 50.0
    sect = frame = None
    names = {}
    loaded = None
    try:
        while time.time() < deadline:
            time.sleep(1.0)
            if not LOG.exists():
                continue
            txt = LOG.read_text(errors="replace")
            for m in STAGE_RE.finditer(txt):
                names[int(m.group(1))] = m.group(2)
            for m in LOADED_STAGE_RE.finditer(txt):
                loaded = m.group(1)  # last .stage opened = the one we loaded
            sects = SECT_RE.findall(txt)
            frames = FRAME_RE.findall(txt)
            if len(sects) >= 2:           # 600 frames = stable
                sect = sects[-1]
                frame = frames[-1] if frames else None
                break
            elif len(sects) >= 1:         # keep the 1-sample result as fallback
                sect = sects[-1]
                frame = frames[-1] if frames else None
    finally:
        kill_strays()
    if loaded and idx not in names:
        names[idx] = loaded.replace(".stage", "")
    return sect, frame, names


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--stages", default="0,1,2,3,4,5,6,7,8,9")
    ap.add_argument("--secs", type=float, default=12.0)
    args = ap.parse_args()
    if not LAUNCHER.exists():
        raise SystemExit(f"launcher not found: {LAUNCHER}")
    if not GAME.exists():
        raise SystemExit(f"game not found: {GAME}")

    indices = [int(x) for x in args.stages.split(",") if x.strip() != ""]
    rows = []
    all_names = {}
    for n, idx in enumerate(indices):
        print(f"[profile] stage idx={idx} ({n+1}/{len(indices)}) ...", flush=True)
        sect, frame, names = profile_stage(idx, args.secs, dump_stages=(n == 0))
        all_names.update(names)
        if sect is None:
            print(f"[profile]   idx={idx}: NO [OFFLINE-SECT] captured "
                  f"(/F didn't reach battle, or stage invalid)")
            rows.append((idx, None))
            continue
        pgi, upd, upd_max, rnd, rnd_max = (float(x) for x in sect)
        work = float(frame[0]) if frame else (pgi + upd + rnd)
        fr   = float(frame[2]) if frame else 0.0
        rows.append((idx, dict(pgi=pgi, upd=upd, upd_max=upd_max, rnd=rnd,
                               rnd_max=rnd_max, work=work, frame=fr)))
        print(f"[profile]   idx={idx} name={all_names.get(idx,'?'):<14} "
              f"work={work:5.2f}ms frame={fr:5.2f}ms | pgi={pgi:4.2f} "
              f"update={upd:5.2f}(max {upd_max:5.2f}) render={rnd:5.2f}(max {rnd_max:5.2f})")

    print("\n==================== SORTED BY per-frame WORK (slow -> fast) ====================")
    print(f"{'idx':>3} {'stage':<16} {'work':>7} {'frame':>7} {'pgi':>6} {'update':>8} {'render':>8}")
    valid = [(i, d) for i, d in rows if d]
    for idx, d in sorted(valid, key=lambda r: -r[1]["work"]):
        print(f"{idx:>3} {all_names.get(idx,'?'):<16} {d['work']:6.2f}m {d['frame']:6.2f}m "
              f"{d['pgi']:5.2f}m {d['upd']:7.2f}m {d['rnd']:7.2f}m")
    if valid:
        slow = max(valid, key=lambda r: r[1]["work"])
        fast = min(valid, key=lambda r: r[1]["work"])
        ds, df = slow[1], fast[1]
        print(f"\nslowest idx={slow[0]} ({all_names.get(slow[0],'?')}) work={ds['work']:.2f}ms"
              f"  vs fastest idx={fast[0]} ({all_names.get(fast[0],'?')}) work={df['work']:.2f}ms")
        # Where did the extra time go between fast and slow?
        d_upd = ds["upd"] - df["upd"]
        d_rnd = ds["rnd"] - df["rnd"]
        print(f"delta update={d_upd:+.2f}ms  render={d_rnd:+.2f}ms  -> "
              f"culprit is {'UPDATE (sim: Hook_GameRand/sound/object scripts)' if d_upd > d_rnd else 'RENDER (RenderFrameWithSnapshot / cnc-ddraw / stage sprites)'}")


if __name__ == "__main__":
    main()
