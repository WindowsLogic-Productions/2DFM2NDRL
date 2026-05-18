#!/usr/bin/env python3
"""Phase 4b — Playwright leaf fetcher.

Re-attempts every registry resource with `kind == "download"` and
`pulled == False` (i.e. earlier HTTP pulls failed because the host
returns an HTML JS-walled landing page or because the URL needs a
button click to release the actual file bytes).

Strategy per URL, in order — first one that lands a valid archive
wins:

  A. Direct download. Playwright navigates with `accept_downloads=True`;
     if the response is a file the browser kicks off a download which
     we capture. Covers any host that just serves the bytes directly
     (e.g. revived freem links, fc2 personal hosts that still work).

  B. Click-to-download. Page loaded as normal HTML — search for the
     "Download / ダウンロード / DL" button or any anchor whose href
     ends in an archive extension. Click it, expect a download.
     Covers getuploader, Vector, freem.ne.jp, etc.

  C. Wayback fallback. If the live URL is dead or the click chain
     fails, retry the same logic on the resource's `archive_url`
     (Wayback snapshot of the same URL). Some Geocities-era ZIPs
     are reachable only through Wayback.

For each successful pull:
  - magic-byte verify the bytes (refuse HTML masquerading as ZIP)
  - hash with sha256
  - update the resource entry with size_bytes / sha256 / kind_real /
    pulled / method

This script writes back to `games/registry.json`. Re-runs are
idempotent: already-pulled entries are skipped unless `--retry-ok`.
"""

import argparse
import hashlib
import json
import re
import sys
import time
from pathlib import Path

from playwright.sync_api import sync_playwright
from playwright.sync_api import TimeoutError as PlaywrightTimeout
from playwright_stealth import Stealth


REPO = Path(__file__).resolve().parents[1]
REGISTRY = REPO / "games" / "registry.json"
DL_DIR   = REPO / "data" / "fm2k_wiki" / "downloads"
ATWIKI_GAMES_DIR = REPO / "data" / "fm2k_wiki" / "games"

USER_AGENT = ("Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
              "AppleWebKit/537.36 (KHTML, like Gecko) "
              "Chrome/120.0.0.0 Safari/537.36")


# ── magic-byte sniff (mirror of atwiki_fetch_downloads.py) ───────────
def detect_kind(data: bytes) -> str:
    if len(data) < 4:
        return "empty"
    if data[:4] in (b"PK\x03\x04", b"PK\x05\x06", b"PK\x07\x08"):
        return "zip"
    if len(data) >= 7 and data[2:5] == b"-lh":
        return "lzh"
    if data[:7] == b"Rar!\x1a\x07\x00" or data[:8] == b"Rar!\x1a\x07\x01\x00":
        return "rar"
    if data[:6] == b"7z\xbc\xaf\x27\x1c":
        return "7z"
    if data[:3] == b"\x1f\x8b\x08":
        return "gzip"
    if data[:2] == b"MZ":
        return "exe"
    head = data[:512].lstrip()
    low = head[:64].lower()
    if low.startswith(b"<!doctype") or low.startswith(b"<html") \
            or low.startswith(b"<head") or b"<body" in head[:300].lower():
        return "html"
    return "unknown"


VALID_KINDS = {"zip", "lzh", "rar", "7z", "gzip", "exe"}

# Hosts that distribute FREE files directly. Their homepage URLs
# become eligible auto-pull targets — we click through and capture
# the resulting archive.
DISTRIBUTOR_HOSTS = (
    "vector.co.jp",
    "ux.getuploader.com",
    "freem.ne.jp",
    "axfc.net",
    "fc2.com",        # mixed: many fc2 hosts are personal homepages
)

# Hosts that sell games (the developer is paid for the work). NEVER
# auto-pull from these — even if a free demo link is reachable, a
# rights/etiquette concern is "we shouldn't make it trivial to
# bypass the storefront". Resources on these hosts get classified
# as kind="store" and the registry surfaces them as a purchase link.
# Future hook: when the launcher embeds a "buy" button, point it at
# the resource's `purchase_url`.
PAYWALLED_HOSTS = (
    "dlsite.com",
    "dmm.com",
    "dmm.co.jp",
    "melonbooks.com",
    "melonbooks.co.jp",
    "toranoana.jp",
    "toranoana.shop",
    "booth.pm",
    "steampowered.com",
    "steamcommunity.com",
)


def host_class(url: str) -> str:
    """Returns 'paywalled' | 'distributor' | 'other'."""
    u = (url or "").lower()
    if any(h in u for h in PAYWALLED_HOSTS):
        return "paywalled"
    if any(h in u for h in DISTRIBUTOR_HOSTS):
        return "distributor"
    return "other"


def is_distributor(url: str) -> bool:
    return host_class(url) == "distributor"


def is_paywalled(url: str) -> bool:
    return host_class(url) == "paywalled"


# Click-to-download selectors, ordered by specificity. We try each in
# turn and stop at the first one that triggers a download. host-
# specific selectors come first so we don't accidentally click
# unrelated links on pages that have many anchors.
DOWNLOAD_SELECTORS = [
    # ── Direct archive hrefs — always trustworthy when present ──
    'a[href$=".zip"]',
    'a[href$=".lzh"]',
    'a[href$=".exe"]',
    'a[href$=".rar"]',
    'a[href$=".7z"]',
    'a[href*=".zip?"]',
    'a[href*=".lzh?"]',
    'a[href*=".exe?"]',

    # ── Host-specific patterns (more specific than generic text) ──
    # Vector's per-game download landing — distinctive /soft/dl/ path.
    'a[href*="/soft/dl/"]',
    # Vector "actual download" button on the dl page.
    'a[href*="/download/file/"]',
    'a[href*="/download/se"]',
    # ux.getuploader — distinctive form-button or .dlbtn class.
    'a.dlbtn',
    'form[action*="/download/"] button',
    # freem.ne.jp — "今すぐダウンロード" on the download page.
    'a:has-text("今すぐダウンロード")',
    # Vector / freem game-name-prefixed phrasing.
    'a:has-text("ダウンロードページへ")',
    'a:has-text("ここからダウンロード")',

    # ── Generic JP/EN text — last resort ──
    'a:has-text("ダウンロード")',
    'a:has-text("DOWNLOAD")',
    'a:has-text("Download")',
    'a:has-text("DL")',
]


def try_pull(page, url: str, dl_dir: Path,
             max_bytes: int = 200 * 1024 * 1024,
             goto_timeout_ms: int = 30_000,
             max_hops: int = 3) -> dict:
    """Single Playwright pull attempt across up to `max_hops` clicks.

    Many doujin distribution chains aren't single-hop: Vector's flow
    is `game.html` → click → `fh??????.html` → click → actual ZIP
    served with Content-Disposition. Each click that DOESN'T trigger
    a download navigates the page to a new URL — we just keep
    searching for download buttons on the resulting page.

    Returns a result dict — never raises. Errors land in the dict so
    the caller can move on to the next URL.
    """
    visited_urls: list[str] = []
    try:
        # Path 1: maybe the URL itself triggers a direct download.
        # Note: when navigation actually IS a download, Playwright's
        # `page.goto` raises an error like "Download is starting" —
        # but the download object IS captured by `expect_download`.
        # We swallow goto errors inside the with-block so the
        # `dl_info.value` resolution still works.
        try:
            with page.expect_download(timeout=10_000) as dl_info:
                try:
                    page.goto(url, wait_until="commit",
                              timeout=goto_timeout_ms)
                except Exception:
                    pass
            return _capture(dl_info.value, dl_dir, "direct_download",
                            max_bytes)
        except PlaywrightTimeout:
            # Page loaded as HTML — fall through to click-search.
            pass

        # Path 2: walk the page tree clicking download-like targets.
        # Each iteration: try every selector on the current page; if a
        # click triggers a download, capture and return; if a click
        # navigates instead, the next iteration sees the new page.
        for hop in range(max_hops):
            try:
                page.wait_for_load_state("domcontentloaded", timeout=8_000)
            except PlaywrightTimeout:
                pass
            cur = page.url
            visited_urls.append(cur)

            # Some pages use a meta-refresh redirect to the file —
            # just waiting another second often kicks off the download.
            try:
                with page.expect_download(timeout=2_000) as dl_info:
                    page.wait_for_timeout(1_500)
                return _capture(dl_info.value, dl_dir,
                                f"hop{hop}/meta_refresh", max_bytes)
            except PlaywrightTimeout:
                pass

            clicked = False
            for sel in DOWNLOAD_SELECTORS:
                loc = page.locator(sel).first
                try:
                    if loc.count() == 0:
                        continue
                except Exception:
                    continue
                # Prefer NAVIGATING to the href rather than clicking.
                # Pages run interstitial Google ads that intercept
                # synthetic clicks (Vector hits us with a vignette);
                # `goto(href)` sidesteps the JS handler entirely AND
                # plays nicely with `accept_downloads` if the response
                # has Content-Disposition: attachment.
                href = None
                try:
                    href = loc.get_attribute("href")
                except Exception:
                    pass
                if href:
                    target = href if href.startswith("http") else \
                             page.evaluate("(h) => new URL(h, document.baseURI).href", href)
                    # Skip targets we've already followed.
                    if target in visited_urls:
                        continue
                    try:
                        with page.expect_download(timeout=15_000) as dl_info:
                            try:
                                page.goto(target, wait_until="commit",
                                          timeout=goto_timeout_ms)
                            except Exception:
                                # `goto` raises when the response is a
                                # file download — that's exactly what
                                # we want here. The download object
                                # is still captured.
                                pass
                        return _capture(dl_info.value, dl_dir,
                                        f"hop{hop}/goto[{sel}]", max_bytes)
                    except PlaywrightTimeout:
                        # No download fired — page navigated to HTML.
                        # Continue with the new page state in next hop.
                        if page.url != cur:
                            clicked = True
                            break
                        continue
                    except Exception:
                        continue
                # No href — try clicking (form button, JS handler).
                try:
                    with page.expect_download(timeout=8_000) as dl_info:
                        loc.click(timeout=3_000)
                    return _capture(dl_info.value, dl_dir,
                                    f"hop{hop}/click[{sel}]", max_bytes)
                except PlaywrightTimeout:
                    if page.url != cur:
                        clicked = True
                        break
                    continue
                except Exception:
                    continue
            if not clicked:
                break
        return {"ok": False, "method": "no_download_after_clicks",
                "visited": visited_urls[:5]}
    except PlaywrightTimeout:
        return {"ok": False, "method": "goto_timeout"}
    except Exception as e:
        return {"ok": False, "method": "exception", "error": str(e)[:200]}


def _capture(download, dl_dir: Path, method: str,
             max_bytes: int) -> dict:
    """Save a Playwright download, magic-byte verify, hash, return
    result dict. Cleans up on any failure (oversize / wrong kind)."""
    dl_dir.mkdir(parents=True, exist_ok=True)
    suggested = download.suggested_filename or "download.bin"
    out = dl_dir / suggested
    download.save_as(str(out))
    try:
        data = out.read_bytes()
    except OSError as e:
        return {"ok": False, "method": method, "error": f"read_failed: {e}"}
    if max_bytes and len(data) > max_bytes:
        out.unlink(missing_ok=True)
        return {"ok": False, "method": method,
                "error": f"oversize ({len(data)} bytes)"}
    kind = detect_kind(data)
    if kind not in VALID_KINDS:
        out.unlink(missing_ok=True)
        return {"ok": False, "method": method, "kind_seen": kind,
                "size_bytes": len(data)}
    return {
        "ok":         True,
        "method":     method,
        "size_bytes": len(data),
        "sha256":     hashlib.sha256(data).hexdigest(),
        "kind":       kind,
        "filename":   suggested,
        "local_path": str(out.relative_to(REPO)),
    }


def fetch_resource(ctx, resource: dict, dl_root: Path,
                   throttle: float = 1.5) -> dict:
    """Try the live URL; on failure, fall back to the Wayback
    archive_url. Each candidate gets its own page so a wrecked DOM
    state doesn't poison the next try."""
    candidates = []
    if resource.get("url"):
        candidates.append(("live", resource["url"]))
    if resource.get("archive_url"):
        candidates.append(("wayback", resource["archive_url"]))

    attempts: list[dict] = []
    for tag, u in candidates:
        page = ctx.new_page()
        try:
            res = try_pull(page, u, dl_root)
            res["target"] = tag
            res["url"]    = u
            attempts.append(res)
            if res.get("ok"):
                return {"ok": True, "result": res, "attempts": attempts}
        finally:
            try: page.close()
            except Exception: pass
        time.sleep(throttle)
    return {"ok": False, "attempts": attempts}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--limit", type=int, default=0,
                    help="cap number of resources to attempt")
    ap.add_argument("--game-ids", default="",
                    help="comma-separated registry game_ids to filter")
    ap.add_argument("--include-pulled", action="store_true",
                    help="also retry resources that previously succeeded")
    ap.add_argument("--throttle", type=float, default=1.5,
                    help="seconds between resources (default 1.5)")
    ap.add_argument("--max-mb", type=int, default=200,
                    help="per-file size cap MB")
    ap.add_argument("--dry-run", action="store_true",
                    help="don't write registry.json back")
    args = ap.parse_args()

    if not REGISTRY.exists():
        print(f"missing {REGISTRY}", file=sys.stderr)
        return 1

    registry = json.loads(REGISTRY.read_text(encoding="utf-8"))
    wanted_gids = ({s.strip() for s in args.game_ids.split(",") if s.strip()}
                   if args.game_ids else None)

    # Build a flat list of (game_idx, resource_idx) to attempt.
    targets: list[tuple[int, int, dict]] = []
    for gi, reg in enumerate(registry):
        if wanted_gids and reg.get("game_id") not in wanted_gids:
            continue
        for ri, r in enumerate(reg.get("resources") or []):
            kind = r.get("kind")
            url = r.get("url") or ""

            # Skip paywalled storefronts entirely — even if a free
            # demo or sale page would technically respond, we don't
            # want to make it look like the launcher gives free
            # access to paid content. These get re-classified to
            # kind=="store" by the auto-tagger below so the audit /
            # template renders them as a purchase link instead.
            if is_paywalled(url):
                if kind != "store":
                    r["kind"] = "store"
                    r.setdefault("purchase_url", url)
                continue

            # Pull anything explicitly labelled as a download, plus
            # homepages on known FREE file-distribution hosts (Vector,
            # getuploader, freem, etc.) — those are effectively
            # download pages that just need a click chain to release
            # the file.
            if kind == "download":
                pass
            elif kind == "homepage" and is_distributor(url):
                pass
            else:
                continue
            if r.get("pulled") and not args.include_pulled:
                continue
            targets.append((gi, ri, r))
    if args.limit:
        targets = targets[: args.limit]

    if not targets:
        print("no resources to attempt — every download in registry "
              "is already pulled, or filter excluded everything.")
        return 0
    print(f"trying {len(targets)} resources via Playwright")

    n_ok = n_fail = 0
    max_bytes = args.max_mb * 1024 * 1024
    Stealth_ = Stealth(navigator_languages_override=("ja-JP", "ja", "en-US", "en"))
    with Stealth_.use_sync(sync_playwright()) as p:
        browser = p.chromium.launch(headless=True,
            args=["--disable-blink-features=AutomationControlled",
                  "--no-sandbox"])
        ctx = browser.new_context(
            user_agent=USER_AGENT,
            locale="ja-JP",
            viewport={"width": 1280, "height": 900},
            accept_downloads=True,
        )
        for i, (gi, ri, resource) in enumerate(targets, 1):
            reg = registry[gi]
            game_id = reg.get("game_id", "?")
            game_dl_dir = DL_DIR / (reg.get("atwiki_id") or game_id)
            label = (resource.get("filename") or resource.get("url") or "")[:60]
            print(f"[{i:3d}/{len(targets)}] {game_id:<24} {label}",
                  flush=True)

            outcome = fetch_resource(ctx, resource, game_dl_dir,
                                     throttle=args.throttle)
            if outcome["ok"]:
                r = outcome["result"]
                resource.update({
                    "pulled":     True,
                    "size_bytes": r["size_bytes"],
                    "sha256":     r["sha256"],
                    "kind_real":  r["kind"],
                    "local_path": r["local_path"],
                    "pull_method":   r["method"],
                    "pull_target":   r["target"],
                })
                resource.pop("error", None)
                resource.pop("attempts", None)

                # Mirror the result into the per-atwiki JSON so the
                # crossref step (which reads from there) can unpack
                # and hash the exe inside. Without this, only the
                # registry sees the pull and crossref stays oblivious.
                aid = reg.get("atwiki_id")
                if aid:
                    aw_path = ATWIKI_GAMES_DIR / f"{aid}.json"
                    if aw_path.exists():
                        aw = json.loads(aw_path.read_text(encoding="utf-8"))
                        aw.setdefault("downloads", [])
                        # De-dup by sha256.
                        if not any(d.get("sha256") == r["sha256"]
                                   for d in aw["downloads"]):
                            aw["downloads"].append({
                                "original_url": resource.get("url"),
                                "filename":     r["filename"],
                                "size_bytes":   r["size_bytes"],
                                "sha256":       r["sha256"],
                                "kind":         r["kind"],
                                "local_path":   r["local_path"],
                                "pulled_from":  resource.get("url"),
                                "method":       r["method"],
                            })
                            aw_path.write_text(
                                json.dumps(aw, ensure_ascii=False, indent=2),
                                encoding="utf-8")

                print(f"            -> OK {r['size_bytes']} bytes "
                      f"{r['kind']} via {r['method']}")
                n_ok += 1
            else:
                resource["pulled"]   = False
                resource["attempts"] = outcome["attempts"]
                # Keep a brief one-liner error so the audit page can
                # show "tried hard, failed" rather than "never tried".
                resource["error"] = "; ".join(
                    f"{a.get('target','?')}:{a.get('method','?')}"
                    + (f"={a.get('kind_seen')}" if a.get('kind_seen') else "")
                    for a in outcome["attempts"]
                )[:200]
                print(f"            -> FAIL {resource['error'][:120]}")
                n_fail += 1

        ctx.close()
        browser.close()

    if not args.dry_run:
        REGISTRY.write_text(json.dumps(registry, ensure_ascii=False,
                                       indent=2), encoding="utf-8")
        print(f"\nwrote -> {REGISTRY.relative_to(REPO)}")
    print(f"results: {n_ok} pulled, {n_fail} failed")
    return 0 if n_ok > 0 else 2


if __name__ == "__main__":
    raise SystemExit(main())
