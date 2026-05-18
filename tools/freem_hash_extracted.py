#!/usr/bin/env python3
"""Hash every .exe inside data/freem/extracted/<freem_id>/ and surface:
  - xxh64 + size for each exe (matches launcher's identification hash)
  - any matching entry in the known-clean stub registry (KNOWN_EXES)
  - all .kgt files (FM2K project files) for downstream kgt-header parsing

Writes results back to data/freem/games/<freem_id>.json under
`extracted.exes` and `extracted.kgts`, replacing the flat-string lists
the extractor wrote with richer dicts.
"""

import json
import sys
from pathlib import Path

import xxhash


REPO = Path(__file__).resolve().parents[1]
GAMES_DIR = REPO / "data" / "freem" / "games"
EXTRACTED = REPO / "data" / "freem" / "extracted"

# Mirror of tools/atwiki_crossref.py::KNOWN_EXES. Stub-runtime hashes
# the launcher recognizes — these map a build to its engine variant
# but DON'T identify the game (one stub serves many games; identity
# comes from the .kgt project name).
KNOWN_EXES: dict[int, tuple[str, str]] = {
    0x506FF9AB93D15134: ("FM2K", "WonderfulWorld v0.946 stub"),
    0x36358AD6F9EC387B: ("FM95", "Comic Party Wars (CPW)"),
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


def process_one(rec_path: Path) -> dict | None:
    rec = json.loads(rec_path.read_text(encoding="utf-8"))
    gid = rec["freem_id"]
    extracted_root = EXTRACTED / gid
    if not extracted_root.exists():
        return None

    # Walk the tree case-insensitively for .exe + .kgt + a few common
    # variants (.EXE on some Windows-cased archives).
    exe_records: list[dict] = []
    kgt_records: list[dict] = []
    for p in extracted_root.rglob("*"):
        if not p.is_file():
            continue
        lower = p.name.lower()
        rel = str(p.relative_to(extracted_root))
        if lower.endswith(".exe"):
            xh = hash_file(p)
            xh_hex = f"0x{xh:016X}"
            known = KNOWN_EXES.get(xh)
            exe_records.append({
                "path":          rel,
                "size_bytes":    p.stat().st_size,
                "xxh64":         xh_hex,
                "match_known":   known[1] if known else None,
                "engine_guess":  known[0] if known else None,
            })
        elif lower.endswith(".kgt"):
            kgt_records.append({
                "path":          rel,
                "size_bytes":    p.stat().st_size,
            })

    rec.setdefault("extracted", {})
    rec["extracted"]["exes"] = exe_records
    rec["extracted"]["kgts"] = kgt_records
    rec_path.write_text(json.dumps(rec, ensure_ascii=False, indent=2),
                        encoding="utf-8")
    return {
        "freem_id": gid,
        "title":    rec.get("title", ""),
        "n_exes":   len(exe_records),
        "n_kgts":   len(kgt_records),
        "n_known":  sum(1 for e in exe_records if e["match_known"]),
        "exes":     exe_records,
    }


def main() -> int:
    if not EXTRACTED.exists():
        print(f"missing {EXTRACTED}", file=sys.stderr)
        return 1

    summaries = []
    for rec_path in sorted(GAMES_DIR.glob("*.json")):
        s = process_one(rec_path)
        if s:
            summaries.append(s)
            print(f"  {s['freem_id']:>6}  {s['n_exes']:>2} exes ({s['n_known']:>2} known) · "
                  f"{s['n_kgts']:>2} kgt  | {s['title'][:50]}")

    total_exes = sum(s["n_exes"] for s in summaries)
    total_kgts = sum(s["n_kgts"] for s in summaries)
    total_known = sum(s["n_known"] for s in summaries)
    print(f"\nprocessed {len(summaries)} games:")
    print(f"  exes:        {total_exes}")
    print(f"  kgts:        {total_kgts}")
    print(f"  known-stub:  {total_known}")

    # Detailed roll-up of unique exe xxh64 → engine variant
    print(f"\nstub variant breakdown:")
    by_known: dict[str, list[str]] = {}
    by_unknown: dict[str, list[str]] = {}
    for s in summaries:
        for e in s["exes"]:
            if e["match_known"]:
                by_known.setdefault(e["match_known"], []).append(
                    f"{s['freem_id']}/{Path(e['path']).name}")
            else:
                by_unknown.setdefault(e["xxh64"], []).append(
                    f"{s['freem_id']}/{Path(e['path']).name} ({e['size_bytes']} B)")
    for label, items in sorted(by_known.items()):
        print(f"  ✓ {label}: {len(items)}")
        for it in items[:5]:
            print(f"      {it}")
    if by_unknown:
        print(f"\nunknown exe hashes ({len(by_unknown)} distinct):")
        for xh, items in list(by_unknown.items())[:15]:
            print(f"  {xh}  ({len(items)} files)  e.g. {items[0]}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
