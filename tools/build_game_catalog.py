#!/usr/bin/env python3
"""Build game catalog — markdown for humans + JSON for programs.

Walks /mnt/d/games/{fm2k,fm95}/, reads each metadata.json sentinel,
emits two artifacts:

  docs/games_catalog.md   — pretty grouped tables, ready to drop in
                            the wiki or stitch into the stats-site
                            landing page.
  games/catalog.json      — flat list of records, used by the hub
                            and launcher's registry-builder.

Schema per record:
  {
    "id":         "<engine>/<studio>/<dir>",   # stable key
    "name":       "Eight Marbles",             # display
    "alt_names":  [],
    "studio":     "Maiga858" | "_NODEV",
    "year":       "2005",
    "engine":     "fm2k_2nd" | "fm95",
    "variants":   ["file-1", "Cleaned exe"],   # subdirs (if multi)
    "exe":        "fm2k/Maiga858/Eight Marbles/8 Marbles.exe",
    "exe_size":   1204224,
    "kgt":        "fm2k/Maiga858/Eight Marbles/8 Marbles.kgt",
    "source":     "MyAbandonware" | "archive.org" | ...,
    "url":        "...",
    "download":   "..." | {"file-1": "..."},
    "status":     "ready" | "locked" | "no_exe",
    "ia_id":      "..." | null,
    "path":       "fm2k/Maiga858/Eight Marbles",  # rel to /mnt/d/games
  }

Usage:
    python3 tools/build_game_catalog.py
    python3 tools/build_game_catalog.py --root /mnt/d/games
    python3 tools/build_game_catalog.py --md-out path.md --json-out p.json
"""

from __future__ import annotations

import argparse
import json
import sys
from collections import defaultdict
from dataclasses import dataclass, field, asdict
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_ROOT = Path("/mnt/d/games")
DEFAULT_MD = REPO_ROOT / "docs" / "games_catalog.md"
DEFAULT_JSON = REPO_ROOT / "games" / "catalog.json"

ENGINE_BASELINE = 1_204_224

# Recognized "variant" subdir names — anything else under a game-dir
# gets ignored as a sub-tree (game files / asset dirs).
VARIANT_HINTS = (
    "file-1", "cleaned exe", "cleaned-exe", "full japanese version",
    "updated version", "version ", "v0.", "v1.", "version-",
    "patch", "english", "japanese", "ja", "en",
)


@dataclass
class CatalogEntry:
    id: str
    name: str
    alt_names: list[str]
    studio: str
    year: str
    engine: str
    variants: list[str]
    exe: str | None
    exe_size: int
    kgt: str | None
    source: str
    url: str
    download: object  # str | dict
    status: str
    ia_id: str | None
    path: str
    notes: list[str] = field(default_factory=list)


def find_kgt_and_exe(d: Path) -> tuple[Path | None, Path | None, int]:
    """BFS for canonical kgt+exe pair under d."""
    queue: list[tuple[Path, int]] = [(d, 0)]
    while queue:
        cur, depth = queue.pop(0)
        if depth > 6:
            continue
        try:
            entries = list(cur.iterdir())
        except OSError:
            continue
        kgt = next((p for p in entries
                    if p.is_file() and p.suffix.lower() == ".kgt"),
                   None)
        if kgt is not None:
            target = cur / f"{kgt.stem}.exe"
            if target.exists():
                return kgt, target, target.stat().st_size
            # fallback any exe
            any_exe = next((p for p in entries
                            if p.is_file()
                            and p.suffix.lower() == ".exe"),
                           None)
            if any_exe is not None:
                return kgt, any_exe, any_exe.stat().st_size
            return kgt, None, 0
        for p in sorted(entries):
            if p.is_dir():
                queue.append((p, depth + 1))
    return None, None, 0


def detect_variants(game_dir: Path) -> list[str]:
    """A variant subdir is a non-asset directory at game_dir's top
    level whose contents include a kgt+exe pair (= playable build)."""
    out: list[str] = []
    try:
        for sub in sorted(game_dir.iterdir()):
            if not sub.is_dir():
                continue
            # If sub itself looks like a game install, it's a variant.
            kgt, exe, _ = find_kgt_and_exe(sub)
            if kgt is not None and exe is not None:
                out.append(sub.name)
    except OSError:
        pass
    return out


def read_metadata(game_dir: Path) -> dict:
    p = game_dir / "metadata.json"
    if not p.is_file():
        return {}
    try:
        return json.loads(p.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return {}


def normalize_studio(name: str) -> str:
    n = (name or "").strip()
    if n.lower() in ("", "empty", "unknown", "n/a"):
        return "_NODEV"
    return n


def detect_engine(exe_size: int) -> str:
    if exe_size == 0:
        return "unknown"
    if exe_size < ENGINE_BASELINE:
        return "fm95"
    return "fm2k_2nd"


def make_entry(game_dir: Path, root: Path,
               engine_bucket: str) -> CatalogEntry:
    meta = read_metadata(game_dir)
    kgt, exe, exe_size = find_kgt_and_exe(game_dir)
    variants = detect_variants(game_dir)

    rel_path = game_dir.relative_to(root)
    parts = rel_path.parts
    # parts = (engine_bucket, studio, game_dir_name) typically
    if len(parts) >= 3:
        studio = parts[1]
    else:
        studio = "_NODEV"
    studio = normalize_studio(studio)

    name = (meta.get("name") or game_dir.name).strip()
    alt = meta.get("alternative names") or meta.get("alt_names") or []
    year = (meta.get("year") or "").strip()
    source = (meta.get("source") or "").strip()
    url = (meta.get("url") or "").strip()
    download = meta.get("download") or meta.get("downloads") or ""

    # Engine: prefer bucket the dir is filed under; fall back to size.
    engine = ("fm95" if engine_bucket == "fm95"
              else "fm2k_2nd")

    # Status:
    if exe is None:
        if game_dir.name.endswith("(locked)"):
            status = "locked"
        else:
            status = "no_exe"
    elif exe_size > ENGINE_BASELINE:
        status = "still_packed"
    else:
        status = "ready"

    return CatalogEntry(
        id=str(rel_path),
        name=name,
        alt_names=list(alt) if isinstance(alt, list) else [],
        studio=studio,
        year=year,
        engine=engine,
        variants=variants,
        exe=str(exe.relative_to(root)) if exe is not None else None,
        exe_size=exe_size,
        kgt=str(kgt.relative_to(root)) if kgt is not None else None,
        source=source,
        url=url,
        download=download,
        status=status,
        ia_id=meta.get("ia_identifier") or None,
        path=str(rel_path),
    )


def discover_game_dirs(root: Path,
                       bucket: str) -> list[Path]:
    bucket_root = root / bucket
    if not bucket_root.exists():
        return []
    out: list[Path] = []
    for sentinel in bucket_root.rglob("metadata.json"):
        if sentinel.is_file():
            out.append(sentinel.parent)
    return sorted(out, key=lambda p: str(p).lower())


def md_escape(s: str) -> str:
    """Escape pipe characters that would break a markdown table cell."""
    return (s or "").replace("|", "\\|").replace("\n", " ").strip()


def render_md(entries: list[CatalogEntry], out_path: Path) -> None:
    by_engine: dict[str, list[CatalogEntry]] = defaultdict(list)
    for e in entries:
        by_engine[e.engine].append(e)

    fm2k = sorted(by_engine.get("fm2k_2nd", []),
                  key=lambda e: (e.studio.lower(),
                                 e.name.lower()))
    fm95 = sorted(by_engine.get("fm95", []),
                  key=lambda e: (e.studio.lower(),
                                 e.name.lower()))

    lines: list[str] = []
    lines.append("# FM2K / FM95 Game Catalog")
    lines.append("")
    lines.append(f"Total: **{len(entries)}** games — "
                 f"{len(fm2k)} FM2K_2nd + {len(fm95)} FM95")
    lines.append("")
    lines.append("Generated by `tools/build_game_catalog.py`. The "
                 "rollback launcher targets FM2K_2nd; FM95 entries "
                 "are kept on disk but not yet hookable.")
    lines.append("")
    lines.append("## Status legend")
    lines.append("")
    lines.append("- ✅ **ready** — clean engine + .kgt + .exe paired")
    lines.append("- 🔒 **locked** — archive password-protected, "
                 "couldn't extract")
    lines.append("- ⚠️ **no_exe** — found .kgt but no matching .exe")
    lines.append("- ⚙️ **still_packed** — exe larger than baseline, "
                 "needs unwrap")
    lines.append("")

    def section(title: str, group: list[CatalogEntry]) -> None:
        lines.append(f"## {title}")
        lines.append("")
        if not group:
            lines.append("_(none)_")
            lines.append("")
            return
        # Group by studio
        by_studio: dict[str, list[CatalogEntry]] = defaultdict(list)
        for e in group:
            by_studio[e.studio].append(e)
        # Iterate studios alphabetically, but always _NODEV last.
        studio_order = sorted(by_studio.keys(),
                              key=lambda s: (s == "_NODEV",
                                             s.lower()))
        for studio in studio_order:
            games = sorted(by_studio[studio],
                           key=lambda e: e.name.lower())
            label = studio if studio != "_NODEV" else (
                "_unsigned / no studio listed_")
            lines.append(f"### {label}  ({len(games)})")
            lines.append("")
            lines.append("| Status | Game | Year | Variants | "
                         "Source | Path |")
            lines.append("|---|---|---|---|---|---|")
            for e in games:
                emoji = {"ready": "✅", "locked": "🔒",
                         "no_exe": "⚠️",
                         "still_packed": "⚙️"}.get(e.status, "❓")
                variants = (", ".join(e.variants)
                            if e.variants else "—")
                src_link = (f"[{md_escape(e.source)}]({e.url})"
                            if e.url and e.source else
                            md_escape(e.source) or "—")
                lines.append(f"| {emoji} | "
                             f"{md_escape(e.name)} | "
                             f"{md_escape(e.year) or '—'} | "
                             f"{md_escape(variants)} | "
                             f"{src_link} | "
                             f"`{md_escape(e.path)}` |")
            lines.append("")

    section("FM2K_2nd (rollback-compatible engine)", fm2k)
    section("FM95 (separate engine)", fm95)

    # Stats footer
    lines.append("## Stats")
    lines.append("")
    studios = {e.studio for e in entries
               if e.studio and e.studio != "_NODEV"}
    by_year: dict[str, int] = defaultdict(int)
    by_source: dict[str, int] = defaultdict(int)
    by_status: dict[str, int] = defaultdict(int)
    for e in entries:
        if e.year:
            decade = e.year[:3] + "0s" if len(e.year) == 4 else "?"
            by_year[decade] += 1
        else:
            by_year["unknown"] += 1
        by_source[e.source or "unknown"] += 1
        by_status[e.status] += 1

    lines.append(f"- Distinct studios: {len(studios)}")
    lines.append(f"- By decade: " + ", ".join(
        f"{d} ({n})" for d, n in sorted(by_year.items())))
    lines.append(f"- By source: " + ", ".join(
        f"{s} ({n})" for s, n in sorted(by_source.items())))
    lines.append(f"- By status: " + ", ".join(
        f"{s} ({n})" for s, n in sorted(by_status.items())))
    lines.append("")

    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text("\n".join(lines), encoding="utf-8")


def render_json(entries: list[CatalogEntry], out_path: Path) -> None:
    data = [asdict(e) for e in entries]
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(
        json.dumps(data, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", type=Path, default=DEFAULT_ROOT)
    ap.add_argument("--md-out", type=Path, default=DEFAULT_MD)
    ap.add_argument("--json-out", type=Path, default=DEFAULT_JSON)
    args = ap.parse_args()

    if not args.root.exists():
        print(f"error: {args.root} not found", file=sys.stderr)
        return 1

    entries: list[CatalogEntry] = []
    for bucket in ("fm2k", "fm95"):
        for gd in discover_game_dirs(args.root, bucket):
            entries.append(make_entry(gd, args.root, bucket))

    print(f"discovered {len(entries)} game(s)")
    by_engine: dict[str, int] = defaultdict(int)
    by_status: dict[str, int] = defaultdict(int)
    for e in entries:
        by_engine[e.engine] += 1
        by_status[e.status] += 1
    print(f"  by engine: {dict(by_engine)}")
    print(f"  by status: {dict(by_status)}")

    render_md(entries, args.md_out)
    render_json(entries, args.json_out)
    print(f"wrote: {args.md_out}")
    print(f"wrote: {args.json_out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
