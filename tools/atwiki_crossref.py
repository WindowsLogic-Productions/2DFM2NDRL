#!/usr/bin/env python3
"""Phase 5: cross-reference downloaded archives with the launcher's
known-exe registry.

For every per-game record that has at least one successfully pulled
archive, this script:

  1. Unpacks each archive into a temp dir (zip / lzh / rar / 7z).
  2. Walks the unpack for *.exe files.
  3. Computes xxh64 of each exe (matches the launcher's identification
     hash — see FM2K_RollbackClient.cpp `HashFileXXH64`).
  4. Looks each hash up against:
        a) the launcher's compiled-in `kKnownExes` registry,
        b) the user's `data/fm2k_wiki/index.json` (atwiki name match),
        c) the running launcher's `games.cache` binary (xxh64 of any
           game already on disk locally).
  5. Records the match status into the game JSON under "exe_matches"
     so the launcher (or a UI on top of this database) can show
     provenance for any given local install.

Output schema (extends per-game JSON):

  "exe_matches": [
    {
      "filename":         "Foo.exe",
      "xxh64":            "0xabcdef0123456789",
      "size_bytes":       1234567,
      "from_archive":     "Foo_v0.5.zip",
      "match_known":      "WonderfulWorld v0.946" | null,
      "match_local_disk": true | false,
      "engine_guess":     "FM2K" | "FM95" | null
    }
  ]

Note: lzh requires `lha` binary (or `7z` with libpkg-extra). zip is
stdlib. rar requires `unrar`. 7z requires `7z`/`p7zip`. We try in
order, skip silently if a tool is missing — partial coverage is
better than failing the whole run.
"""

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

import xxhash


REPO = Path(__file__).resolve().parents[1]
GAMES_DIR = REPO / "data" / "fm2k_wiki" / "games"
DL_DIR    = REPO / "data" / "fm2k_wiki" / "downloads"
LAUNCHER_CACHE = Path.home() / "AppData" / "Roaming" / "FM2K_Rollback" / "games.cache"

# Mirror of FM2K_RollbackClient.cpp::kKnownExes plus engine-variant
# hashes observed during the user's local discovery scan. These are
# stub-runtime hashes (the FM2K editor produces a small set of
# distinct stub binaries; many games ship the SAME exe + a different
# .kgt). Matching one of these tells us "this game uses engine
# variant X" — game identity comes from the .kgt project name, not
# the exe hash.
KNOWN_EXES: dict[int, tuple[str, str]] = {
    # Curated registry (FM2K_RollbackClient.cpp)
    0x506FF9AB93D15134: ("FM2K", "WonderfulWorld v0.946 stub"),
    0x36358AD6F9EC387B: ("FM95", "Comic Party Wars (CPW)"),
    # Observed during local discovery (one stub serves many games)
    0x840EA8B0623060DF: ("FM2K", "FM2K stub variant A (8 Marbles / Mine / Time&Stopper)"),
    0x52539C48466260F8: ("FM2K", "FM2K stub variant B (HHRTFG / vanpri / pkmnfg)"),
    0x6D84C30B99E7DCBD: ("FM2K", "FM2K stub variant — AOB"),
    0xB5A86B539C7B3030: ("FM2K", "FM2K stub variant — pkmncc"),
    0xB285B081D45A989F: ("FM2K", "FM2K stub variant — Crimson Alive"),
    0xCDE25D3498EDA7E8: ("FM2K", "FM2K stub variant — ZANGEF"),
    0xD57B77E97F565FF1: ("FM2K", "FM2K stub variant — SCWU"),
    0x82DA507402BDC62B: ("FM2K", "FM2K stub variant — X_Theateru"),
}


def hash_file(path: Path) -> int:
    h = xxhash.xxh64(seed=0)
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.intdigest()


def unpack_archive(archive: Path, dest: Path) -> bool:
    """Try to extract `archive` into `dest`. Returns True on any
    success, False if no available tool can crack it."""
    suffix = archive.suffix.lower()
    name = archive.name.lower()

    if suffix == ".zip" or name.endswith(".zip"):
        try:
            with zipfile.ZipFile(archive) as z:
                z.extractall(dest)
            return True
        except (zipfile.BadZipFile, OSError):
            return False

    if suffix == ".lzh" or name.endswith(".lzh"):
        # `7z x` can read lzh on most Linux p7zip-full builds; lha
        # binary is the canonical fallback.
        for cmd in (
            ["7z", "x", "-y", f"-o{dest}", str(archive)],
            ["lha", "xw=" + str(dest), str(archive)],
        ):
            if shutil.which(cmd[0]) and subprocess.run(
                    cmd, capture_output=True).returncode == 0:
                return True
        return False

    if suffix == ".rar" or name.endswith(".rar"):
        for cmd in (
            ["unrar", "x", "-o+", str(archive), str(dest) + "/"],
            ["7z", "x", "-y", f"-o{dest}", str(archive)],
        ):
            if shutil.which(cmd[0]) and subprocess.run(
                    cmd, capture_output=True).returncode == 0:
                return True
        return False

    if suffix == ".7z" or name.endswith((".7z", ".tar.gz", ".tgz")):
        if shutil.which("7z") and subprocess.run(
                ["7z", "x", "-y", f"-o{dest}", str(archive)],
                capture_output=True).returncode == 0:
            return True
        return False

    return False


def load_local_disk_hashes() -> set[int]:
    """If the launcher has a games.cache on disk, parse out every
    cached exe_xxh64. The cache format is binary, magic 'FM2K' v2 —
    see FM2K_RollbackClient.cpp::LoadGameCacheMap. We only need the
    hash field per entry, not the full record."""
    if not LAUNCHER_CACHE.exists():
        return set()
    try:
        data = LAUNCHER_CACHE.read_bytes()
    except OSError:
        return set()
    if data[:4] != b"FM2K":
        return set()
    import struct
    o = 4
    version = struct.unpack_from("<I", data, o)[0]; o += 4
    if version != 2:
        return set()
    n = struct.unpack_from("<I", data, o)[0]; o += 4
    hashes: set[int] = set()
    for _ in range(n):
        # Walk the per-entry layout just enough to extract exe_xxh64.
        def read_str() -> int:
            nonlocal o
            slen = struct.unpack_from("<I", data, o)[0]; o += 4 + slen
            return slen
        read_str()                        # exe_path
        read_str()                        # dll_path
        o += 8 + 8                        # exe_size, exe_mtime
        h = struct.unpack_from("<Q", data, o)[0]; o += 8
        hashes.add(h)
        o += 4                            # engine
        read_str()                        # clean_label
        read_str()                        # packer_label
        o += 1 + 1 + 8 + 8 + 1            # is_clean, kgt_present, kgt_size, kgt_mtime, kgt_valid
        read_str()                        # kgt_project_name
        for _list in range(3):
            cnt = struct.unpack_from("<I", data, o)[0]; o += 4
            for _i in range(cnt):
                read_str()
    return hashes


def crossref_one(game_path: Path, local_hashes: set[int]) -> int:
    record = json.loads(game_path.read_text(encoding="utf-8"))
    downloads = [d for d in record.get("downloads", [])
                 if d.get("local_path")]
    if not downloads:
        return 0
    record.setdefault("exe_matches", [])
    seen_hashes = {m["xxh64"] for m in record["exe_matches"]}
    n_added = 0

    for dl in downloads:
        archive = REPO / dl["local_path"]
        if not archive.exists():
            continue

        # Bare .exe shipped as the download itself — common on freett /
        # Geocities-era doujin pages where authors single-file'd the game
        # (or wrapped it as a self-extractor). Hash directly; an SFX
        # might also be unpackable, but the unwrapped exe inside ships
        # the same bytes anyway.
        if archive.suffix.lower() == ".exe":
            try:
                xh = hash_file(archive)
            except OSError:
                continue
            xh_hex = f"0x{xh:016X}"
            if xh_hex not in seen_hashes:
                seen_hashes.add(xh_hex)
                known = KNOWN_EXES.get(xh)
                record["exe_matches"].append({
                    "filename":          archive.name,
                    "xxh64":             xh_hex,
                    "size_bytes":        archive.stat().st_size,
                    "from_archive":      archive.name,   # self
                    "match_known":       known[1] if known else None,
                    "match_local_disk":  xh in local_hashes,
                    "engine_guess":      known[0] if known else record.get("engine"),
                })
                n_added += 1
            continue   # skip unpack for bare exe

        with tempfile.TemporaryDirectory(prefix="fm2k_unpack_") as td:
            dest = Path(td)
            if not unpack_archive(archive, dest):
                continue
            for exe in dest.rglob("*.[Ee][Xx][Ee]"):
                try:
                    xh = hash_file(exe)
                except OSError:
                    continue
                xh_hex = f"0x{xh:016X}"
                if xh_hex in seen_hashes:
                    continue
                seen_hashes.add(xh_hex)
                known = KNOWN_EXES.get(xh)
                record["exe_matches"].append({
                    "filename":          exe.name,
                    "xxh64":             xh_hex,
                    "size_bytes":        exe.stat().st_size,
                    "from_archive":      archive.name,
                    "match_known":       known[1] if known else None,
                    "match_local_disk":  xh in local_hashes,
                    "engine_guess":      known[0] if known else record.get("engine"),
                })
                n_added += 1

    if n_added:
        game_path.write_text(json.dumps(record, ensure_ascii=False, indent=2),
                             encoding="utf-8")
    return n_added


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--ids", default="",
                    help="comma-separated atwiki IDs to process (overrides --limit)")
    args = ap.parse_args()

    if not GAMES_DIR.exists():
        print(f"missing {GAMES_DIR}", file=sys.stderr)
        return 1

    local_hashes = load_local_disk_hashes()
    if local_hashes:
        print(f"loaded {len(local_hashes)} hashes from launcher games.cache")

    if args.ids:
        wanted = {s.strip() for s in args.ids.split(",") if s.strip()}
        paths = [GAMES_DIR / f"{i}.json" for i in wanted
                 if (GAMES_DIR / f"{i}.json").exists()]
    else:
        paths = sorted(GAMES_DIR.glob("*.json"))
        if args.limit:
            paths = paths[: args.limit]

    n_total = 0
    n_known = 0
    n_local = 0
    for i, p in enumerate(paths, 1):
        added = crossref_one(p, local_hashes)
        n_total += added
        # Re-read for summary stats.
        record = json.loads(p.read_text(encoding="utf-8"))
        for m in record.get("exe_matches", []):
            if m["match_known"]:
                n_known += 1
            if m["match_local_disk"]:
                n_local += 1
        print(f"[{i:3d}/{len(paths)}] {p.stem:>5}  +{added} matches",
              flush=True)

    print(f"\ntotal exe matches: {n_total}")
    print(f"  matched against known-clean registry: {n_known}")
    print(f"  matched against local disk install:   {n_local}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
