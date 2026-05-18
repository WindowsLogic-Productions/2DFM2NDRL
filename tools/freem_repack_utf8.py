#!/usr/bin/env python3
"""Repack freem-pulled ZIPs with UTF-8 entry names + UTF-8 GP bit set.

Freem zips entry names are CP932 bytes without the UTF-8 bit (general
purpose bit 11 = 0x800). Default Windows Explorer / WinRAR / 7-Zip
interpret those bytes as CP437, producing mojibake like
`ûºë∩öΩΣ┬ô\`AC_R7` instead of `密会緋萃伝AC_R7`.

This tool rewrites each zip so:
  - Entry names are stored as UTF-8 byte sequences
  - The 0x800 ("UTF-8 names") flag is set on every entry

The repacked zip is portable: WinRAR, 7-Zip, macOS Archive Utility,
Linux unzip, all read it as Japanese without locale tricks.

Original zip is preserved as `<name>.cp932.zip` next to the new one
so we have provenance.
"""

import argparse
import json
import shutil
import sys
import zipfile
from pathlib import Path


REPO          = Path(__file__).resolve().parents[1]
GAMES_DIR     = REPO / "data" / "freem" / "games"
DOWNLOADS_DIR = REPO / "data" / "freem" / "downloads"


def repack(src_zip: Path) -> dict:
    """Returns {ok, original_kept, new_path, n_entries} or {error}."""
    # Read source with cp932 metadata so we get proper Unicode str names.
    try:
        zin = zipfile.ZipFile(src_zip, metadata_encoding="cp932")
    except TypeError:
        # <3.11 fallback — wouldn't actually be running here, but be
        # explicit. The script targets Python 3.12+.
        return {"error": "Python 3.11+ required for metadata_encoding"}

    # Preserve the original by renaming to .cp932.zip
    preserved = src_zip.with_name(src_zip.stem + ".cp932.zip")
    if not preserved.exists():
        shutil.move(str(src_zip), str(preserved))
    # The destination zip goes where the original was.
    dst_zip = src_zip

    n_entries = 0
    n_dirs    = 0
    with zin, zipfile.ZipFile(dst_zip, "w", zipfile.ZIP_DEFLATED) as zout:
        for info in zin.infolist():
            # Re-encode the (already Unicode) filename as UTF-8 bytes.
            # zipfile auto-sets the UTF-8 flag (0x800) when the
            # filename is given as a str AND the bytes have any
            # non-ASCII char — but to be explicit we force-set the
            # flag bit so even ASCII names get the flag (defensive).
            new_info = zipfile.ZipInfo(filename=info.filename,
                                       date_time=info.date_time)
            new_info.compress_type = info.compress_type
            new_info.external_attr = info.external_attr
            new_info.create_system = info.create_system
            new_info.flag_bits = info.flag_bits | 0x800  # UTF-8 flag
            if info.is_dir():
                zout.writestr(new_info, b"")
                n_dirs += 1
            else:
                with zin.open(info) as src:
                    data = src.read()
                zout.writestr(new_info, data)
                n_entries += 1
    return {"ok": True, "preserved": preserved.name,
            "n_entries": n_entries, "n_dirs": n_dirs}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--ids", default="",
                    help="comma-separated freem_ids (default: all zip pulls)")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    if args.ids:
        wanted = {s.strip() for s in args.ids.split(",") if s.strip()}
        paths = [GAMES_DIR / f"{i}.json" for i in wanted
                 if (GAMES_DIR / f"{i}.json").exists()]
    else:
        paths = sorted(GAMES_DIR.glob("*.json"))

    n_repacked = 0
    n_skipped  = 0
    for rec_path in paths:
        rec = json.loads(rec_path.read_text(encoding="utf-8"))
        pulled = rec.get("pulled") or {}
        if pulled.get("kind") != "zip":
            continue
        src = REPO / pulled["local_path"]
        if not src.exists():
            print(f"  SKIP   {rec['freem_id']:>6}  missing: {src}")
            continue
        # Already repacked? (We set utf8_repacked: true on the record.)
        if rec.get("utf8_repacked") and not args.dry_run:
            n_skipped += 1
            print(f"  SKIP   {rec['freem_id']:>6}  already repacked")
            continue

        if args.dry_run:
            print(f"  DRY    {rec['freem_id']:>6}  would repack {src.name}")
            continue

        r = repack(src)
        if r.get("error"):
            print(f"  FAIL   {rec['freem_id']:>6}  {r['error']}")
            continue
        rec["utf8_repacked"] = True
        rec["original_cp932_zip"] = r["preserved"]
        rec_path.write_text(json.dumps(rec, ensure_ascii=False, indent=2),
                            encoding="utf-8")
        n_repacked += 1
        print(f"  OK     {rec['freem_id']:>6}  "
              f"{r['n_entries']:>4d} files / {r['n_dirs']:>2d} dirs  "
              f"({rec['title'][:50]})")

    print(f"\nrepacked: {n_repacked}  skipped: {n_skipped}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
