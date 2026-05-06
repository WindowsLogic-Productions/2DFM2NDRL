"""FM2K Public Stats Site — read-only views over the hub's match log.

Architecture and security model documented at
docs/dev/stats_site_design.md (in the parent repo). Single-file FastAPI
app for v1. Reads `matches.json` from disk on each request — no DB, no
cache invalidation hooks needed; the file is tiny by web-app standards
and we re-parse fast enough that an extra 10ms per request beats the
complexity of a watchdog observer.

Security posture:
  - Read-only file access.
  - Jinja2 autoescape enabled by default for *.html templates; no `safe`
    filter or `Markup()` calls.
  - Discord IDs (p1_dc_id / p2_dc_id) never rendered. They live on disk
    for ops correlation; the public site reads only `p1_id` / `p2_id`.
  - Strict CSP: default-src 'self'; img-src 'self' data:; no inline JS.
  - Per-IP rate limit on /api/v1 endpoints via slowapi.
  - Process should run as an unprivileged user with read-only access to
    the hub's data dir (deploy-time concern, not enforced in code).
"""

from __future__ import annotations

import json
import os
import time
from collections import Counter, defaultdict
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Optional

from fastapi import FastAPI, HTTPException, Query, Request
from fastapi.responses import HTMLResponse, JSONResponse
from fastapi.staticfiles import StaticFiles
from fastapi.templating import Jinja2Templates
from slowapi import Limiter, _rate_limit_exceeded_handler
from slowapi.errors import RateLimitExceeded
from slowapi.util import get_remote_address


# ─── Config ──────────────────────────────────────────────────────────────

# Path to the hub's matches.json. Defaults to ../hub/matches.json relative
# to this file so a standard `cd stats && uvicorn app:app` works without
# fiddling. Override via FM2K_MATCHES_PATH for production deploys where
# the hub data dir is elsewhere.
DEFAULT_MATCHES_PATH = Path(__file__).resolve().parent.parent / "hub" / "matches.json"
MATCHES_PATH = Path(os.environ.get("FM2K_MATCHES_PATH", str(DEFAULT_MATCHES_PATH)))

# Game registry — pretty names + (eventually) banners + character art.
# Built by tools/build_registry.py from MyAbandonware + archive.org
# scrapes. Optional: if absent, the site falls back to rendering the
# raw game_id string.
DEFAULT_REGISTRY_PATH = Path(__file__).resolve().parent.parent / "games" / "registry.json"
REGISTRY_PATH = Path(os.environ.get("FM2K_REGISTRY_PATH",
                                    str(DEFAULT_REGISTRY_PATH)))

# Mounted under nginx at /stats by default; the path prefix is consumed by
# the reverse proxy so we serve from "/" internally. If you're running
# uvicorn directly behind no proxy, the urls in templates still work
# because url_for emits relative paths.
ROOT_PATH = os.environ.get("FM2K_STATS_ROOT_PATH", "")

# Maximum matches the JSON API will return in one request — caps the
# bandwidth a single client can hammer out of us.
API_MATCHES_HARD_LIMIT = 500


# ─── App + middleware ────────────────────────────────────────────────────

app = FastAPI(
    title="FM2K Stats",
    description="Public read-only stats for FM2K Rollback hub matches.",
    version="0.1.0",
    root_path=ROOT_PATH,
    docs_url=None,           # hide Swagger UI on the public site
    redoc_url=None,
    openapi_url=None,
)

limiter = Limiter(key_func=get_remote_address)
app.state.limiter = limiter
app.add_exception_handler(RateLimitExceeded, _rate_limit_exceeded_handler)


@app.middleware("http")
async def security_headers(request: Request, call_next):
    """Apply a strict CSP + a few belt-and-suspenders headers to every
    response. Inline scripts are disallowed by default-src + script-src,
    so any future XSS attempt fails to execute."""
    response = await call_next(request)
    response.headers["Content-Security-Policy"] = (
        "default-src 'self'; "
        "img-src 'self' data:; "
        "style-src 'self' 'unsafe-inline'; "
        "script-src 'none'; "
        "frame-ancestors 'none'"
    )
    response.headers["X-Content-Type-Options"] = "nosniff"
    response.headers["Referrer-Policy"] = "no-referrer"
    response.headers["X-Frame-Options"] = "DENY"
    return response


_static_dir = Path(__file__).resolve().parent / "static"
_templates_dir = Path(__file__).resolve().parent / "templates"

app.mount("/static", StaticFiles(directory=str(_static_dir)), name="static")
templates = Jinja2Templates(directory=str(_templates_dir))


# ─── Match log loader ────────────────────────────────────────────────────

def load_matches() -> list[dict[str, Any]]:
    """Read matches.json off disk. Returns a defensive copy with only the
    fields the public site is allowed to render. Drops any record missing
    structural keys instead of crashing the renderer."""
    try:
        with MATCHES_PATH.open("r", encoding="utf-8") as f:
            payload = json.load(f)
    except (OSError, json.JSONDecodeError):
        return []
    raw = payload.get("matches", []) if isinstance(payload, dict) else []
    out: list[dict[str, Any]] = []
    for r in raw:
        if not isinstance(r, dict):
            continue
        # Required keys for any aggregation. Missing → skip.
        if not r.get("id") or not r.get("p1_id") or not r.get("p2_id"):
            continue
        if not isinstance(r.get("started_at"), (int, float)):
            continue
        # Whitelist fields. Notably excludes p1_dc_id / p2_dc_id —
        # those exist on disk for ops correlation but are not rendered.
        out.append({
            "id":            r.get("id"),
            "p1_id":         r.get("p1_id"),
            "p1_nick":       r.get("p1_nick", "anon"),
            "p2_id":         r.get("p2_id"),
            "p2_nick":       r.get("p2_nick", "anon"),
            "p1_char_id":    r.get("p1_char_id"),
            "p2_char_id":    r.get("p2_char_id"),
            "p1_char_name":  r.get("p1_char_name"),
            "p2_char_name":  r.get("p2_char_name"),
            "game_id":       r.get("game_id", ""),
            "winner_id":     r.get("winner_id", ""),
            "started_at":    r.get("started_at"),
            "finished_at":   r.get("finished_at", r.get("started_at")),
        })
    return out


# ─── Game registry loader ────────────────────────────────────────────────

# In-memory cache keyed by mtime. The registry is small (~140 records,
# <100KB) and reads are fast; we just want to skip re-parsing on every
# request without locking ourselves out of edits during dev.
_registry_cache: dict[str, Any] = {"mtime": -1.0, "by_id": {}}


def load_registry() -> dict[str, dict[str, Any]]:
    """Return {game_id: record} for the current registry. Empty dict
    when the file is absent or corrupt — callers fall back to raw
    game_id rendering in that case."""
    try:
        st = REGISTRY_PATH.stat()
    except OSError:
        return {}
    if st.st_mtime == _registry_cache["mtime"]:
        return _registry_cache["by_id"]
    try:
        with REGISTRY_PATH.open("r", encoding="utf-8") as f:
            recs = json.load(f)
    except (OSError, json.JSONDecodeError):
        return _registry_cache["by_id"]
    by_id: dict[str, dict[str, Any]] = {}
    for r in recs:
        if not isinstance(r, dict):
            continue
        gid = r.get("game_id")
        if not gid:
            continue
        by_id[gid] = r
    _registry_cache["mtime"] = st.st_mtime
    _registry_cache["by_id"] = by_id
    return by_id


def game_friendly_name(game_id: str) -> str:
    """Pretty name from the registry. Falls back to the raw game_id so
    a missing entry still renders as something readable."""
    if not game_id:
        return ""
    rec = load_registry().get(game_id)
    if rec and rec.get("name"):
        return rec["name"]
    return game_id


def game_meta(game_id: str) -> dict[str, Any]:
    """Full registry record for a game_id. Empty dict if not in the
    registry. Templates use this to render banner / year / source
    links when available, with .get() defaults so missing fields
    don't crash the render."""
    return load_registry().get(game_id, {})


# Make the registry helpers available to every template render
# without threading them through each handler explicitly. Templates
# can call {{ friendly_name(m.game_id) }} or {{ game_meta(gid).year }}.
templates.env.globals["friendly_name"] = game_friendly_name
templates.env.globals["game_meta"] = game_meta


def char_label(name: Optional[str], cid: Optional[int]) -> str:
    """Render a friendly character label. Filename (already UTF-8) when
    the hook resolved one; "id N" fallback otherwise; "?" for fully
    unknown."""
    if isinstance(name, str) and name:
        return name
    if isinstance(cid, int):
        return f"id {cid}"
    return "?"


def format_iso(ts: float) -> str:
    """Sortable, human-readable timestamp. UTC for consistency across
    deploys."""
    try:
        return datetime.fromtimestamp(ts, tz=timezone.utc).strftime("%Y-%m-%d %H:%M UTC")
    except (OSError, ValueError, OverflowError):
        return "?"


def aggregate_user(user_id: str) -> dict[str, Any]:
    """Build the per-user rollups consumed by /u/<id>. One full pass over
    the match log; everything derives from the same view of the data."""
    matches = [m for m in load_matches() if m["p1_id"] == user_id or m["p2_id"] == user_id]
    if not matches:
        return {
            "user_id": user_id, "nick": user_id,
            "total": 0, "wins": 0, "losses": 0, "draws": 0,
            "by_game": [], "by_char": [], "vs_opponents": [],
            "recent": [],
        }
    nick = matches[-1]["p1_nick"] if matches[-1]["p1_id"] == user_id else matches[-1]["p2_nick"]

    wins = losses = draws = 0
    by_game: dict[str, dict[str, int]] = defaultdict(lambda: {"wins": 0, "losses": 0, "draws": 0})
    by_char: dict[str, dict[str, int]] = defaultdict(lambda: {"wins": 0, "losses": 0, "draws": 0})
    vs_opp:  dict[str, dict[str, Any]] = defaultdict(
        lambda: {"opp_id": "", "opp_nick": "", "wins": 0, "losses": 0, "draws": 0})

    for m in matches:
        i_am_p1   = (m["p1_id"] == user_id)
        opp_id    = m["p2_id"]   if i_am_p1 else m["p1_id"]
        opp_nick  = m["p2_nick"] if i_am_p1 else m["p1_nick"]
        my_char   = char_label(m["p1_char_name"] if i_am_p1 else m["p2_char_name"],
                                m["p1_char_id"]   if i_am_p1 else m["p2_char_id"])
        gkey      = m["game_id"] or "(unknown)"
        winner    = m["winner_id"]

        if not winner:
            r = "draws"
            draws += 1
        elif winner == user_id:
            r = "wins"
            wins += 1
        else:
            r = "losses"
            losses += 1
        by_game[gkey][r] += 1
        by_char[my_char][r] += 1
        cell = vs_opp[opp_id]
        cell["opp_id"]   = opp_id
        cell["opp_nick"] = opp_nick
        cell[r] += 1

    # Sort: most-played first within each grouping.
    by_game_rows = sorted(
        ({"game_id": g, **rec} for g, rec in by_game.items()),
        key=lambda r: -(r["wins"] + r["losses"] + r["draws"]))
    by_char_rows = sorted(
        ({"char": c, **rec} for c, rec in by_char.items()),
        key=lambda r: -(r["wins"] + r["losses"] + r["draws"]))
    vs_rows = sorted(
        vs_opp.values(),
        key=lambda r: -(r["wins"] + r["losses"] + r["draws"]))[:32]

    recent = sorted(matches, key=lambda m: -m["finished_at"])[:20]
    return {
        "user_id": user_id, "nick": nick,
        "total": len(matches), "wins": wins, "losses": losses, "draws": draws,
        "by_game": by_game_rows[:32],
        "by_char": by_char_rows[:32],
        "vs_opponents": vs_rows,
        "recent": recent,
    }


def aggregate_game(game_id: str) -> dict[str, Any]:
    matches = [m for m in load_matches() if m["game_id"] == game_id]
    if not matches:
        return {"game_id": game_id, "total": 0, "top_players": [],
                "top_chars": [], "recent": []}

    by_player: Counter = Counter()
    by_char:   Counter = Counter()
    by_player_nick: dict[str, str] = {}
    for m in matches:
        by_player[m["p1_id"]] += 1
        by_player[m["p2_id"]] += 1
        by_player_nick[m["p1_id"]] = m["p1_nick"]
        by_player_nick[m["p2_id"]] = m["p2_nick"]
        by_char[char_label(m["p1_char_name"], m["p1_char_id"])] += 1
        by_char[char_label(m["p2_char_name"], m["p2_char_id"])] += 1

    top_players = [
        {"user_id": uid, "nick": by_player_nick[uid], "matches": n}
        for uid, n in by_player.most_common(20)
    ]
    top_chars = [{"char": c, "matches": n} for c, n in by_char.most_common(20)]
    recent = sorted(matches, key=lambda m: -m["finished_at"])[:20]
    return {"game_id": game_id, "total": len(matches),
            "top_players": top_players, "top_chars": top_chars,
            "recent": recent}


def aggregate_character(game_id: str, char_key: str) -> dict[str, Any]:
    """char_key matches against either char_name (preferred) or `id N`
    so URLs can be human-readable when names are available and still
    work for unmapped slots."""
    matches = [m for m in load_matches() if m["game_id"] == game_id]
    on_p1 = [m for m in matches if char_label(m["p1_char_name"], m["p1_char_id"]) == char_key]
    on_p2 = [m for m in matches if char_label(m["p2_char_name"], m["p2_char_id"]) == char_key]
    plays = on_p1 + on_p2
    if not plays:
        return {"game_id": game_id, "char": char_key, "total": 0,
                "top_pilots": [], "matchups": [], "recent": []}

    by_pilot: dict[str, dict[str, Any]] = defaultdict(
        lambda: {"user_id": "", "nick": "", "wins": 0, "losses": 0, "draws": 0})
    matchups: dict[str, dict[str, int]] = defaultdict(
        lambda: {"wins": 0, "losses": 0, "draws": 0})

    def record(pilot_id: str, pilot_nick: str, opp_char: str, winner_id: str):
        cell = by_pilot[pilot_id]
        cell["user_id"] = pilot_id
        cell["nick"]    = pilot_nick
        if not winner_id:
            cell["draws"] += 1; matchups[opp_char]["draws"] += 1
        elif winner_id == pilot_id:
            cell["wins"] += 1;  matchups[opp_char]["wins"] += 1
        else:
            cell["losses"] += 1; matchups[opp_char]["losses"] += 1

    for m in on_p1:
        record(m["p1_id"], m["p1_nick"],
               char_label(m["p2_char_name"], m["p2_char_id"]), m["winner_id"])
    for m in on_p2:
        record(m["p2_id"], m["p2_nick"],
               char_label(m["p1_char_name"], m["p1_char_id"]), m["winner_id"])

    pilots = sorted(by_pilot.values(),
                    key=lambda r: -(r["wins"] + r["losses"] + r["draws"]))[:20]
    mu_rows = sorted(
        ({"opp_char": c, **rec} for c, rec in matchups.items()),
        key=lambda r: -(r["wins"] + r["losses"] + r["draws"]))[:32]
    recent = sorted(plays, key=lambda m: -m["finished_at"])[:20]
    return {"game_id": game_id, "char": char_key, "total": len(plays),
            "top_pilots": pilots, "matchups": mu_rows, "recent": recent}


# ─── Routes ──────────────────────────────────────────────────────────────

@app.get("/", response_class=HTMLResponse)
def landing(request: Request):
    matches = load_matches()
    now = time.time()
    today_cutoff = now - 86400
    week_cutoff = now - 7 * 86400

    by_player: Counter = Counter()
    by_player_nick: dict[str, str] = {}
    by_game: Counter = Counter()
    for m in matches:
        by_player[m["p1_id"]] += 1
        by_player[m["p2_id"]] += 1
        by_player_nick[m["p1_id"]] = m["p1_nick"]
        by_player_nick[m["p2_id"]] = m["p2_nick"]
        if m["game_id"]:
            by_game[m["game_id"]] += 1

    top_players = [
        {"user_id": uid, "nick": by_player_nick[uid], "matches": n}
        for uid, n in by_player.most_common(10)
    ]
    top_games = by_game.most_common(10)
    return templates.TemplateResponse("landing.html", {
        "request": request,
        "total_all":   len(matches),
        "total_day":   sum(1 for m in matches if m["finished_at"] >= today_cutoff),
        "total_week":  sum(1 for m in matches if m["finished_at"] >= week_cutoff),
        "top_players": top_players,
        "top_games":   top_games,
    })


@app.get("/u/{user_id}", response_class=HTMLResponse)
def user_page(request: Request, user_id: str):
    if not user_id or len(user_id) > 64:
        raise HTTPException(status_code=404)
    data = aggregate_user(user_id)
    if data["total"] == 0:
        raise HTTPException(status_code=404, detail="No matches for this user.")
    return templates.TemplateResponse("user.html",
        {"request": request, "u": data, "char_label": char_label,
         "format_iso": format_iso})


@app.get("/g/{game_id}", response_class=HTMLResponse)
def game_page(request: Request, game_id: str):
    if not game_id or len(game_id) > 64:
        raise HTTPException(status_code=404)
    data = aggregate_game(game_id)
    if data["total"] == 0:
        raise HTTPException(status_code=404, detail="No matches for this game.")
    return templates.TemplateResponse("game.html",
        {"request": request, "g": data, "char_label": char_label,
         "format_iso": format_iso})


@app.get("/g/{game_id}/c/{char_key}", response_class=HTMLResponse)
def character_page(request: Request, game_id: str, char_key: str):
    if not game_id or len(game_id) > 64 or not char_key or len(char_key) > 96:
        raise HTTPException(status_code=404)
    data = aggregate_character(game_id, char_key)
    if data["total"] == 0:
        raise HTTPException(status_code=404, detail="No matches for this character.")
    return templates.TemplateResponse("character.html",
        {"request": request, "c": data, "char_label": char_label,
         "format_iso": format_iso})


@app.get("/recent", response_class=HTMLResponse)
def recent_page(request: Request, limit: int = 100):
    limit = max(1, min(limit, 200))
    matches = load_matches()
    matches = sorted(matches, key=lambda m: -m["finished_at"])[:limit]
    return templates.TemplateResponse("recent.html",
        {"request": request, "matches": matches,
         "char_label": char_label, "format_iso": format_iso})


@app.get("/api/v1/matches")
@limiter.limit("60/minute")
def api_matches(
    request: Request,
    limit: int = Query(50, ge=1, le=API_MATCHES_HARD_LIMIT),
    user_id: Optional[str] = Query(None, max_length=64),
    game_id: Optional[str] = Query(None, max_length=64),
):
    matches = load_matches()
    if user_id:
        matches = [m for m in matches if m["p1_id"] == user_id or m["p2_id"] == user_id]
    if game_id:
        matches = [m for m in matches if m["game_id"] == game_id]
    matches = sorted(matches, key=lambda m: -m["finished_at"])[:limit]
    return JSONResponse({"matches": matches})


@app.exception_handler(404)
async def not_found(request: Request, exc: HTTPException):
    return templates.TemplateResponse(
        "404.html",
        {"request": request, "detail": getattr(exc, "detail", None)},
        status_code=404,
    )
