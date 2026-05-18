#!/usr/bin/env python3
"""fm2k_logs — dev CLI for crash/desync log pulldown.

Designed to be invoked by an LLM (Claude) during debugging sessions, so
output is structured, paths are absolute, and bundles land in a
predictable cache layout for follow-up Read calls.

Reads the hub's shared secret from ~/.config/fm2k/log_secret (mode 600).
Optional FM2K_LOGS_HUB env var overrides the default endpoint.

Subcommands:
    summary
        At-a-glance counts by kind/version/game + cross-peer matches.
        First thing to check on a debugging session: "what's pending?"

    recent  [--kind X] [--version V] [--game G] [--match M]
            [--since ISO] [--limit N] [--json]
        List recent uploads with filters. All filters AND together.

    pull <session_id> [--into DIR]
        Rsync all bundles for a session into DIR (default: tools/.fm2k_logs/).
        Prints absolute paths of pulled files on stdout.

    pull-match <match_id> [--into DIR]
        Pull BOTH peers' bundles for a match. Best entry point for
        analyzing a desync — gives you P1's and P2's diff side-by-side.

    pull-recent [--kind X] [--version V] [--game G] [--since ISO]
                [--limit N] [--into DIR]
        Combo of `recent` + `pull` for each result.

    show <session_id> [--player N] [--kind X] [--tail LINES]
        Pull (if not cached) then print meta.json + last N log lines.
        Default --tail 200. Use --tail 0 for full files.

    show-match <match_id> [--tail LINES]
        Pull both peers + print their meta + log tails interleaved.

    tail [--interval SEC]
        Poll /recent every N seconds, print each new upload as it lands.

    open <stored_path>
        Pull a single bundle by its server-side stored_path (returned by
        `recent --json` as the .stored_path field). Prints local dir.

Examples:
    fm2k_logs.py summary
    fm2k_logs.py recent --version 0.2.41 --kind desync --limit 5
    fm2k_logs.py pull-match ww-12345-67890
    fm2k_logs.py show 305419896 --tail 100
"""
from __future__ import annotations

import argparse
import json
import os
import shlex
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path
from typing import Any, Iterable


# ---------- config ----------

DEFAULT_HUB = "https://hub.2dfm.org/logs"
SSH_HOST    = "2dfm-root"  # has read access to /var/lib/fm2k_logs
SECRET_PATH = Path(os.path.expanduser("~/.config/fm2k/log_secret"))
CACHE_DIR   = Path(__file__).resolve().parent / ".fm2k_logs"


def _hub_base() -> str:
    return os.environ.get("FM2K_LOGS_HUB", DEFAULT_HUB).rstrip("/")


def _load_secret() -> str:
    if not SECRET_PATH.exists():
        sys.exit(f"fm2k_logs: secret file missing at {SECRET_PATH}\n"
                 f"Create it with mode 600 and the same value baked into the "
                 f"launcher build:\n"
                 f"  echo <secret> > {SECRET_PATH}\n"
                 f"  chmod 600 {SECRET_PATH}")
    s = SECRET_PATH.read_text().strip()
    if not s:
        sys.exit(f"fm2k_logs: secret file at {SECRET_PATH} is empty")
    return s


# ---------- HTTP helpers ----------

def _http(method: str, path: str, *, query: dict | None = None,
          timeout: int = 30) -> Any:
    url = f"{_hub_base()}{path}"
    if query:
        url += "?" + urllib.parse.urlencode(query)
    req = urllib.request.Request(url, method=method)
    req.add_header("X-FM2K-Log-Secret", _load_secret())
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            data = resp.read()
    except urllib.error.HTTPError as e:
        sys.exit(f"fm2k_logs: HTTP {e.code} from {url}: "
                 f"{e.read(8192).decode('utf-8', 'replace')}")
    except urllib.error.URLError as e:
        sys.exit(f"fm2k_logs: connection failed to {url}: {e.reason}")
    if not data:
        return None
    try:
        return json.loads(data)
    except json.JSONDecodeError:
        return data.decode("utf-8", "replace")


def _api_recent(limit: int = 50, *, kind: str | None = None,
                version: str | None = None, game_id: str | None = None,
                match_id: str | None = None,
                since: str | None = None) -> list[dict]:
    q: dict[str, str | int] = {"limit": limit}
    if kind: q["kind"] = kind
    if version: q["version"] = version
    if game_id: q["game_id"] = game_id
    if match_id: q["match_id"] = match_id
    if since: q["since"] = since
    rows = _http("GET", "/recent", query=q)
    return rows if isinstance(rows, list) else []


def _api_by_session(session_id: str) -> list[dict]:
    rows = _http("GET", f"/by_session/{urllib.parse.quote(session_id)}")
    return rows if isinstance(rows, list) else []


def _api_by_match(match_id: str) -> list[dict]:
    rows = _http("GET", f"/by_match/{urllib.parse.quote(match_id)}")
    return rows if isinstance(rows, list) else []


def _api_summary() -> dict:
    out = _http("GET", "/summary")
    return out if isinstance(out, dict) else {}


# ---------- bundle pulldown ----------

def _ssh_path_to_local(remote: str, local_root: Path) -> Path:
    # remote looks like /var/lib/fm2k_logs/<date>/<sid>/p<n>/<kind>/<uid>
    rel = remote.removeprefix("/var/lib/fm2k_logs/").lstrip("/")
    return local_root / rel


def _pull(remote_dir: str, local_root: Path) -> Path:
    dst = _ssh_path_to_local(remote_dir, local_root)
    dst.mkdir(parents=True, exist_ok=True)
    # Trailing slash on src copies contents into dst.
    src = f"{SSH_HOST}:{remote_dir}/"
    cmd = ["rsync", "-az", "--quiet", src, str(dst)]
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        sys.exit(f"fm2k_logs: rsync failed: {result.stderr}")
    # Auto-extract any .zip bundles in place. v0.2.42+ hooks ship a
    # single .zip per upload containing Debug.log + desync.log +
    # rngtrace.csv. Without extraction, a follow-up Read of the
    # bundle dir just sees binary. Extract each entry next to the
    # .zip so plain-text reads work directly. Keep the .zip as a
    # backup so re-extraction is idempotent.
    _extract_zips_in_place(dst)
    return dst


def _extract_zips_in_place(bundle_dir: Path) -> None:
    """For every .zip in bundle_dir, extract its entries into
    bundle_dir/ (skipping entries that already exist on disk —
    re-pulls become a no-op rather than overwriting). Bad zips are
    logged but don't fail the pull."""
    import zipfile
    for z in bundle_dir.glob("*.zip"):
        try:
            with zipfile.ZipFile(z) as zf:
                for info in zf.infolist():
                    if info.is_dir():
                        continue
                    # Strip directory components from entry name — the
                    # hook ships basename-only entries but defend against
                    # zip slip from any future bundle source.
                    leaf = Path(info.filename).name
                    if not leaf:
                        continue
                    dst_path = bundle_dir / leaf
                    if dst_path.exists():
                        continue
                    try:
                        dst_path.write_bytes(zf.read(info.filename))
                    except Exception as e:
                        sys.stderr.write(
                            f"fm2k_logs: extract failed "
                            f"{z.name}!{info.filename}: {e}\n")
        except zipfile.BadZipFile as e:
            sys.stderr.write(f"fm2k_logs: bad zip {z}: {e}\n")


# ---------- subcommand: recent ----------

def cmd_recent(args: argparse.Namespace) -> int:
    rows = _api_recent(limit=max(args.limit, 1),
                       kind=args.kind, version=args.version,
                       game_id=args.game, match_id=args.match,
                       since=args.since)
    if args.json:
        print(json.dumps(rows, indent=2))
        return 0
    if not rows:
        print("(no matching uploads)")
        return 0
    header = (f"{'TS':<25} {'KIND':<14} {'V':<10} {'GAME':<24} "
              f"{'P':<2} {'SID':<15} MATCH")
    print(header)
    print("-" * len(header))
    for r in rows:
        print(f"{r.get('ts','?'):<25.25} "
              f"{r.get('kind','?'):<14.14} "
              f"{r.get('client_version','?'):<10.10} "
              f"{r.get('game_id','?'):<24.24} "
              f"{r.get('player_index','?')!s:<2} "
              f"{r.get('session_id','?')!s:<15.15} "
              f"{r.get('match_id') or '-'}")
    return 0


def cmd_summary(args: argparse.Namespace) -> int:
    s = _api_summary()
    if args.json:
        print(json.dumps(s, indent=2))
        return 0
    print(f"Total uploads: {s.get('total_uploads', 0)}")
    print()
    def _section(title: str, key: str, k_field: str, v_field: str = "count"):
        rows = s.get(key, [])
        if not rows:
            return
        print(f"{title}:")
        for r in rows:
            print(f"  {r[k_field]:<32.32} {r[v_field]}")
        print()
    _section("By kind", "by_kind", "kind")
    _section("By version (top 20)", "by_version", "version")
    _section("By game (top 20)", "by_game", "game_id")
    matches = s.get("cross_peer_matches", [])
    if matches:
        print("Cross-peer matches (both P1 and P2 uploaded — best for desync diffs):")
        print(f"  {'MATCH_ID':<36.36} {'PEERS':<6} {'UPLOADS':<8} LATEST")
        for r in matches:
            print(f"  {r['match_id']:<36.36} {r['peer_count']:<6} "
                  f"{r['upload_count']:<8} {r['latest']}")
        print()
    last = s.get("last_upload_per_kind", [])
    if last:
        print("Latest upload per kind:")
        for r in last:
            print(f"  {r['kind']:<14.14} {r['ts']}")
    return 0


# ---------- subcommand: pull ----------

def cmd_pull(args: argparse.Namespace) -> int:
    rows = _api_by_session(args.session_id)
    if not rows:
        sys.exit(f"fm2k_logs: no uploads for session {args.session_id}")
    local_root = Path(args.into).resolve() if args.into else CACHE_DIR
    local_root.mkdir(parents=True, exist_ok=True)
    pulled = []
    for r in rows:
        remote = r.get("stored_path")
        if not remote:
            continue
        dst = _pull(remote, local_root)
        pulled.append(str(dst.resolve()))
        print(dst.resolve())
    if not pulled and not args.into:
        sys.exit("fm2k_logs: nothing pulled (index empty for that session)")
    return 0


# ---------- subcommand: pull-recent ----------

def cmd_pull_recent(args: argparse.Namespace) -> int:
    rows = _api_recent(limit=max(args.limit, 1),
                       kind=args.kind, version=args.version,
                       game_id=args.game, since=args.since)
    if not rows:
        print("(no matching uploads to pull)")
        return 0
    local_root = Path(args.into).resolve() if args.into else CACHE_DIR
    local_root.mkdir(parents=True, exist_ok=True)
    for r in rows:
        remote = r.get("stored_path")
        if not remote:
            continue
        dst = _pull(remote, local_root)
        print(f"{r.get('ts','?'):<25.25} {r.get('kind','?'):<14.14} "
              f"P{r.get('player_index','?')} → {dst.resolve()}")
    return 0


def cmd_pull_match(args: argparse.Namespace) -> int:
    rows = _api_by_match(args.match_id)
    if not rows:
        sys.exit(f"fm2k_logs: no uploads for match {args.match_id}")
    local_root = Path(args.into).resolve() if args.into else CACHE_DIR
    local_root.mkdir(parents=True, exist_ok=True)
    for r in rows:
        remote = r.get("stored_path")
        if not remote:
            continue
        dst = _pull(remote, local_root)
        print(f"{r.get('ts','?'):<25.25} {r.get('kind','?'):<14.14} "
              f"P{r.get('player_index','?')} sid={r.get('session_id')} "
              f"→ {dst.resolve()}")
    return 0


# ---------- subcommand: show ----------

def _print_text_tail(label: str, text: str, tail: int) -> None:
    """Print `text` with optional tail-truncation. Shared by raw-log
    files and zip-extracted entries so output formatting is uniform."""
    print(f"\n--- {label} ---")
    lines = text.splitlines()
    if tail and len(lines) > tail:
        elided = len(lines) - tail
        print(f"[…elided {elided} lines from head, last {tail} kept…]")
        lines = lines[-tail:]
    for ln in lines:
        print(ln)


def _print_bundles(rows: list[dict], tail: int) -> None:
    """Pull each bundle locally and print its meta + log tails to stdout
    in a stable format for LLM consumption.

    v0.2.42+ bundles arrive as a single .zip (Debug.log + desync.log +
    rngtrace.csv inside); `_pull` extracts in place so we just glob
    the resulting flat files and skip the .zip itself."""
    local_root = CACHE_DIR
    local_root.mkdir(parents=True, exist_ok=True)
    for r in rows:
        remote = r.get("stored_path")
        if not remote:
            continue
        dst = _pull(remote, local_root)
        print()
        print("=" * 78)
        print(f"BUNDLE  {dst}")
        print(f"  kind={r.get('kind')} player={r.get('player_index')} "
              f"frame={r.get('frame')} ts={r.get('ts')} "
              f"sid={r.get('session_id')} match={r.get('match_id') or '-'}")
        print("=" * 78)

        meta = dst / "meta.json"
        if meta.exists():
            print("--- meta.json ---")
            try:
                m = json.loads(meta.read_text())
                print(json.dumps(m, indent=2, sort_keys=True))
            except json.JSONDecodeError:
                print(meta.read_text())

        for f in sorted(dst.glob("*")):
            if f.name == "meta.json" or f.is_dir():
                continue
            # Skip the .zip — its contents are already extracted as
            # sibling files by _pull.
            if f.suffix.lower() == ".zip":
                continue
            try:
                text = f.read_text(errors="replace")
            except Exception as e:
                print(f"\n--- {f.name} ---")
                print(f"(read failed: {e})")
                continue
            _print_text_tail(f.name, text, tail)


def cmd_show(args: argparse.Namespace) -> int:
    rows = _api_by_session(args.session_id)
    if not rows:
        sys.exit(f"fm2k_logs: no uploads for session {args.session_id}")
    if args.player is not None:
        rows = [r for r in rows if str(r.get("player_index")) == str(args.player)]
    if args.kind:
        rows = [r for r in rows if r.get("kind") == args.kind]
    if not rows:
        sys.exit("fm2k_logs: no matching upload after filters")
    _print_bundles(rows, args.tail)
    return 0


def cmd_show_match(args: argparse.Namespace) -> int:
    rows = _api_by_match(args.match_id)
    if not rows:
        sys.exit(f"fm2k_logs: no uploads for match {args.match_id}")
    # Sort so P1 always prints before P2, then by timestamp within each
    # peer. Makes side-by-side comparison readable.
    rows.sort(key=lambda r: (r.get("player_index", 0), r.get("ts", "")))
    _print_bundles(rows, args.tail)
    return 0


# ---------- subcommand: tail ----------

def cmd_tail(args: argparse.Namespace) -> int:
    seen: set[str] = set()
    # Prime with current state so we don't replay history.
    for r in _api_recent(limit=50):
        seen.add(r.get("stored_path", ""))
    print(f"tailing {_hub_base()}/recent every {args.interval}s (Ctrl-C to stop)")
    try:
        while True:
            rows = _api_recent(limit=50)
            new_rows = [r for r in rows if r.get("stored_path") not in seen]
            for r in reversed(new_rows):  # show oldest-first
                print(f"{r.get('ts','?'):<25.25} {r.get('kind','?'):<14.14} "
                      f"v={r.get('client_version','?')} "
                      f"P{r.get('player_index','?')} sid={r.get('session_id')} "
                      f"→ {r.get('stored_path')}")
                seen.add(r.get("stored_path", ""))
            time.sleep(args.interval)
    except KeyboardInterrupt:
        return 0


# ---------- subcommand: open ----------

def cmd_open(args: argparse.Namespace) -> int:
    local_root = CACHE_DIR
    local_root.mkdir(parents=True, exist_ok=True)
    dst = _pull(args.stored_path, local_root)
    print(dst.resolve())
    return 0


# ---------- subcommand: parity-diff ----------
#
# HOST-FP and SPEC-FP fingerprint lines in the hook log have the form:
#
#   [12:42:03.653] [P1] [INFO] [HOST-FP] bf=30 rng=0x12345678 buf=30
#       p1_hp=1 p2_hp=1 timer=1
#       p1_pos=(32751800,60293120) p2_pos=(50594116,60293120)
#       p1_script=31 p2_script=31 p1_in=0x000 p2_in=0x000
#
#   [12:42:18.488] [P3] [INFO] [SPEC-FP] bf=30 (pop=1137) rng=0x12345678 ...
#       (same fields after rng, plus catchup=N at end)
#
# We extract by `bf` (battle-frame counter, host-authoritative), then
# compare host vs spec value-by-value. First divergence + diverging
# field is what we want to surface.

import re as _re_pd

_FP_FIELD_RE = _re_pd.compile(
    r"bf=(?P<bf>\d+)"
    r"(?:\s+\(pop=\d+\))?"
    r"\s+rng=(?P<rng>0x[0-9A-Fa-f]+)"
    r"\s+buf=(?P<buf>-?\d+)"
    r"\s+p1_hp=(?P<p1_hp>-?\d+)"
    r"\s+p2_hp=(?P<p2_hp>-?\d+)"
    r"\s+timer=(?P<timer>-?\d+)"
    r"\s+p1_pos=\((?P<p1_pos>-?\d+,-?\d+)\)"
    r"\s+p2_pos=\((?P<p2_pos>-?\d+,-?\d+)\)"
    r"\s+p1_script=(?P<p1_script>-?\d+)"
    r"\s+p2_script=(?P<p2_script>-?\d+)"
    r"\s+p1_in=(?P<p1_in>0x[0-9A-Fa-f]+)"
    r"\s+p2_in=(?P<p2_in>0x[0-9A-Fa-f]+)"
)

# Order matters: we report the FIRST diverging field at each frame.
# Engine-state fields come before input fields because state divergence
# is what causes desyncs; input mismatches usually trace back to state
# divergence one frame earlier.
_FP_FIELDS_ORDERED = [
    "rng", "p1_hp", "p2_hp", "timer",
    "p1_pos", "p2_pos", "p1_script", "p2_script",
    "p1_in", "p2_in", "buf",
]


def _parse_fp_lines(path: Path, marker: str) -> dict[int, dict[str, str]]:
    """Returns {bf: {field: value}} for every {marker}-FP line in path."""
    out: dict[int, dict[str, str]] = {}
    for raw in path.read_text(errors="replace").splitlines():
        if f"[{marker}-FP]" not in raw:
            continue
        m = _FP_FIELD_RE.search(raw)
        if not m:
            continue
        d = m.groupdict()
        bf = int(d.pop("bf"))
        # Keep all fields as strings — we compare for equality, not
        # ordering, and stringly-typed avoids hex/decimal hassles.
        out[bf] = d
    return out


def _bundle_for_player(local_root: Path, rows: list[dict],
                       player_index: int) -> Path | None:
    """Pull the bundle for player_index from match rows and return the
    Debug.log path inside it."""
    for r in rows:
        if r.get("player_index") != player_index:
            continue
        remote = r.get("stored_path")
        if not remote:
            continue
        dst = _pull(remote, local_root)
        # The debug log filename inside the bundle is the original
        # client-side name (FM2K_P{N+1}_Debug.log).
        for cand in dst.glob("FM2K_P*_Debug.log"):
            return cand
    return None


def cmd_parity_diff(args: argparse.Namespace) -> int:
    if args.host_log and args.spec_log:
        host_log = Path(args.host_log)
        spec_log = Path(args.spec_log)
        if not host_log.is_file() or not spec_log.is_file():
            sys.exit(f"fm2k_logs: --host-log / --spec-log file missing")
    elif args.match_id:
        rows = _api_by_match(args.match_id)
        if not rows:
            sys.exit(f"fm2k_logs: no uploads for match {args.match_id}")
        local_root = CACHE_DIR
        local_root.mkdir(parents=True, exist_ok=True)
        host_log = _bundle_for_player(local_root, rows, 0)
        spec_log = _bundle_for_player(local_root, rows, 1)
        # NOTE: convention here is player_index=0 = host, =1 = client/spec.
        # If both peers crashed/desynced and we have THREE uploads (P1, P2,
        # P3), the spec-side upload uses its own player_index — typically
        # the hook side stamps it. May need a --spec-player flag later.
        if not host_log or not spec_log:
            sys.exit(f"fm2k_logs: couldn't find both peers' Debug.log "
                     f"in match {args.match_id} bundles")
    else:
        sys.exit("fm2k_logs: parity-diff needs either <match_id> "
                 "or --host-log + --spec-log")

    host_fp = _parse_fp_lines(host_log, "HOST")
    spec_fp = _parse_fp_lines(spec_log, "SPEC")

    if not host_fp:
        sys.exit(f"fm2k_logs: no HOST-FP lines in {host_log}")
    if not spec_fp:
        sys.exit(f"fm2k_logs: no SPEC-FP lines in {spec_log}")

    common = sorted(set(host_fp) & set(spec_fp))
    host_only = sorted(set(host_fp) - set(spec_fp))
    spec_only = sorted(set(spec_fp) - set(host_fp))

    print(f"host log: {host_log}")
    print(f"spec log: {spec_log}")
    print(f"host frames: {len(host_fp)}  spec frames: {len(spec_fp)}  "
          f"common: {len(common)}")
    if host_only:
        print(f"frames only in host: {len(host_only)}  "
              f"(first={host_only[0]}, last={host_only[-1]})")
    if spec_only:
        print(f"frames only in spec: {len(spec_only)}  "
              f"(first={spec_only[0]}, last={spec_only[-1]})")
    print()

    first_diverge_bf: int | None = None
    first_diverge_field: str | None = None
    diverge_count = 0

    for bf in common:
        h = host_fp[bf]
        s = spec_fp[bf]
        for field in _FP_FIELDS_ORDERED:
            if h.get(field) != s.get(field):
                if first_diverge_bf is None:
                    first_diverge_bf = bf
                    first_diverge_field = field
                diverge_count += 1
                break

    if first_diverge_bf is None:
        print("✓ NO DIVERGENCE — host and spec agree on every shared "
              "fingerprint frame.")
        return 0

    print(f"✗ FIRST DIVERGENCE  bf={first_diverge_bf}  field={first_diverge_field}")
    h = host_fp[first_diverge_bf]
    s = spec_fp[first_diverge_bf]
    print(f"  host: {first_diverge_field}={h.get(first_diverge_field)}")
    print(f"  spec: {first_diverge_field}={s.get(first_diverge_field)}")
    print()
    print(f"diverged at {diverge_count}/{len(common)} fingerprint frames "
          f"({100.0*diverge_count/max(1,len(common)):.1f}%)")
    print()

    # Context window: show -N..+N frames around the divergence side-by-side.
    win = max(0, args.context)
    if win > 0:
        i0 = max(0, common.index(first_diverge_bf) - win)
        i1 = min(len(common), common.index(first_diverge_bf) + win + 1)
        print(f"context  bf -{win} .. bf +{win}:")
        print(f"{'bf':>6}  {'side':<4}  rng         hp(P1/P2)  "
              f"timer  pos(P1)              pos(P2)              "
              f"script(P1/P2)")
        for j in range(i0, i1):
            bf = common[j]
            for label, fp in (("HOST", host_fp[bf]), ("SPEC", spec_fp[bf])):
                marker = "*" if bf == first_diverge_bf else " "
                print(f"{marker}{bf:>5}  {label:<4}  {fp['rng']:<10}  "
                      f"{fp['p1_hp']:>4}/{fp['p2_hp']:<4} "
                      f"  {fp['timer']:>3}    "
                      f"({fp['p1_pos']:<18})  "
                      f"({fp['p2_pos']:<18})  "
                      f"{fp['p1_script']:>3}/{fp['p2_script']:<3}")
            print()

    return 1  # non-zero exit = "divergence found" for shell scripting


# ---------- entry point ----------

def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="fm2k_logs",
        description="Crash/desync log pull from the FM2K hub. "
                    "Bundles cached under tools/.fm2k_logs/ for follow-up "
                    "inspection.")
    sub = parser.add_subparsers(dest="cmd", required=True)

    p_summary = sub.add_parser("summary",
                               help="at-a-glance counts by kind/version/game")
    p_summary.add_argument("--json", action="store_true",
                           help="machine-readable JSON output")
    p_summary.set_defaults(func=cmd_summary)

    p_recent = sub.add_parser("recent", help="list recent uploads")
    p_recent.add_argument("--kind", help="filter: desync|crash|exception_recovered|probe")
    p_recent.add_argument("--version", help="filter by client_version")
    p_recent.add_argument("--game", help="filter by game_id")
    p_recent.add_argument("--match", help="filter by match_id")
    p_recent.add_argument("--since", help="ISO timestamp lower bound (e.g. 2026-05-13)")
    p_recent.add_argument("--limit", type=int, default=20)
    p_recent.add_argument("--json", action="store_true",
                          help="machine-readable JSON output")
    p_recent.set_defaults(func=cmd_recent)

    p_pull = sub.add_parser("pull", help="pull all bundles for a session")
    p_pull.add_argument("session_id")
    p_pull.add_argument("--into", help="local dir (default: tools/.fm2k_logs/)")
    p_pull.set_defaults(func=cmd_pull)

    p_pm = sub.add_parser("pull-match",
                          help="pull both peers' bundles for a match")
    p_pm.add_argument("match_id")
    p_pm.add_argument("--into")
    p_pm.set_defaults(func=cmd_pull_match)

    p_pr = sub.add_parser("pull-recent",
                          help="pull all bundles matching filters")
    p_pr.add_argument("--kind")
    p_pr.add_argument("--version")
    p_pr.add_argument("--game")
    p_pr.add_argument("--since")
    p_pr.add_argument("--limit", type=int, default=10)
    p_pr.add_argument("--into")
    p_pr.set_defaults(func=cmd_pull_recent)

    p_show = sub.add_parser("show",
                            help="pull + print meta + log content for a session")
    p_show.add_argument("session_id")
    p_show.add_argument("--player", type=int, choices=[0, 1])
    p_show.add_argument("--kind")
    p_show.add_argument("--tail", type=int, default=200,
                        help="lines per file (0 = full)")
    p_show.set_defaults(func=cmd_show)

    p_sm = sub.add_parser("show-match",
                          help="pull + print meta + log for both peers in a match")
    p_sm.add_argument("match_id")
    p_sm.add_argument("--tail", type=int, default=200,
                      help="lines per file (0 = full)")
    p_sm.set_defaults(func=cmd_show_match)

    p_tail = sub.add_parser("tail", help="long-poll new uploads")
    p_tail.add_argument("--interval", type=int, default=10)
    p_tail.set_defaults(func=cmd_tail)

    p_open = sub.add_parser("open",
                            help="pull a single bundle by stored_path")
    p_open.add_argument("stored_path")
    p_open.set_defaults(func=cmd_open)

    p_pd = sub.add_parser("parity-diff",
                          help="diff HOST-FP vs SPEC-FP fingerprints to "
                               "find spectator divergence frame + field")
    p_pd.add_argument("match_id", nargs="?",
                      help="match_id to pull both peers from the hub "
                           "(omit if using --host-log + --spec-log)")
    p_pd.add_argument("--host-log",
                      help="local path to host's Debug.log "
                           "(skips hub pull)")
    p_pd.add_argument("--spec-log",
                      help="local path to spectator's Debug.log "
                           "(skips hub pull)")
    p_pd.add_argument("--context", type=int, default=2,
                      help="frames of context around the divergence "
                           "to print side-by-side (default 2)")
    p_pd.set_defaults(func=cmd_parity_diff)

    args = parser.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
