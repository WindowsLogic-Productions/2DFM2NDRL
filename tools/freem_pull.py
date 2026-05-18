#!/usr/bin/env python3
"""Pull ZIP/EXE bytes from freem.ne.jp for FM2K/FM95 games.

Uses Playwright + stealth (same setup as tools/atwiki_playwright_pull.py)
since freem's `/dl/win/<id>` is a JS-walled interstitial — direct curl
gets an HTML "please click the button" page, not the file.

Flow per game:
  1. Visit https://www.freem.ne.jp/win/game/<id>     (detail page, for any author-set click chain)
  2. Click the "ダウンロード" / Download button → freem nav to /dl/win/<id>
  3. /dl/win/<id> auto-fires the file download (Content-Disposition: attachment)
  4. Capture the bytes via expect_download, save to
     data/freem/downloads/<id>/<filename>
  5. Compute sha256 + xxh64 (xxh64 matches launcher's identification hash).
  6. Update per-game JSON with `pulled: {sha256, xxh64, size_bytes, kind,
     filename, pulled_at}`.

Idempotent: skip games that already have `pulled` set. Use --force to
re-pull.

Input: data/freem/games/<id>.json files produced by freem_crawl.py.
By default pulls all FM2K/FM95-tagged games; --ids restricts.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import shutil
import sys
import time
from pathlib import Path

import xxhash
from playwright.sync_api import sync_playwright
from playwright.sync_api import TimeoutError as PlaywrightTimeout
from playwright_stealth import Stealth


REPO = Path(__file__).resolve().parents[1]
FREEM_GAMES_DIR    = REPO / "data" / "freem" / "games"
FREEM_DL_DIR       = REPO / "data" / "freem" / "downloads"
DETAIL_URL_FMT     = "https://www.freem.ne.jp/win/game/{}"
DL_URL_FMT         = "https://www.freem.ne.jp/dl/win/{}"

UA = ("Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
      "AppleWebKit/537.36 (KHTML, like Gecko) "
      "Chrome/121.0.0.0 Safari/537.36")

# Per-file safety cap. Reduces risk of an evil server feeding us GBs.
MAX_BYTES = 1_500_000_000   # 1.5 GB

# Magic-byte sniff so a "download" that's actually HTML gets rejected
# (freem doesn't normally do this but the atwiki pull was burned by it).
MAGIC = {
    b"PK\x03\x04": "zip",
    b"MZ":         "exe",
    b"Rar!\x1a\x07": "rar",
    b"7z\xbc\xaf\x27\x1c": "7z",
    b"\x1f\x9d":   "lzh",       # weak — LHA archives don't have a strong magic
    b"-lh":        "lzh",       # `-lh5-` etc. appears at offset 2 in LHA
}


def sniff(buf: bytes) -> str | None:
    for prefix, name in MAGIC.items():
        if buf.startswith(prefix):
            return name
    # LHA's identifier is at offset 2 (size byte) + "-lh"
    if len(buf) >= 7 and buf[2:7].startswith(b"-lh"):
        return "lzh"
    return None


def hash_file(path: Path) -> tuple[str, int]:
    """Returns (sha256_hex, xxh64_int) over the file."""
    h_sha = hashlib.sha256()
    h_xx  = xxhash.xxh64(seed=0)
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h_sha.update(chunk)
            h_xx.update(chunk)
    return h_sha.hexdigest(), h_xx.intdigest()


def pull_one(page, freem_id: str, force: bool = False) -> dict:
    """Drive Playwright to fetch the bytes for `freem_id`. Returns a
    status dict — never raises."""
    rec_path = FREEM_GAMES_DIR / f"{freem_id}.json"
    if not rec_path.exists():
        return {"ok": False, "error": "no metadata; run freem_crawl.py first"}

    rec = json.loads(rec_path.read_text(encoding="utf-8"))
    if rec.get("pulled") and not force:
        return {"ok": True, "skipped": True, "filename": rec["pulled"].get("filename")}

    dl_dir = FREEM_DL_DIR / freem_id
    dl_dir.mkdir(parents=True, exist_ok=True)

    # ── Two-hop download flow on freem ──
    #
    #   /win/game/<id>             → marketing detail page (sets session)
    #   /dl/win/<id>               → interstitial with a Download anchor:
    #       <a id="dlLink"
    #          href="/dl/download/win/<id>/<token>"
    #          onclick="dl(); return false;">Download</a>
    #   /dl/download/win/<id>/<token>  → actual file, served with
    #       Content-Disposition: attachment
    #
    # The token is generated per pageview, so we can't shortcut to step 3
    # without first hitting the interstitial and reading the href. The
    # onclick handler fires a JS tracker before navigation; sidestep by
    # extracting the href and gotoing it directly (same pattern as the
    # atwiki Vector flow).

    # Step 1 — detail page (sets cookies, looks human).
    try:
        page.goto(DETAIL_URL_FMT.format(freem_id),
                  wait_until="domcontentloaded", timeout=20_000)
    except Exception as e:
        return {"ok": False, "error": f"detail goto failed: {e}"}
    page.wait_for_timeout(600)

    # Step 2 — interstitial. Extract the per-pageview token href.
    try:
        page.goto(DL_URL_FMT.format(freem_id),
                  wait_until="domcontentloaded", timeout=20_000)
    except Exception as e:
        return {"ok": False, "error": f"interstitial goto failed: {e}"}

    href = None
    try:
        loc = page.locator("a#dlLink").first
        if loc.count() > 0:
            href = loc.get_attribute("href")
    except Exception:
        pass
    if not href:
        # Fallback — any anchor whose href starts with /dl/download/win/
        try:
            loc = page.locator('a[href^="/dl/download/win/"]').first
            if loc.count() > 0:
                href = loc.get_attribute("href")
        except Exception:
            pass
    if not href:
        return {"ok": False,
                "error": "no /dl/download/win/<id>/<token> anchor on interstitial"}

    download_url = href if href.startswith("http") else \
                   f"https://www.freem.ne.jp{href}"

    # Step 3 — fire the real download. expect_download captures the
    # file even when goto raises (which it does when the response is
    # Content-Disposition: attachment and there's no HTML body).
    try:
        with page.expect_download(timeout=60_000) as dl_info:
            try:
                page.goto(download_url, wait_until="commit",
                          timeout=20_000)
            except Exception:
                pass
        dl = dl_info.value
    except PlaywrightTimeout:
        return {"ok": False, "error": f"download didn't fire from {download_url}"}

    return _capture_download(dl, freem_id, dl_dir, rec_path, rec)


def _capture_download(dl, freem_id, dl_dir, rec_path, rec):
    suggested = dl.suggested_filename or f"{freem_id}.bin"
    safe = re.sub(r"[^A-Za-z0-9._\-]+", "_", suggested)[:120]
    target = dl_dir / safe

    try:
        dl.save_as(str(target))
    except Exception as e:
        return {"ok": False, "error": f"save_as failed: {e}"}

    size = target.stat().st_size
    if size > MAX_BYTES:
        target.unlink()
        return {"ok": False, "error": f"oversized ({size} > {MAX_BYTES})"}
    if size < 1024:
        target.unlink()
        return {"ok": False, "error": f"tiny ({size} bytes — likely error page)"}

    # Sniff the first 16 bytes to make sure it's a real archive/exe
    # and not an HTML error.
    with target.open("rb") as f:
        head = f.read(16)
    kind = sniff(head)
    if kind is None:
        target.unlink()
        return {"ok": False,
                "error": f"magic-byte mismatch (first bytes: {head[:8]!r})"}

    sha, xx = hash_file(target)
    rec["pulled"] = {
        "filename":    safe,
        "size_bytes":  size,
        "sha256":      sha,
        "xxh64":       f"0x{xx:016X}",
        "kind":        kind,
        "pulled_at":   int(time.time()),
        "local_path":  str(target.relative_to(REPO)),
    }
    rec_path.write_text(json.dumps(rec, ensure_ascii=False, indent=2),
                        encoding="utf-8")
    return {"ok": True, "filename": safe, "size_bytes": size,
            "kind": kind, "xxh64": rec["pulled"]["xxh64"]}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--ids", default="",
                    help="comma-separated freem_ids to pull (overrides --limit)")
    ap.add_argument("--limit", type=int, default=0,
                    help="cap at N games (0 = all matching)")
    ap.add_argument("--force", action="store_true",
                    help="re-pull even if already have bytes")
    ap.add_argument("--throttle", type=float, default=2.0,
                    help="seconds between game pulls (default 2.0)")
    ap.add_argument("--headed", action="store_true",
                    help="run browser in headed mode (debug)")
    ap.add_argument("--include-all", action="store_true",
                    help=("pull every record (no engine filter). Use when "
                          "trusting freem's search results as the source of "
                          "truth even for games whose detail-page HTML "
                          "doesn't repeat the engine tag string."))
    ap.add_argument("--max-bytes-per-file", type=int, default=0,
                    help="skip games whose declared size exceeds this many "
                         "bytes (0 = unlimited)")
    args = ap.parse_args()

    if not FREEM_GAMES_DIR.exists():
        print(f"missing {FREEM_GAMES_DIR} — run freem_crawl.py first",
              file=sys.stderr)
        return 1

    # Build the work list.
    if args.ids:
        wanted = {s.strip() for s in args.ids.split(",") if s.strip()}
        records = [(g, FREEM_GAMES_DIR / f"{g}.json") for g in wanted
                   if (FREEM_GAMES_DIR / f"{g}.json").exists()]
    else:
        records = []
        for p in sorted(FREEM_GAMES_DIR.glob("*.json")):
            d = json.loads(p.read_text(encoding="utf-8"))
            if not args.include_all:
                if d.get("engine") not in ("FM2K", "FM95"):
                    continue
            if args.max_bytes_per_file and (d.get("size_bytes") or 0) > args.max_bytes_per_file:
                continue
            records.append((d["freem_id"], p))
        if args.limit:
            records = records[: args.limit]

    print(f"pulling {len(records)} game(s) from freem.ne.jp ...\n",
          flush=True)

    with Stealth().use_sync(sync_playwright()) as p:
        browser = p.chromium.launch(
            headless=not args.headed,
            args=["--no-sandbox", "--disable-blink-features=AutomationControlled"])
        context = browser.new_context(
            user_agent=UA,
            accept_downloads=True,
            locale="ja-JP",
            viewport={"width": 1280, "height": 800})
        page = context.new_page()

        n_pulled = n_failed = n_skipped = 0
        total_bytes = 0
        for i, (freem_id, _path) in enumerate(records, 1):
            r = pull_one(page, freem_id, force=args.force)
            if r.get("skipped"):
                n_skipped += 1
                print(f"  [{i:3d}/{len(records)}] {freem_id:>6}  SKIP "
                      f"(already pulled: {r.get('filename')})", flush=True)
                continue
            if r.get("ok"):
                n_pulled += 1
                total_bytes += r.get("size_bytes", 0)
                print(f"  [{i:3d}/{len(records)}] {freem_id:>6}  OK   "
                      f"{r['filename']} ({r['size_bytes']/1e6:.1f} MB · "
                      f"{r['kind']} · {r['xxh64']})", flush=True)
            else:
                n_failed += 1
                print(f"  [{i:3d}/{len(records)}] {freem_id:>6}  FAIL "
                      f"{r.get('error')}", flush=True)
            time.sleep(args.throttle)

        browser.close()

    print(f"\npulled:  {n_pulled} games  ({total_bytes/1e9:.2f} GB)")
    print(f"failed:  {n_failed}")
    print(f"skipped: {n_skipped}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
