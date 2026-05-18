#!/usr/bin/env python3
"""Real-netplay replay determinism test.

Spins up TWO launcher instances in 127.0.0.1 loopback (real GekkoSession,
predictive rollback, not stress). Both peers run autoplay so the match
plays out deterministically. After auto-terminate, the harness:

  1. Picks the host's .fm2krep (P1's recording — the canonical one)
  2. Re-launches that file in --replay mode
  3. Diffs record.pty (P1's parity stream) vs replay.pty
  4. Reports first divergent frame, if any

This is the FAITHFUL test for the "replays desync from matches" bug —
the stress-session harness skips real-netplay's predictive rollback,
which is what users actually hit.
"""

from __future__ import annotations
import argparse, os, shutil, subprocess, sys, time
from pathlib import Path

LAUNCHER = Path("/mnt/c/games/FM2K_RollbackLauncher.exe")
GAME_EXE = Path("/mnt/c/games/2dfm/wanwan/WonderfulWorld_ver_0946.exe")
OUT_DIR  = Path("/mnt/c/dev/wanwan/tools/.netplay_selftest")
PARITY_DIFF = Path(__file__).parent / "parity_diff.py"
P1_PORT = 7000
P2_PORT = 7001

def to_win(p: Path) -> str:
    s = str(p)
    if s.startswith("/mnt/") and len(s) > 6 and s[6] == "/":
        return s[5].upper() + ":" + s[6:]
    return s

def launch(label, args, env_extra, log_path, timeout, done_when=None):
    # WSL→Windows env vars don't cross via subprocess.Popen(env=) directly.
    # Use cmd.exe /C "set K=V && ... && exe args" so the Windows process
    # sees the env vars. Same trick the older replay_selftest.py uses.
    log_path.parent.mkdir(parents=True, exist_ok=True)
    log_f = open(log_path, "w")
    set_parts = [f"set {k}={v}" for k, v in env_extra.items()]
    win_args = []
    for a in args:
        if a.startswith("/mnt/") and len(a) > 6 and a[6] == "/":
            a = a[5].upper() + ":" + a[6:]
        win_args.append(f'"{a}"' if " " in a else a)
    cmd_inner = "&& ".join(set_parts + [" ".join(win_args)])
    cmd_args = ["cmd.exe", "/C", cmd_inner]
    print(f"[{label}] launching via cmd.exe: {' '.join(win_args)}")
    for k, v in env_extra.items():
        print(f"[{label}]   {k}={v}")
    print(f"[{label}]   log: {log_path}")
    proc = subprocess.Popen(cmd_args, stdout=log_f, stderr=subprocess.STDOUT)
    deadline = time.time() + timeout
    rc = None
    try:
        while True:
            if proc.poll() is not None:
                rc = proc.returncode
                print(f"[{label}] exited rc={rc}")
                break
            if done_when is not None and done_when():
                print(f"[{label}] done_when satisfied; killing")
                proc.terminate()
                try: proc.wait(timeout=3)
                except subprocess.TimeoutExpired: proc.kill()
                rc = 0
                break
            if time.time() > deadline:
                print(f"[{label}] TIMEOUT after {timeout}s; killing")
                proc.terminate()
                try: proc.wait(timeout=3)
                except subprocess.TimeoutExpired: proc.kill()
                rc = 124
                break
            time.sleep(0.5)
    finally:
        log_f.close()
    return rc

def find_latest_fm2krep(game_dir: Path, since: float, suffix="") -> Path | None:
    rep = game_dir / "replays"
    if not rep.is_dir(): return None
    cands = []
    for f in rep.glob(f"*{suffix}.fm2krep"):
        try:
            m = f.stat().st_mtime
            if m + 1.0 >= since: cands.append((m, f))
        except OSError: pass
    return max(cands)[1] if cands else None

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--frames", type=int, default=300)
    ap.add_argument("--record-timeout", type=float, default=180.0)
    ap.add_argument("--replay-timeout", type=float, default=120.0)
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()

    OUT_DIR.mkdir(parents=True, exist_ok=True)
    record_pty = OUT_DIR / "record.pty"
    replay_pty = OUT_DIR / "replay.pty"
    for f in (record_pty, replay_pty):
        if f.exists(): f.unlink()

    game_arg = to_win(GAME_EXE)
    game_dir = GAME_EXE.parent

    # Common env: autoplay drives title→CSS→battle, both peers compute
    # local input via Hook_ComputeAutoplayBattleInput(player_id, buf_idx).
    # FM2K_AUTO_TERMINATE_AT_FRAME fires at a confirmed battle frame on
    # each peer; both will hit ~same frame because gekko keeps them in
    # lockstep.
    common_env = {
        "FM2K_PARITY_AUTOPLAY": "1",
        "FM2K_PARITY_AUTOPLAY_BATTLE": "1",
        "FM2K_AUTO_TITLE_SKIP": "1",
        "FM2K_AUTO_TERMINATE_AT_FRAME": str(args.frames),
        # CSS auto-confirm — both peers pick char 0/color 0, stage 0,
        # press confirm. Drives both engines deterministically through
        # CSS into battle without the user mashing keys. Must be IDENTICAL
        # on both peers for gekko CSS-delay session to converge.
        "FM2K_TEST_AUTO_CSS": "0,0,0,0,0",
    }

    record_path_p1 = OUT_DIR / "p1_parity.pty"
    record_path_p2 = OUT_DIR / "p2_parity.pty"
    for f in (record_path_p1, record_path_p2):
        if f.exists(): f.unlink()

    start_ts = time.time()

    # Launch P1 (host) and P2 (joiner) concurrently. Loopback 127.0.0.1.
    # P1 = --host with FM2K_LOCAL_PORT=7000, FM2K_REMOTE_ADDR=127.0.0.1:7001
    # P2 = --connect with FM2K_LOCAL_PORT=7001, FM2K_REMOTE_ADDR=127.0.0.1:7000
    p1_env = {
        **common_env,
        "FM2K_LOCAL_PORT": str(P1_PORT),
        "FM2K_REMOTE_ADDR": f"127.0.0.1:{P2_PORT}",
        "FM2K_PARITY_RECORD_PATH": to_win(record_path_p1),
    }
    p2_env = {
        **common_env,
        "FM2K_LOCAL_PORT": str(P2_PORT),
        "FM2K_REMOTE_ADDR": f"127.0.0.1:{P1_PORT}",
        "FM2K_PARITY_RECORD_PATH": to_win(record_path_p2),
    }

    p1_log = OUT_DIR / "p1.log"
    p2_log = OUT_DIR / "p2.log"

    print("[harness] launching P1 (host) + P2 (join) in loopback")
    # Spawn both in parallel — host first to start listening.
    # --host <game-path> and --connect <addr> <game-path> are the new
    # direct-path bypasses (mirrors --stress's convention). Without the
    # path, the launcher picks the first discovered game in its registry
    # which is whichever 2DFM title ends up alphabetically first — NOT
    # necessarily wanwan.
    p1_args = [str(LAUNCHER), "--host", game_arg, "--port", str(P1_PORT)]
    p2_args = [str(LAUNCHER), "--connect", f"127.0.0.1:{P1_PORT}", game_arg,
               "--port", str(P2_PORT)]

    # done_when: wait for the harness terminator inside the hook to flush
    # the .fm2krep slice. parity.pty's first byte appears at frame 1 — way
    # before MATCH_END — so gating on .pty size kills the run before the
    # actual match completes. Instead, wait for a new .fm2krep file
    # newer than the harness start timestamp.
    def has_new_replay(): return find_latest_fm2krep(game_dir, start_ts) is not None
    def p1_done(): return has_new_replay()
    def p2_done(): return has_new_replay()

    # Background-launch P1, then P2, wait for both.
    import threading
    rc1 = [None]; rc2 = [None]
    t1 = threading.Thread(target=lambda: rc1.__setitem__(0,
        launch("P1", p1_args, p1_env, p1_log, args.record_timeout, p1_done)))
    t2 = threading.Thread(target=lambda: rc2.__setitem__(0,
        launch("P2", p2_args, p2_env, p2_log, args.record_timeout, p2_done)))
    t1.start()
    time.sleep(1.0)  # let host bind first
    t2.start()
    t1.join(); t2.join()
    print(f"[harness] P1 rc={rc1[0]} P2 rc={rc2[0]}")

    # Find P1's .fm2krep (host is canonical recorder)
    p1_rep = find_latest_fm2krep(game_dir, start_ts)
    if not p1_rep:
        print("[harness] FAIL: no .fm2krep found from netplay run")
        return 1
    print(f"[harness] P1 .fm2krep: {p1_rep}")

    # Replay phase
    print("[harness] launching REPLAY of P1's .fm2krep")
    rep_env = {
        "FM2K_PARITY_RECORD_PATH": to_win(replay_pty),
        "FM2K_AUTO_TERMINATE_AT_FRAME": str(args.frames + 20),
    }
    rep_args = [str(LAUNCHER), "--replay", to_win(p1_rep)]
    def rep_done(): return replay_pty.exists() and replay_pty.stat().st_size > 100
    rep_rc = launch("REPLAY", rep_args, rep_env, OUT_DIR / "replay.log",
                    args.replay_timeout, rep_done)
    print(f"[harness] replay rc={rep_rc}")

    if not record_path_p1.exists():
        print(f"[harness] FAIL: P1 parity {record_path_p1} missing")
        return 1
    if not replay_pty.exists():
        print(f"[harness] FAIL: replay parity {replay_pty} missing")
        return 1

    # Diff
    print("[harness] diffing P1 parity vs replay parity")
    diff_rc = subprocess.call([sys.executable, str(PARITY_DIFF),
                               str(record_path_p1), str(replay_pty)])
    return diff_rc

if __name__ == "__main__":
    sys.exit(main())
