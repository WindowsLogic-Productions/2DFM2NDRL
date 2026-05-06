#!/usr/bin/env python3
"""Delete archives whose contents are already on disk.

After the migrate + unpack pass, every .zip / .7z / .rar /
SFX-style .exe has a sibling extracted directory. The originals are
just dead weight at that point. This pass walks the tree and removes
each one that's safe to drop:

  - <archive>.zip  → drop if <archive>/ exists and is non-empty
  - <archive>.7z   → drop if <archive>/ exists and is non-empty
  - <archive>.rar  → drop if <archive>/ exists and is non-empty
  - <archive>.exe  → drop ONLY if <archive>.unpacked/ exists and
                     is non-empty (i.e. unpack_sfx_exes.py confirmed
                     it's an SFX wrapper). Plain game .exes never
                     have a .unpacked/ sibling and therefore never
                     get touched.

Always dry-run by default. Pass --apply to actually delete.

Usage:
    python3 tools/prune_extracted.py /mnt/d/games/2dfm
    python3 tools/prune_extracted.py /mnt/d/games/2dfm --apply
    python3 tools/prune_extracted.py /mnt/d/games/2dfm --apply \\
        --keep-large 200    # keep originals over 200 MB as backup
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path


ARCHIVE_EXTS = (".zip", ".7z", ".rar")


def has_extracted_sibling(archive: Path) -> Path | None:
    """Return the populated sibling extract dir for an archive, or
    None if no usable extraction exists."""
    if archive.suffix.lower() == ".exe":
        candidate = archive.parent / f"{archive.stem}.unpacked"
    elif archive.suffix.lower() in ARCHIVE_EXTS:
        candidate = archive.parent / archive.stem
    else:
        return None
    if not candidate.exists() or not candidate.is_dir():
        return None
    try:
        if not any(candidate.iterdir()):
            return None
    except OSError:
        return None
    return candidate


def fmt_size(n: int) -> str:
    units = ["B", "KB", "MB", "GB"]
    s = float(n)
    for u in units:
        if s < 1024.0:
            return f"{s:7.1f} {u}"
        s /= 1024.0
    return f"{s:7.1f} TB"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("root", type=Path,
                    help="dir to walk; usually /mnt/d/games/2dfm")
    ap.add_argument("--apply", action="store_true",
                    help="actually delete; default is dry run")
    ap.add_argument("--keep-large", type=int, default=0,
                    help="keep archives larger than this MB even "
                         "when extracted (safety net for very large "
                         "downloads worth re-storing). default 0 = "
                         "no size protection.")
    ap.add_argument("--include-exe-sfx", action="store_true",
                    default=True,
                    help="also prune SFX-style .exe wrappers (those "
                         "with a .unpacked/ sibling). on by default.")
    args = ap.parse_args()

    if not args.root.exists():
        print(f"error: {args.root} not found", file=sys.stderr)
        return 1

    cands: list[tuple[Path, Path, int]] = []
    sfx_exes: list[tuple[Path, Path, int]] = []
    print(f"scanning {args.root} for extractable archives ...",
          flush=True)
    for p in args.root.rglob("*"):
        if not p.is_file():
            continue
        suffix = p.suffix.lower()
        if suffix in ARCHIVE_EXTS:
            sib = has_extracted_sibling(p)
            if sib is not None:
                cands.append((p, sib, p.stat().st_size))
        elif suffix == ".exe" and args.include_exe_sfx:
            sib = has_extracted_sibling(p)
            if sib is not None:
                sfx_exes.append((p, sib, p.stat().st_size))

    threshold_bytes = args.keep_large * 1024 * 1024 if args.keep_large else 0

    total_archive_bytes = sum(s for _, _, s in cands)
    total_sfx_bytes = sum(s for _, _, s in sfx_exes)
    total_files = len(cands) + len(sfx_exes)

    print()
    print(f"found {len(cands)} extracted archive(s) "
          f"({fmt_size(total_archive_bytes)} reclaimable)")
    print(f"found {len(sfx_exes)} extracted SFX exe(s) "
          f"({fmt_size(total_sfx_bytes)} reclaimable)")
    if not total_files:
        print("nothing to prune.")
        return 0
    print()

    pruned = 0
    skipped_size = 0
    bytes_freed = 0
    for path, sib, size in cands + sfx_exes:
        rel = path
        for base in ("/mnt/d/games/", "/mnt/c/games/"):
            try:
                rel = path.relative_to(base)
            except ValueError:
                continue
            break
        if threshold_bytes and size >= threshold_bytes:
            print(f"  KEEP  {fmt_size(size)}  {rel}  "
                  f"(over {args.keep_large} MB)")
            skipped_size += 1
            continue
        if args.apply:
            try:
                path.unlink()
                pruned += 1
                bytes_freed += size
                print(f"  DEL   {fmt_size(size)}  {rel}")
            except OSError as e:
                print(f"  ERR   {rel}: {e}", file=sys.stderr)
        else:
            pruned += 1
            bytes_freed += size
            print(f"  drop  {fmt_size(size)}  {rel}  "
                  f"(extracted → {sib.name}/)")

    print()
    if args.apply:
        print(f"deleted {pruned} file(s), freed {fmt_size(bytes_freed)}")
    else:
        print(f"would delete {pruned} file(s), freeing "
              f"{fmt_size(bytes_freed)}")
        if skipped_size:
            print(f"would skip {skipped_size} file(s) "
                  f"(over --keep-large threshold)")
        print("(dry run — pass --apply to actually delete)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
