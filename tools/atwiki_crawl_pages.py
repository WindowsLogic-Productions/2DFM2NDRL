#!/usr/bin/env python3
"""Phase 2: crawl every atwiki game page listed in index.json.

Bypasses Cloudflare via headless Chromium (Playwright). One browser
context is reused across all 337 fetches, so the CF challenge is
solved once on the first page and the resulting clearance cookie
covers the whole run.

Caches each fetched HTML to data/fm2k_wiki/cache/<atwiki_id>.html.
Re-running skips cached IDs unless --force is passed, so partial runs
are cheap to resume.

Usage:
    python3 tools/atwiki_crawl_pages.py            # full crawl
    python3 tools/atwiki_crawl_pages.py --limit 10 # smoke test
    python3 tools/atwiki_crawl_pages.py --force    # re-fetch everything
"""

import argparse
import json
import sys
import time
from pathlib import Path

from playwright.sync_api import sync_playwright, TimeoutError as PlaywrightTimeout
from playwright_stealth import Stealth


REPO = Path(__file__).resolve().parents[1]
INDEX = REPO / "data" / "fm2k_wiki" / "index.json"
CACHE_DIR = REPO / "data" / "fm2k_wiki" / "cache"

PAGE_URL = "https://w.atwiki.jp/arunau32167/pages/{id}.html"

# CF challenge can be slow on the very first hit (cold IP / fresh
# context). Subsequent pages reuse the clearance cookie and clear in
# milliseconds. The 60s budget here only matters once per run.
CF_BUDGET_MS = 60_000      # max wait while still on CF challenge page
PAGE_BUDGET_MS = 30_000    # max wait for actual page after clearance
POLITE_DELAY_S = 2.0       # between pages — be nice to atwiki


def is_challenge_page(html: str) -> bool:
    return "Just a moment" in html or "challenge-platform" in html


def fetch_page(page, atwiki_id: str) -> tuple[bool, str, str]:
    """Returns (ok, html, status). Status is a short tag for logs."""
    url = PAGE_URL.format(id=atwiki_id)
    try:
        page.goto(url, wait_until="domcontentloaded", timeout=PAGE_BUDGET_MS)
    except PlaywrightTimeout:
        return False, "", "page-timeout"

    # If we landed on a CF challenge, give it room to resolve. Polling
    # the page text avoids picking a fixed sleep that's too short on a
    # slow run or wastes time on the fast path.
    deadline = time.monotonic() + (CF_BUDGET_MS / 1000.0)
    html = page.content()
    while is_challenge_page(html) and time.monotonic() < deadline:
        time.sleep(0.5)
        html = page.content()

    if is_challenge_page(html):
        return False, html, "cf-stuck"
    if "wikibody" not in html and "id=\"contents\"" not in html:
        return False, html, "no-content"
    return True, html, "ok"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--limit", type=int, default=0, help="stop after N pages")
    ap.add_argument("--force", action="store_true",
                    help="re-fetch even if already cached")
    ap.add_argument("--delay", type=float, default=POLITE_DELAY_S,
                    help=f"sec between pages (default: {POLITE_DELAY_S})")
    args = ap.parse_args()

    if not INDEX.exists():
        print(f"missing {INDEX} — run atwiki_extract_index.py first", file=sys.stderr)
        return 1
    CACHE_DIR.mkdir(parents=True, exist_ok=True)

    with INDEX.open(encoding="utf-8") as f:
        games = json.load(f)["games"]

    todo = []
    for g in games:
        cached = CACHE_DIR / f"{g['atwiki_id']}.html"
        if cached.exists() and not args.force:
            continue
        todo.append(g)
    if args.limit:
        todo = todo[: args.limit]

    print(f"crawling {len(todo)}/{len(games)} atwiki pages "
          f"({len(games) - len(todo)} already cached)")

    n_ok = n_fail = 0
    failures: list[dict] = []

    # Cloudflare Turnstile reads many runtime signals (navigator.webdriver,
    # WebGL fingerprint, audio fingerprint, plugin list shape, sec-ch-ua,
    # etc.). The full evasion set lives in playwright-stealth — Stealth()
    # patches all of those before page scripts run, which is what gets us
    # past atwiki's challenge unattended in WSL.
    stealth = Stealth(navigator_languages_override=("ja-JP", "ja", "en-US", "en"))
    with stealth.use_sync(sync_playwright()) as p:
        browser = p.chromium.launch(
            headless=True,
            args=[
                "--disable-blink-features=AutomationControlled",
                "--no-sandbox",
            ],
        )
        ctx = browser.new_context(
            user_agent=("Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
                        "AppleWebKit/537.36 (KHTML, like Gecko) "
                        "Chrome/120.0.0.0 Safari/537.36"),
            locale="ja-JP",
            viewport={"width": 1280, "height": 900},
        )
        page = ctx.new_page()

        for i, g in enumerate(todo, 1):
            ok, html, status = fetch_page(page, g["atwiki_id"])
            line = f"[{i:3d}/{len(todo)}] {g['engine']} {g['atwiki_id']:>4} {status:<12} {g['title']}"
            print(line, flush=True)
            if ok:
                (CACHE_DIR / f"{g['atwiki_id']}.html").write_text(
                    html, encoding="utf-8")
                n_ok += 1
            else:
                n_fail += 1
                failures.append({
                    "atwiki_id": g["atwiki_id"],
                    "title": g["title"],
                    "status": status,
                })
            # Skip the polite delay on the very last page.
            if i < len(todo):
                time.sleep(args.delay)

        ctx.close()
        browser.close()

    if failures:
        fail_path = REPO / "data" / "fm2k_wiki" / "crawl_failures.json"
        fail_path.write_text(json.dumps({"failures": failures},
                             ensure_ascii=False, indent=2), encoding="utf-8")
        print(f"  wrote {len(failures)} failures -> {fail_path.relative_to(REPO)}")

    print(f"\ndone: {n_ok} ok, {n_fail} failed")
    return 0 if n_fail == 0 else 2


if __name__ == "__main__":
    raise SystemExit(main())
