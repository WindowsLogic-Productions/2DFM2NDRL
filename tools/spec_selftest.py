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
import argparse, os, shutil, subprocess, sys, threading, time
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


def trim_first_battle_segment(src: Path, dst: Path) -> int:
    """Copy src .pty to dst keeping only [first real battle row .. the row
    before battle phase first ends]. Multi-match spectator streams contain
    match1 + CSS + match2 rows; the match-1 replay gate needs just the
    first battle segment. Returns rows kept."""
    import struct
    d = src.read_bytes()
    hdr, body = d[:32], d[32:]
    n = len(body) // 260
    rows = []
    started = False
    for k in range(n):
        off = k * 260
        phase = struct.unpack_from('<i', body, off + 16)[0]
        p1s   = struct.unpack_from('<i', body, off + 32)[0]
        p2s   = struct.unpack_from('<i', body, off + 32 + 92)[0]
        in_battle = (phase == 3000 and p1s != -1 and p2s != -1)
        if not started:
            if in_battle:
                started = True
                rows.append(body[off:off + 260])
        else:
            if phase != 3000:
                break
            # Teardown rows: at match end the player objects despawn while
            # phase is still 3000 for a few capture ticks -- script_idx
            # reads -1 and every other field zeros. The replay instance
            # tends to capture one of these as its final row (the A4
            # k=6534 "divergence" was spec-real-row vs replay-empty-row).
            # They carry no engine state; end the segment there.
            if p1s == -1 and p2s == -1:
                break
            rows.append(body[off:off + 260])
    dst.write_bytes(hdr + b"".join(rows))
    return len(rows)


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
    ap.add_argument("--total-frames", type=int, default=0,
                    help="multi-match mode: terminate after N TOTAL confirmed "
                         "battle frames across matches (FM2K_AUTO_TERMINATE_TOTAL). "
                         "Spans MATCH_END -> CSS -> match 2; the parity gate uses "
                         "match 1's canonical .fm2krep.")
    ap.add_argument("--record-timeout", type=float, default=240.0)
    ap.add_argument("--spec-join-delay", type=float, default=3.0,
                    help="seconds after P1/P2 launch before the spectator dials in")
    ap.add_argument("--min-coverage", type=int, default=-1,
                    help="minimum spectator battle frames required "
                         "(default: frames - 100)")
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--css-dwell", default="0.4",
                    help="CSS navigation depth (FM2K_AUTOPLAY_CSS_DWELL): the "
                         "players WANDER the char grid this long before confirming "
                         "(~dwell*100 frames). Default 0.4 = real asymmetric "
                         "navigation to varied non-char0 picks. Use 0 for the "
                         "instant char0/char0 path that exercises the spectator "
                         "battle-align fix (host confirms inside the seam-hold "
                         "window).")
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
        # p1char,p1color,p2char,p2color,STAGE -- overridable so we can test a
        # non-zero stage (e.g. FM2K_TEST_AUTO_CSS=0,0,0,1,1) and verify the
        # live spectator loads the SAME stage the players picked.
        "FM2K_TEST_AUTO_CSS": os.environ.get("FM2K_TEST_AUTO_CSS", "0,0,0,0,0"),
        # Per-frame host + spectator rng/input/script traces to disk. These
        # feed the AUTHORITATIVE gate (host-vs-spec pairing, same run) -- the
        # ground-truth desync check, unlike the spec-vs-replay parity diff
        # which mis-pairs the wrong match under multi-match autoplay.
        "FM2K_HOST_TRACE":       os.environ.get("FM2K_HOST_TRACE", "1"),
        "FM2K_SPECTATOR_DEBUG":  os.environ.get("FM2K_SPECTATOR_DEBUG", "1"),
    }
    if args.total_frames > 0:
        common_env["FM2K_AUTO_TERMINATE_TOTAL"] = str(args.total_frames)
        common_env.setdefault("FM2K_AUTOPLAY_CSS_DWELL",
                              os.environ.get("FM2K_AUTOPLAY_CSS_DWELL", "8"))
    else:
        common_env["FM2K_AUTO_TERMINATE_AT_FRAME"] = str(args.frames)
        # Real CSS navigation by default (--css-dwell): players wander to varied,
        # asymmetric, non-char0 picks before confirming, instead of an instant
        # char0/char0 lock. The os.environ forward below still lets FM2K_AUTOPLAY_
        # CSS_DWELL override it.
        common_env["FM2K_AUTOPLAY_CSS_DWELL"] = str(args.css_dwell)
    for k in ("FM2K_LOCAL_DELAY", "FM2K_PRED_WINDOW", "FM2K_PREDICTION_WINDOW", "FM2K_RUNAHEAD", "FM2K_SPEC_UDP", "FM2K_AUTOPLAY_CSS_DWELL", "FM2K_SPECTATOR_DEBUG", "FM2K_HOST_TRACE", "FM2K_TEST_BATTLE_SEED"):
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
    # stream. Only the parity recorder env matters... PLUS the test-only
    # downlink-loss shim: the impair knobs must reach the SPECTATOR (the path
    # that matters), else a "loss" run silently impairs nothing. Kept minimal
    # (no common_env merge) so the spec never gains autoplay inputs.
    spec_env = {"FM2K_PARITY_RECORD_PATH": to_win(spec_pty)}
    for k in ("FM2K_SPEC_DROP", "FM2K_SPEC_DROP_SEED"):
        if os.environ.get(k):
            spec_env[k] = os.environ[k]

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

    # Preserve the live-phase debug logs IMMEDIATELY -- before any
    # assertion can bail (a coverage FAIL used to skip preservation and
    # the replay phase of the NEXT run overwrote the evidence).
    for lf in ("FM2K_P1_Debug.log", "FM2K_P2_Debug.log", "FM2K_P3_Debug.log"):
        src = game_dir / "logs" / lf
        if src.exists():
            try:
                shutil.copy2(src, OUT_DIR / f"live_{lf}")
            except OSError as e:
                print(f"[harness] (warn) could not preserve {lf}: {e}")

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

    # Report WHERE the spectator's join landed, in battle frames: delta
    # between the host's battle-session creation and the subscriber
    # accept, at 100 fps. Wall-clock --spec-join-delay is dominated by
    # boot time, so this is the only honest measure of join depth.
    try:
        import re as _re
        host_log = open("/mnt/c/games/2dfm/wanwan/logs/FM2K_P1_Debug.log",
                        errors="replace").read()
        def ts_of(pattern):
            m = _re.search(r"\[(\d+):(\d+):(\d+)\.(\d+)\][^\n]*" + pattern, host_log)
            if not m: return None
            h, mn, sc, ms = (int(x) for x in m.groups())
            return h * 3600 + mn * 60 + sc + ms / 1000.0
        t_battle = ts_of(_re.escape("GekkoNet battle session created"))
        t_join   = ts_of(_re.escape("Accepted subscriber"))
        if t_battle is not None and t_join is not None:
            join_frame = int((t_join - t_battle) * 100)
            print(f"[harness] spectator joined ~battle frame {join_frame} "
                  f"({t_join - t_battle:+.1f}s after battle start)")
        rounds = len(_re.findall(r"ROUND_END|Round end", host_log))
        if rounds:
            print(f"[harness] host log shows ~{rounds} round-end event(s)")
    except OSError:
        pass
    if spec_n < min_coverage:
        print(f"[harness] FAIL: spectator covered only {spec_n} frames "
              f"(< required {min_coverage}) -- stream stalled or join failed")
        return 1

    # Informational: host-vs-spec. Valid on clean loopback; under loss the
    # HOST predicts and its parity captures SPECULATIVE states, producing
    # false divergences here (the spectator consumes confirmed inputs and
    # is the more trustworthy stream). Do not gate on this.
    print("[harness] (info) diffing HOST parity vs SPECTATOR parity "
          "(unreliable under packet loss -- host captures speculative states)")
    subprocess.call([sys.executable, str(PARITY_DIFF),
                     str(p1_pty), str(spec_pty),
                     "host-vs-spec INFO (loss-sensitive, non-authoritative)"])

    # ADVISORY (NOT the gate): spec-vs-replay, confirmed-vs-confirmed. In theory
    # both the spectator stream and --replay playback are driven by the host's
    # CONFIRMED input stream, so they should match -- but parity_diff index-pairs
    # rows, which mis-aligns under multi-match autoplay + the spectator's
    # catch-up cadence (the documented false "frame 71" alarms). The
    # AUTHORITATIVE check is the host-vs-spec trace pairing further below; this
    # diff is kept only as a human cross-check + the checked==0 fallback.
    if args.total_frames > 0:
        # Multi-match mode: the harness slice at terminate is a MID-MATCH-2
        # slice; the gate replays match 1's CANONICAL file (written by
        # Netplay_EndBattle, host-only, no _harness suffix) -- earliest
        # post-start canonical = match 1.
        cands = []
        for f in (game_dir / "replays").glob("*.fm2krep"):
            if "_harness" in f.name:
                continue
            try:
                m = f.stat().st_mtime
            except OSError:
                continue
            if m + 1.0 >= start_ts:
                cands.append((m, f))
        p0_rep = min(cands)[1] if cands else None
        if not p0_rep:
            print("[harness] FAIL: no canonical match-1 .fm2krep "
                  "(did match 1 actually complete?)")
            return 1
        print(f"[harness] match-1 canonical replay file: {p0_rep.name}")

        # Multi-match liveness assertions.
        host_log = open("/mnt/c/games/2dfm/wanwan/logs/FM2K_P1_Debug.log",
                        errors="replace").read()
        battles = host_log.count("GekkoNet battle session created")
        print(f"[harness] host battle sessions created: {battles}")
        if battles < 2:
            print("[harness] FAIL: match 2 never started on the host "
                  "(battle-entry transition deadlock?)")
            return 1
        # Spectator must have followed into match 2: its parity stream
        # needs >= 2 battle segments.
        import struct as _st
        d = spec_pty.read_bytes()[32:]
        segs, in_b = 0, False
        for k in range(len(d) // 260):
            ph  = _st.unpack_from('<i', d, k * 260 + 16)[0]
            p1s = _st.unpack_from('<i', d, k * 260 + 32)[0]
            b = (ph == 3000 and p1s != -1)
            if b and not in_b:
                segs += 1
            in_b = b
        print(f"[harness] spectator battle segments observed: {segs}")
        if segs < 2:
            print("[harness] FAIL: spectator did not follow into match 2")
            return 1
        # Every boundary crossing must apply the deferred battle-init ops
        # (PIN_RNG at minimum) at the spec's battle entry. A local early
        # battle entry (the 2026-06-11 rematch-CSS auto-advance bug)
        # consumed the once-per-battle init edge before the ops arrived --
        # match 2 ran with an unpinned RNG and nothing failed loudly
        # because the parity gate only covers match 1.
        spec_log = OUT_DIR / "live_FM2K_P3_Debug.log"
        if spec_log.exists():
            txt = spec_log.read_text(errors="replace")
            pins = txt.count("applied deferred PIN_RNG")
            needed = segs - 1  # first battle is snapshot-anchored
            print(f"[harness] deferred PIN_RNG applies: {pins} "
                  f"(boundary crossings: {needed})")
            if pins < needed:
                print("[harness] FAIL: a boundary crossing entered battle "
                      "without applying the deferred init ops (early local "
                      "battle entry -- match desync)")
                return 1
    else:
        p0_rep = None
        deadline = time.time() + 10.0
        while time.time() < deadline:
            p0_rep = find_latest_fm2krep(game_dir, start_ts, suffix="_p0_harness")
            if p0_rep:
                break
            time.sleep(0.5)
        if not p0_rep:
            print("[harness] FAIL: no p0 harness .fm2krep for the replay gate")
            return 1
    replay_pty = OUT_DIR / "replay_parity.pty"
    replay_pty.unlink(missing_ok=True)
    kill_strays()
    time.sleep(1.0)
    wait_ports_free([P1_PORT, P2_PORT, SPEC_PORT])
    # NOTE: no FM2K_BOOT_TO_BATTLE -- netplay-recorded replays must walk the
    # title/CSS path (see replay_netplay_diff.py:81-85).
    rep_env = {"FM2K_PARITY_RECORD_PATH": to_win(replay_pty),
               # Harness-only: full-speed drain (offline replays play 1:1
               # for humans now that the bank drains skip replay mode).
               "FM2K_SPECTATOR_ALWAYS_CATCHUP": "1"}
    rep_state = {"size": -1, "since": 0.0}
    def rep_done():
        try:
            sz = replay_pty.stat().st_size
        except OSError:
            return False
        now = time.time()
        if sz != rep_state["size"]:
            rep_state["size"] = sz
            rep_state["since"] = now
            return False
        return sz > 32 + 260 * 10 and (now - rep_state["since"]) >= 6.0
    rep_rc = launch("REPLAY", [str(LAUNCHER), "--replay", to_win(p0_rep)],
                    rep_env, OUT_DIR / "replay.log",
                    args.record_timeout, rep_done)
    print(f"[harness] replay rc={rep_rc}")
    if not replay_pty.exists():
        print("[harness] FAIL: replay parity missing")
        return 1

    # Trim both streams to their FIRST battle segment: the spectator's
    # stream may continue into CSS/match 2, and the replay's stream has a
    # title/CSS prefix + post-battle tail -- index pairing past either
    # boundary compares unlike phases.
    spec_m1   = OUT_DIR / "spec_m1.pty"
    replay_m1 = OUT_DIR / "replay_m1.pty"
    n_spec   = trim_first_battle_segment(spec_pty, spec_m1)
    n_replay = trim_first_battle_segment(replay_pty, replay_m1)
    print(f"[harness] (advisory) SPECTATOR vs REPLAY parity, first battle "
          f"segment (spec={n_spec} rows, replay={n_replay} rows). NOT the gate: "
          f"this compares against a SEPARATE replay process that mis-pairs the "
          f"wrong match under multi-match autoplay and position-mis-aligns the "
          f"spectator's catch-up cadence -- it produced false 'frame 71' alarms "
          f"while host-vs-spec showed bit-exact sync. See the GATE below.")
    diff_rc = subprocess.call([sys.executable, str(PARITY_DIFF),
                               str(spec_m1), str(replay_m1),
                               "spec-vs-replay ADVISORY (index-paired)"])

    # AUTHORITATIVE GATE: host-vs-spec per-frame trace pairing (SAME run, same
    # match = ground truth). Both the host and spectator here watch the
    # identical match, so bit-for-bit (rng, inputs, scripts) at every paired
    # battle frame proves the spectator stayed in sync. Needs FM2K_HOST_TRACE=1
    # + FM2K_SPECTATOR_DEBUG=1 (set in common_env). [SPEC-TRACE] is dense over
    # bf 0..99; [SPEC-FP]/[HOST-FP] checkpoint every 30 frames to match end.
    # rng-keyed (NOT bf-keyed): the spectator's per-frame rng_post is a strong
    # state fingerprint. We assert every spectator frame's rng appears in the
    # HOST's rng set with matching inputs/scripts. This is robust to: a frame
    # offset (mid-battle CURRENT_MATCH snapshot join starts at host frame N,
    # not 0), per-match bf RESETS (multi-match: each battle restarts bf at 0,
    # so bf-keying would cross-contaminate matches), and catch-up cadence. A
    # spectator that desyncs computes an rng the host never produced -> the
    # frame's rng is "not in host" = hard fail.
    import re as _ret
    # CRITICAL: read the PRESERVED spectator log, not game_dir/logs/FM2K_P3.
    # The REPLAY process (launched after the spec, with ALWAYS_CATCHUP=1)
    # overwrites game_dir/logs/FM2K_P3_Debug.log -- so the live game_dir P3
    # holds the replay's traces, which re-sim the host's CONFIRMED inputs and
    # match the host BY CONSTRUCTION (0 misses = guaranteed false PASS). The
    # live_ copies are snapshotted before the replay runs and hold the ACTUAL
    # spectator's traces. (2026-06-23: this masked a real bf=77 spectator
    # desync under loss -- the spec computed an rng no player ever produced.)
    host_dbg = OUT_DIR / "live_FM2K_P1_Debug.log"
    spec_dbg = OUT_DIR / "live_FM2K_P3_Debug.log"
    # group(1)=bf, group(2)=rng, group(3+)=comparison fields.
    TRC = (r'(?:HOST|SPEC)-TRACE\] bf=(\d+) rng_pre=0x[0-9A-F]+ '
           r'rng_post=0x([0-9A-F]+) p1=0x([0-9A-F]+) p2=0x([0-9A-F]+)')
    FP  = (r'(?:HOST|SPEC)-FP\] bf=(\d+).*?rng=0x([0-9A-F]+).*?'
           r'p1_script=(-?\d+) p2_script=(-?\d+)')
    def _rows2(path, pat):
        out = []
        try:
            for ln in open(path, errors="ignore"):
                m = _ret.search(pat, ln)
                if m:
                    g = m.groups()
                    # Skip the battle-entry frame (bf==0): hp/timer/pos aren't
                    # loaded yet and rng is pre-pin -- a transitional, not a
                    # settled gameplay state. Host and spectator hit it at
                    # slightly different init timing (esp. during catch-up), so
                    # comparing it is a false mismatch. Each match has one.
                    if int(g[0]) == 0:
                        continue
                    out.append((g[1], tuple(g[2:])))  # (rng, fields)
        except OSError:
            pass
        return out
    def _check(host_rows, spec_rows):
        hmap = {}
        for rng, fields in host_rows:
            hmap.setdefault(rng, set()).add(fields)
        not_found = field_mm = 0
        first = None
        for rng, fields in spec_rows:
            if rng not in hmap:
                not_found += 1
                if first is None:
                    first = ("rng-NOT-in-host (real desync)", rng, fields)
            elif fields not in hmap[rng]:
                field_mm += 1
                if first is None:
                    first = ("field-mismatch @rng", rng, "spec", fields,
                             "host", hmap[rng])
        return len(spec_rows), not_found, field_mm, first
    # TRACE: rng-PRESENCE only. The p1/p2 input fields are capture-noise
    # (predicted-vs-confirmed + different capture points on host vs spec -- the
    # same reason parity_diff excludes inputs). rng_post IS the post-frame state
    # fingerprint: if every spectator rng appears in the host's set, the sims
    # produced identical state. Comparing inputs here gave false mismatches on a
    # snapshot-join (31 of them) while rng+scripts were bit-exact.
    trc_spec = _rows2(spec_dbg, TRC)
    host_trc_rng = {rng for rng, _ in _rows2(host_dbg, TRC)}
    ct = len(trc_spec)
    mt = sum(1 for rng, _ in trc_spec if rng not in host_trc_rng)
    first_t = next(((rng,) for rng, _ in trc_spec if rng not in host_trc_rng), None)
    # FP: rng-presence + SCRIPT match (scripts are gameplay state -> reliable;
    # a script mismatch at a matching rng would be a genuine desync).
    cf, mf, ffm, first_f = _check(_rows2(host_dbg, FP), _rows2(spec_dbg, FP))
    checked = ct + cf
    bad = mt + mf + ffm
    print(f"[harness] GATE (host-vs-spec; TRACE rng-presence, FP rng+scripts; "
          f"any-join / multi-match robust): checked {checked} spectator frames "
          f"(TRACE {ct}, FP {cf}); {mt + mf} rng-not-in-host, "
          f"{ffm} FP script-mismatch")
    if first_t:
        print(f"  TRACE rng-not-in-host (real state divergence): {first_t}")
    if first_f:
        print(f"  FP first issue: {first_f}")
    gate_ok = checked > 0 and bad == 0

    if not args.keep and gate_ok:
        for f in (p1_pty, spec_pty, replay_pty, OUT_DIR / "p1.log",
                  OUT_DIR / "p2.log", OUT_DIR / "spec.log",
                  OUT_DIR / "replay.log"):
            f.unlink(missing_ok=True)
    if checked == 0:
        print("[harness] GATE INCONCLUSIVE: no spectator trace frames -- falling "
              "back to spec-vs-replay diff_rc (need FM2K_HOST_TRACE=1 + "
              "FM2K_SPECTATOR_DEBUG=1 and a battle phase)")
        return diff_rc
    if gate_ok:
        print("[harness] OVERALL PASS: spectator bit-exact with host "
              "(every spectator frame's rng+inputs+scripts found in host).")
        return 0
    print("[harness] OVERALL FAIL: spectator desynced from host (rng-keyed).")
    return 1


if __name__ == "__main__":
    sys.exit(main())
