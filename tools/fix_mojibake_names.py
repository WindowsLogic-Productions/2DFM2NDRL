#!/usr/bin/env python3
"""Recursively rename CP932-via-CP437 mojibake filenames / dirnames.

The classic Windows mojibake: a Japanese ZIP gets extracted with a tool
that didn't override the entry encoding, so CP932 bytes get stored
on disk as if they were Latin-1 codepoints. The result on NTFS shows
up as strings like  ûºë∩öΩΣ┬ô`AC_R7  when the underlying intent was
密会緋萃伝AC_R7.

Detection rule: a name is mojibake if encoding it as CP437 and
decoding as CP932 yields a Unicode string with at least 2 CJK chars
(by Unicode block test). One-off Latin-1 chars in a plain English
filename don't trip this.

Walks bottom-up so leaf files are renamed before their parent dirs.
Dry-run by default; pass --apply to actually mutate the FS.
"""

import argparse
import sys
import unicodedata
from pathlib import Path

CJK_BLOCKS = (
    (0x3000, 0x303F),   # CJK Symbols
    (0x3040, 0x309F),   # Hiragana
    (0x30A0, 0x30FF),   # Katakana
    (0x3400, 0x4DBF),   # CJK Ext A
    (0x4E00, 0x9FFF),   # CJK Unified
    (0xFF00, 0xFFEF),   # Halfwidth / fullwidth forms
)


def is_cjk(ch: str) -> bool:
    cp = ord(ch)
    for lo, hi in CJK_BLOCKS:
        if lo <= cp <= hi:
            return True
    return False


def try_unmojibake(name: str) -> str | None:
    """Returns the corrected name if `name` looks like CP932-as-CP437
    mojibake, else None."""
    # Quick reject: pure-ASCII names can't be mojibake. They're real.
    if all(ord(c) < 128 for c in name):
        return None
    # Names containing real Japanese (already correct) aren't mojibake.
    # Detect by: does the name already have ≥ 1 CJK char?
    if any(is_cjk(c) for c in name):
        return None
    try:
        fixed = name.encode("cp437").decode("cp932")
    except (UnicodeEncodeError, UnicodeDecodeError):
        return None
    # The decoded form must have at least 2 CJK chars to count as a
    # confident match — a Western name with a few Latin-1 accented
    # chars (café.txt) would also encode to cp437 and decode as cp932
    # producing 2-byte JIS noise; require real CJK density.
    cjk_count = sum(1 for c in fixed if is_cjk(c))
    if cjk_count < 2:
        return None
    return fixed


def walk_and_rename(root: Path, apply: bool) -> tuple[int, int]:
    """Bottom-up rename. Returns (n_renamed, n_skipped_conflict)."""
    n_renamed = 0
    n_conflict = 0
    # Collect all paths first so we don't trip over rename-in-place
    # mutations during iteration.
    paths = sorted((p for p in root.rglob("*")),
                   key=lambda p: -len(p.parts))   # deepest first
    for p in paths:
        try:
            fixed_name = try_unmojibake(p.name)
        except OSError:
            continue
        if fixed_name is None:
            continue
        target = p.with_name(fixed_name)
        if target.exists():
            n_conflict += 1
            print(f"  CONFLICT  {p} → {fixed_name} (target exists, skipping)")
            continue
        print(f"  rename    {p.parent}/{p.name}")
        print(f"         →  {p.parent}/{fixed_name}")
        if apply:
            try:
                p.rename(target)
            except OSError as e:
                print(f"  FAIL: {e}")
                continue
        n_renamed += 1
    return n_renamed, n_conflict


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("root", help="directory tree to walk")
    ap.add_argument("--apply", action="store_true",
                    help="actually rename (default is dry-run)")
    args = ap.parse_args()

    root = Path(args.root).resolve()
    if not root.is_dir():
        print(f"not a directory: {root}", file=sys.stderr)
        return 1

    print(f"walking {root} ({'APPLY' if args.apply else 'dry-run'})...\n")
    n, conflicts = walk_and_rename(root, apply=args.apply)
    print(f"\nrenamed: {n}")
    print(f"conflicts (target exists): {conflicts}")
    if not args.apply:
        print("\n(dry-run — pass --apply to actually rename)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
