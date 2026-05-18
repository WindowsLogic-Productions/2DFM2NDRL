#!/usr/bin/env python3
"""probe_upload_queue — hand-write a manifest into a game dir's
upload_queue/, then watch for the launcher to pick it up.

Pass criterion: manifest moves to upload_queue/done/ within ~5s.
Failure modes:
  - manifest stays in place: launcher's checkbox is off, or network
    is failing (open https://hub.2dfm.org/logs/healthz in a browser).
  - manifest moves to upload_queue/quarantine/: launcher rejected
    the manifest (malformed JSON, referenced file missing, server
    returned 4xx). Check the launcher's log for "UploadQueue:" lines.

Usage:
    ./probe_upload_queue.py [--game-dir PATH]

If --game-dir isn't given, autodetects from common install locations.
"""
from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path


DEFAULT_GAME_DIRS = [
    "/mnt/c/games/2dfm/wanwan",
    "/mnt/d/Games/fm2k/_NODEV/wanwan",
]


def wsl_to_windows(p: str) -> str:
    """/mnt/c/games/x → C:\\games\\x. /mnt/d/Games/x → D:\\Games\\x."""
    if p.startswith("/mnt/") and len(p) >= 7 and p[6] == "/":
        drive = p[5].upper()
        rest = p[7:].replace("/", "\\")
        return f"{drive}:\\{rest}"
    # Fallback: assume already in Windows form.
    return p


def autodetect_game_dir() -> str | None:
    for d in DEFAULT_GAME_DIRS:
        # Has an exe that smells like an FM2K install.
        if Path(d).is_dir():
            return d
    return None


def write_synthetic_log(game_dir: Path) -> Path:
    logs_dir = game_dir / "logs"
    logs_dir.mkdir(exist_ok=True)
    log_path = logs_dir / "FM2K_P1_Debug.log"
    ts = datetime.now(timezone.utc).isoformat(timespec="seconds")
    with log_path.open("a") as f:
        f.write(f"[{ts}] probe_upload_queue synthetic line\n")
        f.write(f"[{ts}] this is what the launcher should upload\n")
    return log_path


def write_manifest(game_dir: Path, log_path_wsl: Path) -> Path:
    queue = game_dir / "upload_queue"
    queue.mkdir(exist_ok=True)
    unix_ms = int(time.time() * 1000)
    fname = f"{unix_ms}_probe_p0_{unix_ms}.json"
    mpath = queue / fname

    log_path_win = wsl_to_windows(str(log_path_wsl))
    manifest = {
        "kind": "probe",  # NOTE: server's KIND_RE allows [a-z_]{2,32}
        "frame": -1,
        "session_id": str(unix_ms),
        "match_id": f"probe-{unix_ms}",
        "player_index": 0,
        "client_version": "probe",
        "game_id": "WonderfulWorld_ver_0946",
        "hook_dll_sha1": "",
        "rng_seed": "0x00000000",
        "peer_ip": "",
        "timestamp": datetime.now(timezone.utc).isoformat(timespec="seconds")
                            .replace("+00:00", "Z"),
        "files": [log_path_win],
    }
    mpath.write_text(json.dumps(manifest, indent=2))
    return mpath


def poll_for_disposition(mpath: Path, timeout: float = 10.0) -> str:
    """Returns 'done', 'quarantine', or 'stuck'."""
    done_path = mpath.parent / "done" / mpath.name
    quar_path = mpath.parent / "quarantine" / mpath.name
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if done_path.exists():
            return "done"
        if quar_path.exists():
            return "quarantine"
        if not mpath.exists():
            return "vanished"  # something moved it elsewhere
        time.sleep(0.25)
    return "stuck"


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--game-dir", help="path to game install (autodetect if omitted)")
    p.add_argument("--timeout", type=float, default=10.0,
                   help="seconds to wait for the launcher to process (default 10)")
    p.add_argument("--verify-pull", action="store_true",
                   help="after launcher processes, fetch the upload back via fm2k_logs.py show")
    args = p.parse_args(argv)

    gd = args.game_dir or autodetect_game_dir()
    if not gd:
        sys.exit(f"probe_upload_queue: no game dir given and none autodetected.\n"
                 f"Tried: {DEFAULT_GAME_DIRS}\n"
                 f"Pass --game-dir explicitly.")
    game_dir = Path(gd)
    if not game_dir.is_dir():
        sys.exit(f"probe_upload_queue: {game_dir} is not a directory")

    print(f"[probe] game_dir = {game_dir}")
    log_path = write_synthetic_log(game_dir)
    print(f"[probe] wrote synthetic log: {log_path}")
    mpath = write_manifest(game_dir, log_path)
    print(f"[probe] dropped manifest:  {mpath}")
    print(f"[probe] watching for disposition (up to {args.timeout}s)...")

    status = poll_for_disposition(mpath, timeout=args.timeout)
    if status == "done":
        print(f"[probe] PASS — manifest moved to upload_queue/done/")
        if args.verify_pull:
            # Extract session_id from manifest name and verify the pull
            # cycle works end-to-end.
            try:
                sid = json.loads(mpath.parent.joinpath("done", mpath.name).read_text())\
                    .get("session_id")
                if sid:
                    print(f"[probe] verifying pull-down for sid={sid} ...")
                    subprocess.run(
                        ["/mnt/c/dev/wanwan/tools/fm2k_logs.py", "show", sid,
                         "--tail", "3"],
                        check=False)
            except Exception as e:
                print(f"[probe] verify-pull skipped: {e}")
        return 0
    if status == "quarantine":
        print(f"[probe] FAIL — manifest moved to upload_queue/quarantine/")
        print(f"[probe] reasons usually live in the launcher's log file.")
        print(f"[probe] check {game_dir}/logs/ (or launcher's stderr)")
        return 2
    if status == "vanished":
        print(f"[probe] manifest vanished — possibly moved somewhere unexpected")
        return 3
    print(f"[probe] STUCK — manifest still at {mpath} after {args.timeout}s")
    print(f"[probe] likely causes:")
    print(f"        - launcher's 'Auto-upload crash/desync diagnostics' is OFF")
    print(f"          (check Developer section in launcher)")
    print(f"        - launcher isn't running")
    print(f"        - network down (curl https://hub.2dfm.org/logs/healthz)")
    return 1


if __name__ == "__main__":
    sys.exit(main())
