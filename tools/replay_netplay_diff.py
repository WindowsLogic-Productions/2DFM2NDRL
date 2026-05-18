#!/usr/bin/env python3
"""Replay a user-recorded .fm2krep and parity-diff against the host's
parity stream captured during that live netplay run.

Pair with replay_netplay_record.sh, which spawns the live host+joiner
loopback and captures both peers' parity. After the user closes the
launcher windows, run:

  python3 tools/replay_netplay_diff.py <host-fm2krep>

This launches the launcher in --replay mode with FM2K_PARITY_RECORD_PATH
set to a replay.pty, then diffs against tools/.netplay_manual/p1.pty.
"""

from __future__ import annotations
import argparse, os, struct, subprocess, sys, time
from pathlib import Path

LAUNCHER  = Path("/mnt/c/games/FM2K_RollbackLauncher.exe")
P1_PTY    = Path("/mnt/c/dev/wanwan/tools/.netplay_manual/p1.pty")
REPLAY_PTY = Path("/mnt/c/dev/wanwan/tools/.netplay_manual/replay.pty")
REPLAY_LOG = Path("/mnt/c/dev/wanwan/tools/.netplay_manual/replay.log")
PARITY_DIFF = Path(__file__).parent / "parity_diff.py"

def to_win(p: Path | str) -> str:
    s = str(p)
    if s.startswith("/mnt/") and len(s) > 6 and s[6] == "/":
        return s[5].upper() + ":" + s[6:]
    return s

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("fm2krep", help="Path to host-recorded .fm2krep")
    ap.add_argument("--timeout", type=float, default=180.0)
    args = ap.parse_args()

    rep_path = Path(args.fm2krep)
    if not rep_path.exists():
        print(f"[diff] FAIL: {rep_path} not found")
        return 1
    if not P1_PTY.exists():
        print(f"[diff] FAIL: P1 parity {P1_PTY} not found. Run replay_netplay_record.sh first.")
        return 1
    if REPLAY_PTY.exists(): REPLAY_PTY.unlink()
    REPLAY_LOG.parent.mkdir(parents=True, exist_ok=True)

    # Parse MATCH_START header from .fm2krep to extract chars/stage. With
    # /F boot-to-battle, the engine skips title/CSS entirely and starts
    # in battle with the chars/stage we pin via FM2K_BTB_* env vars. This
    # eliminates the pre-battle title/CSS sim divergence (host's run took
    # 1463 frames through CSS, replay's took 114 via auto-mash) which was
    # causing the first-aligned-frame rng mismatch in parity_diff.
    data = rep_path.read_bytes()
    # Header layout: 256-byte FM2KSessionFileHeader. Chars/colors at 0x80.
    p1_char  = data[0x80]
    p2_char  = data[0x81]
    p1_color = data[0x82]
    p2_color = data[0x83]
    # MATCH_START event payload (96-byte ReplayHeader) inside the body
    # has stage_id at offset 80. Scan past the 256-byte file header for
    # the first MATCH_START tag (= 5) and decode its stage_id.
    stage_id = 0
    off = 256
    while off < len(data):
        t = data[off]; off += 1
        if t == 5:  # MATCH_START
            stage_id = data[off + 80]
            break
        elif t == 1: off += 4   # INPUT
        elif t == 2: off += 4   # PIN_RNG
        elif t in (3, 4): pass  # RESET / SOUND_INIT
        elif t == 6: off += 7   # MATCH_END
        elif t == 7: off += 4   # FINGERPRINT
        elif t == 8: off += 7   # ROUND_START
        elif t == 9: off += 9   # ROUND_END
        elif t == 0x0A: off += 8  # SESSION_ID
        else: break
    print(f"[diff] MATCH_START: p1={p1_char}/c{p1_color} "
          f"p2={p2_char}/c{p2_color} stage={stage_id}")

    # NOTE: /F boot-to-battle (FM2K_BOOT_TO_BATTLE=1) was tried for replay
    # but produced WORSE divergence: /F's create_game_object(14, 127, 0, 0)
    # battle-init path differs from host's CSS-confirmed entry path, so
    # the engine state at first battle frame differs in script_idx /
    # item_idx / state_flags. Stick with the CSS auto-confirm path.
    cmd_inner = (
        f"set FM2K_PARITY_RECORD_PATH={to_win(REPLAY_PTY)}"
        f"&& {to_win(LAUNCHER)} --replay {to_win(rep_path)}"
    )
    print(f"[diff] launching --replay: {cmd_inner}")
    log_f = open(REPLAY_LOG, "wb")
    proc = subprocess.Popen(["cmd.exe", "/C", cmd_inner],
                            stdout=log_f, stderr=subprocess.STDOUT)
    deadline = time.time() + args.timeout
    rc = None
    last_size = 0
    stable_since = 0.0
    try:
        while True:
            if proc.poll() is not None:
                rc = proc.returncode
                break
            # Wait until parity .pty stops growing (= replay finished
            # consuming the .fm2krep). The replay process drains pb_queue
            # to 0 then ExitProcess(0); we still poll size in case it
            # doesn't self-exit on some path.
            cur_size = REPLAY_PTY.stat().st_size if REPLAY_PTY.exists() else 0
            now = time.time()
            if cur_size > 0 and cur_size == last_size:
                if stable_since == 0.0:
                    stable_since = now
                elif now - stable_since > 5.0:
                    # Size unchanged for 5 sec → replay done.
                    print(f"[diff] parity size stable at {cur_size} for 5s — replay done")
                    proc.terminate()
                    try: proc.wait(timeout=3)
                    except subprocess.TimeoutExpired: proc.kill()
                    rc = 0
                    break
            else:
                stable_since = 0.0
                last_size = cur_size
            if time.time() > deadline:
                print(f"[diff] TIMEOUT after {args.timeout}s; killing")
                proc.terminate()
                try: proc.wait(timeout=3)
                except subprocess.TimeoutExpired: proc.kill()
                rc = 124
                break
            time.sleep(1.0)
    finally:
        log_f.close()

    print(f"[diff] replay rc={rc}")
    if not REPLAY_PTY.exists():
        print(f"[diff] FAIL: replay parity {REPLAY_PTY} missing")
        return 1

    print(f"[diff] running parity_diff:")
    return subprocess.call([sys.executable, str(PARITY_DIFF),
                            str(P1_PTY), str(REPLAY_PTY)])

if __name__ == "__main__":
    sys.exit(main())
