#!/usr/bin/env python3
"""Triage freem-pulled exes that didn't match any known FM2K stub.

The bare-exe pulls (CalendarRave.exe, brkarspd35.exe, p2dx.exe,
ShadowArts.exe, wh160830.exe) are 25-81 MB single-file blobs — way
too large to be the 1.2 MB FM2K stub. They're almost certainly one of:

  - Self-extracting archive (7z/RAR SFX, zip SFX)
  - Inno Setup installer
  - NSIS installer
  - Custom FM2K bundle (engine + data in one file)

This script attempts to identify what each is by:
  1. file(1) magic detection
  2. 7z l         — succeeds for SFX archives (zip/7z/rar SFX)
  3. innounp -v   — succeeds for Inno Setup installers
  4. Search for known marker strings (Inno Setup, Nullsoft, etc.)

Outputs a triage row per exe. Updates the per-game JSON's
`extracted.exes[*].guess` field with the inferred type.
"""

import json
import re
import subprocess
import sys
from pathlib import Path

REPO       = Path(__file__).resolve().parents[1]
GAMES_DIR  = REPO / "data" / "freem" / "games"
EXTRACTED  = REPO / "data" / "freem" / "extracted"


# Known marker strings — quick PE-internal signatures.
MARKERS = [
    (b"Inno Setup",                "inno-setup"),
    (b"Nullsoft.NSIS",             "nsis"),
    (b"NullsoftInst",              "nsis"),
    (b"7z\xbc\xaf'\x1c",           "7z-sfx"),
    (b"WinRAR SFX",                "rar-sfx"),
    (b"PK\x03\x04",                "zip-sfx"),
    (b"FM2K_Version",              "fm2k-stub"),
    (b"2DFM",                      "fm2k-related"),
]


def file_magic(p: Path) -> str:
    try:
        out = subprocess.run(
            ["file", "-b", str(p)],
            capture_output=True, timeout=10, text=True
        ).stdout.strip()
        return out
    except Exception as e:
        return f"file(1) failed: {e}"


def scan_markers(p: Path) -> list[str]:
    """Slurp up to 32MB of the exe and look for marker bytes."""
    found = []
    try:
        with p.open("rb") as f:
            data = f.read(32 * 1024 * 1024)
    except Exception:
        return []
    for marker, label in MARKERS:
        if marker in data:
            found.append(label)
    return found


def try_7z_list(p: Path) -> tuple[bool, str]:
    """Try `7z l` on the exe. Returns (looks_like_sfx, sample_listing)."""
    try:
        r = subprocess.run(
            ["7z", "l", "-slt", str(p)],
            capture_output=True, timeout=20, text=True,
            errors="replace"
        )
    except FileNotFoundError:
        return False, "7z not installed"
    except subprocess.TimeoutExpired:
        return False, "7z timed out"
    if r.returncode == 0 and "Path = " in r.stdout:
        # Pull a few sample entries
        paths = re.findall(r"Path = (.+)", r.stdout)
        sample = paths[:5]
        return True, " · ".join(sample)
    return False, (r.stderr or r.stdout)[:200]


def inspect_one(exe_path: Path) -> dict:
    if not exe_path.exists():
        return {"error": "missing"}

    out = {
        "size_bytes":  exe_path.stat().st_size,
        "file_magic":  file_magic(exe_path),
        "markers":     scan_markers(exe_path),
    }
    sfx_ok, sfx_msg = try_7z_list(exe_path)
    out["sfx_listable"] = sfx_ok
    out["sfx_msg"]     = sfx_msg

    # Classify
    if "inno-setup" in out["markers"]:
        out["guess"] = "inno-setup-installer"
    elif "nsis" in out["markers"]:
        out["guess"] = "nsis-installer"
    elif "7z-sfx" in out["markers"] and sfx_ok:
        out["guess"] = "7z-sfx-archive"
    elif "rar-sfx" in out["markers"]:
        out["guess"] = "rar-sfx-archive"
    elif "zip-sfx" in out["markers"] and sfx_ok:
        out["guess"] = "zip-sfx-archive"
    elif "fm2k-stub" in out["markers"] or "fm2k-related" in out["markers"]:
        out["guess"] = "fm2k-bundle-or-custom"
    elif sfx_ok:
        out["guess"] = "generic-sfx (7z recognized)"
    else:
        out["guess"] = "unknown"
    return out


def main() -> int:
    n_total = 0
    n_classified = 0
    for rec_path in sorted(GAMES_DIR.glob("*.json")):
        rec = json.loads(rec_path.read_text(encoding="utf-8"))
        if not rec.get("extracted"):
            continue
        ex = rec["extracted"]
        # Only unknown exes (no match_known) need triage.
        unknowns = [e for e in (ex.get("exes") or [])
                    if not e.get("match_known")]
        if not unknowns:
            continue

        gid_root = EXTRACTED / rec["freem_id"]
        changed = False
        for e in unknowns:
            n_total += 1
            full = gid_root / e["path"]
            info = inspect_one(full)
            e["guess"]      = info.get("guess")
            e["file_magic"] = info.get("file_magic", "")[:80]
            e["markers"]    = info.get("markers", [])
            e["sfx_listable"] = info.get("sfx_listable", False)
            changed = True
            if info.get("guess") and info["guess"] != "unknown":
                n_classified += 1
            print(f"  {rec['freem_id']:>6}  "
                  f"{e['xxh64']}  "
                  f"{e['size_bytes']/1e6:>5.1f} MB  "
                  f"→ {info.get('guess','?'):<24s} "
                  f"| {Path(e['path']).name}")
            if info.get("markers"):
                print(f"          markers: {info['markers']}")
            if info.get("sfx_listable"):
                print(f"          contents (sample): {info['sfx_msg'][:120]}")

        if changed:
            rec_path.write_text(json.dumps(rec, ensure_ascii=False, indent=2),
                                encoding="utf-8")

    print(f"\ntotal unknowns triaged: {n_total}")
    print(f"classified (not 'unknown'): {n_classified}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
