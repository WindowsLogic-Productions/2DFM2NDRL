#!/usr/bin/env python3
"""Scan FM2K game .exe files for known packers / protectors.

A vanilla FM2K game .exe is built by the engine's own compiler and is
small (~250-1500 KB), unprotected, and links its DirectDraw imports
straight against ddraw.dll. Many third-party / commercial-leaning
distributions repacked the engine output with MoleBox / UPX /
Themida / VMProtect / similar to bundle assets or thwart casual RE.
That makes the exe:

  - Bigger (often 5-50 MB) because the original + assets are
    embedded in a self-extracting wrapper.
  - Unhookable in some cases (bound IATs / encrypted sections /
    anti-debug tripwires).
  - Hash-divergent across "supposedly the same" builds because the
    wrapper's pack-time PRNG produces different bytes on each repack.

We can't unpack arbitrarily, but we CAN identify what's packed so
the operator knows which exes need a manual unwrap pass before the
hash check, replay capture, or screenshot pipeline will work.

Detection strategy: read the PE header's section table and match
against a curated list of well-known section-name / signature
strings. Cheap (one stat + one ~512-byte read per file) and
reliable enough that false positives are vanishingly rare.

Usage:
    python3 tools/analyze_packing.py /mnt/d/games/2dfm
    python3 tools/analyze_packing.py /mnt/d/games/2dfm --json > report.json
    python3 tools/analyze_packing.py /mnt/d/games/2dfm --only-packed
"""

from __future__ import annotations

import argparse
import json
import struct
import sys
from dataclasses import dataclass, asdict
from pathlib import Path
from typing import Iterable


# Known packer / protector section-name signatures. Lowercased; we
# match if any section name CONTAINS the substring (some packers
# leave a single recognizable section, others rename multiple).
PACKER_SECTIONS = {
    "UPX":         ("upx0", "upx1", "upx2", "upx!"),
    "MoleBox":     (".mole", "molebox", ".mlb"),
    "ASPack":      (".aspack", ".adata"),
    "ASProtect":   (".asprotect",),
    "Themida":     (".themida", ".winlice"),  # WinLicense shares
    "VMProtect":   (".vmp0", ".vmp1", ".vmp2"),
    "Enigma":      (".enigma1", ".enigma2"),
    "PECompact":   ("pec1", "pec2", ".pec"),
    "Petite":      (".petite",),
    "FSG":         (".fsg",),
    "MEW":         (".mew",),
    "tElock":      (".telock",),
    "PESpin":      (".pespin",),
    "Yoda":        (".yp",),
    "Armadillo":   (".armadillo", "nicode"),
    "ExeStealth":  (".exest", ".exestea"),
    "Krypton":     (".krypton",),
    # FM2K-scene packer. Adds a `.uro\x07` section (the trailing byte
    # is non-printable). Light wrapper — original four standard
    # sections still present, just +4 KB of payload tacked on. Common
    # on _NODEV releases (Cascom Henichi, all the Battle Calling /
    # Breakers / Chaos Striker / etc. drops) and on Maiga858's
    # "Cleaned exe" variants.
    "URO":         (".uro",),
}

# Some packers don't rename sections cleanly; fall back to byte
# signatures hunting in the first 4 KB of the file. Each entry is
# (label, bytes-prefix, search-window). Hits accumulate alongside
# section matches in the report.
BYTE_SIGNATURES = [
    ("MoleBox",   b"MoleBox",       4096),
    ("MoleBox",   b"\x00MoleBox",   4096),
    ("UPX",       b"UPX!",          1024),
    ("UPX",       b"$Info: This file is packed with the UPX",  4096),
    ("Themida",   b"Themida",       4096),
    ("VMProtect", b"VMProtect",     4096),
    ("Enigma",    b"Enigma1",       4096),
]


@dataclass
class ExeReport:
    path: str
    size: int
    pe_valid: bool
    sections: list[str]
    packer: str
    matches: list[str]
    likely_clean: bool


def read_pe_sections(path: Path) -> tuple[bool, list[str]]:
    """Parse just enough of the PE header to extract section names.
    Returns (pe_valid, [section_name_lowercase]). Bails cleanly on
    truncated / non-PE files."""
    try:
        with path.open("rb") as f:
            head = f.read(0x40)
            if len(head) < 0x40 or head[:2] != b"MZ":
                return False, []
            # e_lfanew at offset 0x3C — points to the "PE\0\0" sig.
            (e_lfanew,) = struct.unpack("<I", head[0x3C:0x40])
            f.seek(e_lfanew)
            sig = f.read(4)
            if sig != b"PE\x00\x00":
                return False, []
            # COFF file header — 20 bytes.
            coff = f.read(20)
            if len(coff) < 20:
                return False, []
            num_sections    = struct.unpack("<H", coff[2:4])[0]
            optional_size   = struct.unpack("<H", coff[16:18])[0]
            # Skip the optional header to land at the section table.
            f.seek(e_lfanew + 4 + 20 + optional_size)
            names: list[str] = []
            for _ in range(num_sections):
                rec = f.read(40)
                if len(rec) < 40:
                    break
                # First 8 bytes = ASCII name (NUL-padded).
                name = rec[:8].split(b"\x00", 1)[0]
                names.append(name.decode("ascii",
                                         errors="replace").lower())
            return True, names
    except OSError:
        return False, []


def scan_byte_sigs(path: Path) -> list[str]:
    """Look for packer-marker byte sequences in the first few KB.
    Returns labels of matched packers (deduped, preserves order)."""
    try:
        with path.open("rb") as f:
            buf = f.read(8192)
    except OSError:
        return []
    seen: list[str] = []
    for label, needle, window in BYTE_SIGNATURES:
        if needle in buf[:window] and label not in seen:
            seen.append(label)
    return seen


def classify(path: Path) -> ExeReport:
    size = path.stat().st_size if path.exists() else 0
    pe_valid, sections = read_pe_sections(path)
    matches: list[str] = []
    if pe_valid:
        for label, sig_strs in PACKER_SECTIONS.items():
            for s in sections:
                if any(sig in s for sig in sig_strs):
                    if label not in matches:
                        matches.append(label)
                    break
    matches.extend(s for s in scan_byte_sigs(path) if s not in matches)
    # All-null-named sections is a packer telltale (MoleBox /
    # similar set every section name to NUL bytes to hide the
    # original layout). A clean compile always produces named
    # sections (`.text`, `.rdata`, ...). If we have multiple
    # sections and all of them came back empty after NUL strip,
    # that's a packer.
    if pe_valid and len(sections) >= 2 and all(s == "" for s in sections):
        if "MoleBox?" not in matches:
            matches.append("MoleBox?")
    packer = matches[0] if matches else ""
    # Heuristic: a clean FM2K exe is <4MB, has the standard PE
    # sections (.text/.rdata/.data/.rsrc), and no packer hits. Bigger
    # than that without a packer hit is "suspicious — investigate"
    # (probably packed by something we don't have a signature for).
    standard_sections = {".text", ".rdata", ".data", ".rsrc",
                         ".reloc", ".bss", ".idata"}
    nonempty_sections = set(s for s in sections if s)
    has_only_standard = (pe_valid and bool(nonempty_sections)
                         and nonempty_sections <= standard_sections)
    likely_clean = (pe_valid and not matches and size < 4 * 1024 * 1024
                    and has_only_standard)
    return ExeReport(
        path=str(path),
        size=size,
        pe_valid=pe_valid,
        sections=sections,
        packer=packer,
        matches=matches,
        likely_clean=likely_clean,
    )


def find_exes(root: Path) -> Iterable[Path]:
    if root.is_file() and root.suffix.lower() == ".exe":
        yield root
        return
    for p in root.rglob("*.exe"):
        if p.is_file():
            yield p


def fmt_size(n: int) -> str:
    units = ["B", "KB", "MB", "GB"]
    s = float(n)
    for u in units:
        if s < 1024.0:
            return f"{s:7.1f} {u}"
        s /= 1024.0
    return f"{s:.1f} TB"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("root", type=Path, help="dir or single .exe")
    ap.add_argument("--json", action="store_true",
                    help="emit machine-readable JSON instead of "
                         "the human table")
    ap.add_argument("--only-packed", action="store_true",
                    help="suppress likely-clean entries")
    ap.add_argument("--skip-installers", action="store_true", default=True,
                    help="skip exes whose name suggests they're "
                         "installers / launchers / unins / antimicrox / "
                         "lilithport / update (off by default — "
                         "you usually want to know about those too)")
    args = ap.parse_args()

    if not args.root.exists():
        print(f"error: {args.root} not found", file=sys.stderr)
        return 1

    SKIP_NAMES = ("unins", "setup", "install", "antimicrox",
                  "lilithport", "update.exe", "_patch")

    reports: list[ExeReport] = []
    for exe in find_exes(args.root):
        low = exe.name.lower()
        if args.skip_installers and any(s in low for s in SKIP_NAMES):
            continue
        rep = classify(exe)
        if args.only_packed and rep.likely_clean:
            continue
        reports.append(rep)

    if args.json:
        print(json.dumps([asdict(r) for r in reports],
                         indent=2, ensure_ascii=False))
        return 0

    # Human-readable table. Group by packer label so the report
    # ranks "stuff to investigate" first and clean exes last.
    reports.sort(key=lambda r: (r.likely_clean,
                                r.packer.lower(),
                                r.path.lower()))
    n_clean   = sum(1 for r in reports if r.likely_clean)
    n_packed  = sum(1 for r in reports if r.packer)
    n_invalid = sum(1 for r in reports if not r.pe_valid)
    n_susp    = len(reports) - n_clean - n_packed - n_invalid

    print(f"scanned: {len(reports)} exe(s) under {args.root}")
    print(f"  clean (small + standard sections, no packer):   {n_clean}")
    print(f"  packed (known packer signature hit):            {n_packed}")
    print(f"  suspicious (oversize / unknown sections):       {n_susp}")
    print(f"  invalid PE (couldn't parse header):             {n_invalid}")
    print()
    print(f"{'PACKER':<14} {'SIZE':>10}  PATH (first matching section)")
    print("-" * 100)
    for r in reports:
        if r.likely_clean:
            label = "clean"
        elif r.packer:
            label = r.packer
        elif not r.pe_valid:
            label = "(invalid)"
        else:
            label = "(unknown)"
        first_section = r.sections[0] if r.sections else "-"
        rel = r.path
        # Trim repo-prefix for readability.
        for base in ("/mnt/d/games/", "/mnt/c/games/"):
            if rel.startswith(base):
                rel = rel[len(base):]
                break
        print(f"{label:<14} {fmt_size(r.size):>10}  {rel}  "
              f"[{first_section}]")
    return 0


if __name__ == "__main__":
    sys.exit(main())
