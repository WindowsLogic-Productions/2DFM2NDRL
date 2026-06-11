#!/usr/bin/env python3
"""Live-spectator determinism self-test.

Spins up THREE launcher instances on 127.0.0.1:

  P1   --host    <game> --port 7000   (autoplay, parity -> p1_parity.pty)
  P2   --connect 127.0.0.1:7000 --port 7001   (autoplay)
  SPEC --spectate 127.0.0.1:7000 --port 7002  (passive viewer,
       parity -> spec_parity.pty, joins during title/CSS so the host
       backfills the full session -- FULL_SESSION-equivalent path)

The host terminates at FM2K_AUTO_TERMINATE_AT_FRAME battle frames; the
harness then waits for the spectator's parity stream to go quiescent,
kills everything, and diffs host parity vs spectator parity with
tools/parity_diff.py. ALL ALIGNED FRAMES IDENTICAL = the spectator's
input-replay sim bit-matched the host's live sim.

Notes:
  * The spectator trails the host by the EVENT_BATCH flush cadence
    (every 8 confirmed frames) plus TCP latency, and the host
    TerminateProcess()es immediately at the target frame -- the spec's
    last few frames may never arrive. --min-coverage (default
    frames-100) guards against a vacuous pass on a short stream.
  * Mid-battle CURRENT_MATCH snapshot join is NOT covered here yet:
    parity_diff aligns index-paired from each side's first battle
    snapshot, which only matches a from-the-start join. A --join-delay
    mode needs frame-keyed alignment first.

Usage:
  python3 tools/spec_selftest.py --frames 1500
  FM2K_LOCAL_DELAY=0 python3 tools/spec_selftest.py --frames 1500 --keep
"""

from __future__ import annotations
import argparse, os, subprocess, sys, threading, time
from pathlib import Path

LAUNCHER = Path("/mnt/c/games/FM2K_RollbackLauncher.exe")
GAME_EXE = Path("/mnt/c/games/2dfm/wanwan/WonderfulWorld_ver_0946.exe")
OUT_DIR  = Path("/mnt/c/dev/wanwan/tools/.spec_selftest")
PARITY_DIFF = Path(__file__).parent / "parity_diff.py"
P1_PORT, P2_PORT, SPEC_PORT = 7000, 7001, 7002


def to_win(p: Path) -> str:
    s = str(p)
    if s.startswith("/mnt/") and len(s) > 6 and s[6] == "/":
        return s[5].upper() + ":" + s[6:]
    return s


def kill_strays():
    for image in ("FM2K_RollbackLauncher.exe", "WonderfulWorld_ver_0946.exe"):
        subprocess.run(["taskkill.exe", "/F", "/T", "/IM", image],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def launch(label, args, env_extra, log_path, timeout, done_when=None):
    # WSL->Windows env vars don't cross subprocess.Popen(env=); use the
    # cmd.exe `set K=V&& ...` trick (same as the other harnesses).
    log_path.parent.mkdir(parents=True, exist_ok=True)
    log_f = open(log_path, "w")
    set_parts = [f"set {k}={v}" for k, v in env_extra.items()]
    win_args = []
    for a in args:
        if a.startswith("/mnt/") and len(a) > 6 and a[6] == "/":
            a = a[5].upper() + ":" + a[6:]
        win_args.append(f'"{a}"' if " " in a else a)
    cmd_inner = "&& ".join(set_parts + [" ".join(win_args)])
    print(f"[{label}] launching: {' '.join(win_args)}")
    for k, v in env_extra.items():
        print(f"[{label}]   {k}={v}")
    proc = subprocess.Popen(["cmd.exe", "/C", cmd_inner],
                            stdout=log_f, stderr=subprocess.STDOUT)
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
                try:
                    proc.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    proc.kill()
                rc = 0
                break
            if time.time() > deadline:
                print(f"[{label}] TIMEOUT after {timeout}s; killing")
                proc.terminate()
                try:
                    proc.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    proc.kill()
                rc = 124
                break
            time.sleep(0.5)
    finally:
        log_f.close()
    return rc


def find_latest_fm2krep(game_dir: Path, since: float, suffix="") -> Path | None:
    rep = game_dir / "replays"
    if not rep.is_dir():
        return None
    cands = []
    for f in rep.glob(f"*{suffix}.fm2krep"):
        try:
            m = f.stat().st_mtime
            if m + 1.0 >= since:
                cands.append((m, f))
        except OSError:
            pass
    return max(cands)[1] if cands else None


def pty_snapshots(path: Path) -> int:
    try:
        return max(0, (path.stat().st_size - 32) // 260)
    except OSError:
        return 0


def wait_ports_free(ports, timeout=20.0):
    """Poll Windows netstat until none of `ports` appear as bound UDP
    sockets. A fixed post-taskkill sleep is unreliable: socket teardown
    occasionally outlives it and the next bind() fails err=10013."""
    needles = [f":{p} " for p in ports]
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            out = subprocess.run(["netstat.exe", "-an", "-p", "UDP"],
                                 capture_output=True, text=True,
                                 timeout=10).stdout
        except Exception:
            return  # netstat unavailable; fall back to hoping
        if not any(n in out for n in needles):
            return
        time.sleep(1.0)
    print(f"[harness] WARN: ports {ports} still bound after {timeout}s; "
          f"launching anyway")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--frames", type=int, default=1500,
                    help="battle frames the host plays before terminating")
    ap.add_argument("--record-timeout", type=float, default=240.0)
    ap.add_argument("--spec-join-delay", type=float, default=3.0,
                    help="seconds after P1/P2 launch before the spectator dials in")
    ap.add_argument("--min-coverage", type=int, default=-1,
                    help="minimum spectator battle frames required "
                         "(default: frames - 100)")
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()
    min_coverage = args.min_coverage if args.min_coverage >= 0 else args.frames - 100

    OUT_DIR.mkdir(parents=True, exist_ok=True)
    p1_pty   = OUT_DIR / "p1_parity.pty"
    spec_pty = OUT_DIR / "spec_parity.pty"
    for f in (p1_pty, spec_pty):
        if f.exists():
            f.unlink()

    game_arg = to_win(GAME_EXE)
    game_dir = GAME_EXE.parent
    kill_strays()
    time.sleep(1.0)
    wait_ports_free([P1_PORT, P2_PORT, SPEC_PORT])

    common_env = {
        "FM2K_PARITY_AUTOPLAY": "1",
        "FM2K_PARITY_AUTOPLAY_BATTLE": "1",
        "FM2K_AUTO_TITLE_SKIP": "1",
        "FM2K_AUTO_TERMINATE_AT_FRAME": str(args.frames),
        "FM2K_TEST_AUTO_CSS": "0,0,0,0,0",
    }
    for k in ("FM2K_LOCAL_DELAY", "FM2K_PRED_WINDOW", "FM2K_PREDICTION_WINDOW", "FM2K_RUNAHEAD"):
        if os.environ.get(k):
            common_env[k] = os.environ[k]

    p1_env = {**common_env,
              "FM2K_LOCAL_PORT": str(P1_PORT),
              "FM2K_REMOTE_ADDR": f"127.0.0.1:{P2_PORT}",
              "FM2K_PARITY_RECORD_PATH": to_win(p1_pty)}
    p2_env = {**common_env,
              "FM2K_LOCAL_PORT": str(P2_PORT),
              "FM2K_REMOTE_ADDR": f"127.0.0.1:{P1_PORT}"}
    # Spectator: no autoplay -- it is driven entirely by the host's event
    # stream. Only the parity recorder env matters.
    spec_env = {"FM2K_PARITY_RECORD_PATH": to_win(spec_pty)}

    start_ts = time.time()

    def has_new_replay():
        return find_latest_fm2krep(game_dir, start_ts, suffix="_p0_harness") is not None

    p1_args = [str(LAUNCHER), "--host", game_arg, "--port", str(P1_PORT)]
    p2_args = [str(LAUNCHER), "--connect", f"127.0.0.1:{P1_PORT}", game_arg,
               "--port", str(P2_PORT)]
    # --host <game> supplies the direct-path game selection; --spectate
    # flips the launch into passive-viewer mode (see FM2K_RollbackClient
    # direct/spectate block). session-kind stays at the default "battle"
    # (boot-to-battle): the spec boots straight to game_mode 3000 with
    # placeholder chars, the deferred CURRENT_MATCH snapshot applies, and
    # the input tail replays from the battle-start anchor. DO NOT pass
    # "menu" here -- CURRENT_MATCH has no pre-anchor title/CSS events, so
    # a menu-walk spectator waits at the title screen forever (verified
    # 2026-06-11: 592 captured frames, all phase=1000).
    #
    # The snapshot is stashed at Netplay_StartBattle, so the spec replays
    # battle frames from 0 regardless of when it joins -- parity_diff's
    # index-paired battle alignment against the host stream is valid.
    spec_args = [str(LAUNCHER), "--host", game_arg,
                 "--spectate", f"127.0.0.1:{P1_PORT}",
                 "--port", str(SPEC_PORT)]

    print("[harness] launching P1 (host) + P2 (join), spectator joins "
          f"{args.spec_join_delay:.0f}s later")
    rcs = [None, None, None]
    t1 = threading.Thread(target=lambda: rcs.__setitem__(0,
        launch("P1", p1_args, p1_env, OUT_DIR / "p1.log",
               args.record_timeout, has_new_replay)))
    t2 = threading.Thread(target=lambda: rcs.__setitem__(1,
        launch("P2", p2_args, p2_env, OUT_DIR / "p2.log",
               args.record_timeout, has_new_replay)))

    # Spectator completion: parity stream quiescent (no growth for 6s
    # after real content) -- the host's TerminateProcess cuts the TCP
    # feed, so the spec drains whatever arrived and then idles.
    spec_state = {"size": -1, "since": 0.0}
    def spec_done():
        if not has_new_replay():
            return False    # match still running; keep consuming
        try:
            sz = spec_pty.stat().st_size
        except OSError:
            return False
        now = time.time()
        if sz != spec_state["size"]:
            spec_state["size"] = sz
            spec_state["since"] = now
            return False
        return sz > 32 + 260 * 10 and (now - spec_state["since"]) >= 6.0

    t3 = threading.Thread(target=lambda: rcs.__setitem__(2,
        launch("SPEC", spec_args, spec_env, OUT_DIR / "spec.log",
               args.record_timeout + 60, spec_done)))

    t1.start()
    time.sleep(1.0)
    t2.start()
    time.sleep(args.spec_join_delay)
    t3.start()
    t1.join(); t2.join(); t3.join()
    kill_strays()
    print(f"[harness] rcs: P1={rcs[0]} P2={rcs[1]} SPEC={rcs[2]}")

    if not p1_pty.exists():
        print("[harness] FAIL: host parity missing")
        return 1
    if not spec_pty.exists():
        print("[harness] FAIL: spectator parity missing -- spectator never "
              "joined or never captured (check .spec_selftest/spec.log and "
              "logs/FM2K_P3_Debug.log)")
        return 1

    host_n, spec_n = pty_snapshots(p1_pty), pty_snapshots(spec_pty)
    print(f"[harness] host parity: {host_n} snapshots; spec parity: {spec_n}")
    if spec_n < min_coverage:
        print(f"[harness] FAIL: spectator covered only {spec_n} frames "
              f"(< required {min_coverage}) -- stream stalled or join failed")
        return 1

    print("[harness] diffing HOST parity vs SPECTATOR parity")
    diff_rc = subprocess.call([sys.executable, str(PARITY_DIFF),
                               str(p1_pty), str(spec_pty)])

    if not args.keep and diff_rc == 0:
        for f in (p1_pty, spec_pty, OUT_DIR / "p1.log", OUT_DIR / "p2.log",
                  OUT_DIR / "spec.log"):
            f.unlink(missing_ok=True)
    return diff_rc


if __name__ == "__main__":
    sys.exit(main())
