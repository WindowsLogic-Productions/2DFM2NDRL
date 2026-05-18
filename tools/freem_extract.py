#!/usr/bin/env python3
"""Extract freem pulls with proper CP932 (Shift-JIS) filename handling.

Japanese doujin game ZIPs almost always store entry filenames as
Shift-JIS bytes WITHOUT setting the ZIP "UTF-8 names" general-purpose
bit. Python's stdlib zipfile interprets such bytes as CP437 (DOS-era
default) by default, which produces mojibake like "âJâìâôâ_ü[âîâCâu"
for what should be "カレンダーレイブ".

Python 3.11 added `metadata_encoding` to ZipFile so the right move
is just to pass `metadata_encoding="cp932"`. We fall back to manual
byte-roundtrip (`name.encode('cp437').decode('cp932')`) for older
runtimes.

Bare-EXE pulls (no extraction needed) are just copied/linked into the
extracted/ tree so every game has a uniform path layout downstream.

Output:
  data/freem/extracted/<freem_id>/...   — extracted tree per game
  data/freem/games/<freem_id>.json      — updated with `extracted`:
    {
      "kind":         "zip" | "exe" | ...,
      "n_files":      int,
      "n_bytes":      int,
      "exes":         ["path/to/Game.exe", ...],
      "kgts":         ["path/to/Game.kgt", ...],     # FM project files
      "extracted_at": unix_ts,
      "had_mojibake": bool,   # true if we hit any name we had to fix
    }

Idempotent: skip games where `extracted` is already set (use --force).
"""
from __future__ import annotations

import argparse
import json
import shutil
import sys
import time
import zipfile
from pathlib import Path

REPO         = Path(__file__).resolve().parents[1]
DOWNLOADS    = REPO / "data" / "freem" / "downloads"
EXTRACTED    = REPO / "data" / "freem" / "extracted"
GAMES_DIR    = REPO / "data" / "freem" / "games"


def safe_join(base: Path, name: str) -> Path:
    """Resolve `name` against `base` and reject anything that escapes
    via ../. Replaces backslashes with forward slashes (Windows
    archives sometimes use backslash separators)."""
    name = name.replace("\\", "/")
    # Drop any leading slash so abs paths don't escape base.
    name = name.lstrip("/")
    out = (base / name).resolve()
    if not str(out).startswith(str(base.resolve())):
        raise ValueError(f"unsafe path escape: {name!r}")
    return out


def decode_cp932(raw_name: str) -> tuple[str, bool]:
    """For when ZipFile's metadata_encoding wasn't used (older Python)
    or didn't fire (UTF-8 bit was set falsely). Returns (decoded, had_mojibake)."""
    # If the name is already pure ASCII it's almost certainly correct
    # as-is (filenames like 'Setup.exe' don't need re-decoding).
    if all(ord(c) < 128 for c in raw_name):
        return raw_name, False
    try:
        # CP437 is what stdlib used; reverse it and re-decode as CP932.
        decoded = raw_name.encode("cp437").decode("cp932")
        return decoded, True
    except (UnicodeEncodeError, UnicodeDecodeError):
        return raw_name, False


def extract_zip(zip_path: Path, target: Path) -> dict:
    """Extract a ZIP with CP932 filename handling. Returns a dict of
    extraction metadata."""
    target.mkdir(parents=True, exist_ok=True)
    # Resolve symlinks once so `out.relative_to(target)` works when
    # target is a symlink (it is — data/freem/extracted → D:/...).
    target = target.resolve()
    had_mojibake = False
    n_files = 0
    n_bytes = 0
    exes: list[str] = []
    kgts: list[str] = []

    # Try the 3.11+ metadata_encoding kwarg first. Drop to a manual
    # roundtrip if Python rejects it (3.10 and earlier).
    try:
        z = zipfile.ZipFile(zip_path, metadata_encoding="cp932")
        used_meta = True
    except TypeError:
        z = zipfile.ZipFile(zip_path)
        used_meta = False

    with z:
        for info in z.infolist():
            name = info.filename
            if not used_meta:
                name, fixed = decode_cp932(name)
                if fixed:
                    had_mojibake = True
            if info.is_dir():
                safe_join(target, name).mkdir(parents=True, exist_ok=True)
                continue
            out = safe_join(target, name)
            out.parent.mkdir(parents=True, exist_ok=True)
            with z.open(info) as src, out.open("wb") as dst:
                shutil.copyfileobj(src, dst)
            n_files += 1
            n_bytes += info.file_size
            lower = out.name.lower()
            if lower.endswith(".exe"):
                exes.append(str(out.relative_to(target)))
            elif lower.endswith(".kgt"):
                kgts.append(str(out.relative_to(target)))

    return {
        "kind":         "zip",
        "n_files":      n_files,
        "n_bytes":      n_bytes,
        "exes":         exes,
        "kgts":         kgts,
        "had_mojibake": had_mojibake,
        "extracted_at": int(time.time()),
    }


def stage_bare_exe(exe_path: Path, target: Path) -> dict:
    """Bare .exe pull: no archive to extract. Hard-link the file into
    the extracted/ tree so callers can treat all games uniformly."""
    target.mkdir(parents=True, exist_ok=True)
    dst = target / exe_path.name
    if dst.exists():
        dst.unlink()
    try:
        # Hard link saves disk; falls back to copy across filesystems.
        dst.hardlink_to(exe_path)
    except OSError:
        shutil.copy2(exe_path, dst)
    return {
        "kind":         "exe",
        "n_files":      1,
        "n_bytes":      dst.stat().st_size,
        "exes":         [exe_path.name],
        "kgts":         [],
        "had_mojibake": False,
        "extracted_at": int(time.time()),
    }


def extract_one(rec: dict, force: bool = False) -> dict | None:
    gid = rec["freem_id"]
    if rec.get("extracted") and not force:
        return None
    pulled = rec.get("pulled")
    if not pulled:
        return {"skipped": True, "reason": "no pulled bytes"}

    src = REPO / pulled["local_path"]
    if not src.exists():
        return {"error": f"missing local file {src}"}

    target = EXTRACTED / gid
    if target.exists() and force:
        shutil.rmtree(target)

    kind = pulled.get("kind")
    if kind == "zip":
        meta = extract_zip(src, target)
    elif kind == "exe":
        meta = stage_bare_exe(src, target)
    else:
        # lzh / rar / 7z — try 7z command-line as a fallback. For now
        # just flag and skip; these are rare in the freem set.
        return {"error": f"unsupported archive kind: {kind}"}
    return meta


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--ids", default="",
                    help="comma-separated freem_ids (default: all pulled)")
    ap.add_argument("--force", action="store_true",
                    help="re-extract even if already done")
    args = ap.parse_args()

    if args.ids:
        wanted = {s.strip() for s in args.ids.split(",") if s.strip()}
        paths = [GAMES_DIR / f"{i}.json" for i in wanted
                 if (GAMES_DIR / f"{i}.json").exists()]
    else:
        paths = sorted(GAMES_DIR.glob("*.json"))

    n_extracted = 0
    n_skipped   = 0
    n_failed    = 0
    n_files_total = 0
    n_bytes_total = 0
    for p in paths:
        rec = json.loads(p.read_text(encoding="utf-8"))
        if not rec.get("pulled"):
            continue
        result = extract_one(rec, force=args.force)
        if result is None:
            n_skipped += 1
            continue
        if "error" in result:
            n_failed += 1
            print(f"  FAIL  {rec['freem_id']:>6}  {result['error']}")
            continue
        if result.get("skipped"):
            n_skipped += 1
            continue
        rec["extracted"] = result
        p.write_text(json.dumps(rec, ensure_ascii=False, indent=2),
                     encoding="utf-8")
        n_extracted += 1
        n_files_total += result["n_files"]
        n_bytes_total += result["n_bytes"]
        mojibake_flag = " [mojibake fixed]" if result["had_mojibake"] else ""
        first_exe = result["exes"][0] if result["exes"] else "—"
        print(f"  OK    {rec['freem_id']:>6}  "
              f"{result['n_files']:>4d} files / "
              f"{result['n_bytes']/1e6:>5.1f} MB  "
              f"→ {first_exe[:50]}{mojibake_flag}")

    print(f"\nextracted: {n_extracted} games  "
          f"({n_files_total} files / {n_bytes_total/1e9:.2f} GB)")
    print(f"skipped:   {n_skipped}")
    print(f"failed:    {n_failed}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
