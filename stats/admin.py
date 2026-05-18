"""Stats admin editor — trusted-user write surface for games/registry.json.

v1 auth model: shared bearer tokens stored in stats/admin_tokens.json.
A token is a long random string plus a human nick. Tokens are presented
either via:
  - Cookie `fm2k_admin` (set by /admin/login form POST)
  - Authorization: Bearer <token> header (for any future API client)

There is no signed session — the token IS the credential, kept HttpOnly
+ SameSite=Strict so a hostile site can't trigger an edit by linking to
/admin/g/{id}/save with a forged form. CSP already forbids inline JS,
so token theft via XSS isn't a vector either.

v2 path (not implemented here): swap _check_token for a hub-side Discord
role lookup. The router shape stays the same; only `current_editor()`
changes.

Edit scope (per-game):
  - resources[]   — add / edit / remove rows. Source defaults to
                    "manual" so we can tell hand-edits from atwiki-crawler
                    and other automated importers.
  - description, alt_names, year, developer, publisher
  - homepage, wayback_homepage  (manual override of auto-resolved value)
  - purchase_url                 (paywalled / store row helper)

NOT in scope:
  - versions[]    — populated by hash-tracked imports only
  - match data    — immutable
  - _raw          — provenance from upstream scrapes; preserved untouched
  - game_id       — primary key, edits would orphan match history
"""

from __future__ import annotations

import contextlib
import fcntl
import json
import os
import re
import secrets
import time
from collections import Counter
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Iterator, Optional

from fastapi import APIRouter, Body, Cookie, Form, HTTPException, Request
from fastapi.responses import HTMLResponse, JSONResponse, RedirectResponse
from fastapi.templating import Jinja2Templates


# ─── Paths ───────────────────────────────────────────────────────────────

_BASE = Path(__file__).resolve().parent
TOKENS_PATH  = Path(os.environ.get("FM2K_ADMIN_TOKENS_PATH",
                                   str(_BASE / "admin_tokens.json")))
EDITLOG_PATH = Path(os.environ.get("FM2K_EDITLOG_PATH",
                                   str(_BASE / "editlog.jsonl")))

# Registry path is set by app.py at mount time (calls set_registry_path).
# We default to the same env var the read side honors so a unit test
# importing this module without going through app.py still finds the
# right file.
REGISTRY_PATH: Path = Path(os.environ.get(
    "FM2K_REGISTRY_PATH",
    str(_BASE.parent / "games" / "registry.json")))


def set_registry_path(p: Path) -> None:
    """Called by app.py to align this module with whatever path the
    public read side resolved (env var → default)."""
    global REGISTRY_PATH
    REGISTRY_PATH = Path(p)


# ─── Token store ─────────────────────────────────────────────────────────

def _load_tokens() -> dict[str, dict[str, Any]]:
    """{token_string: {nick, created_at, ...}}.  Empty dict when the
    file doesn't exist — keeps `/admin/login` working (with no valid
    tokens) on a fresh deploy until someone mints one."""
    try:
        data = json.loads(TOKENS_PATH.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return {}
    out: dict[str, dict[str, Any]] = {}
    for row in data.get("tokens", []):
        if not isinstance(row, dict):
            continue
        tok = row.get("token")
        if isinstance(tok, str) and tok:
            out[tok] = row
    return out


def _check_token(token: Optional[str]) -> Optional[dict[str, Any]]:
    """Constant-time match. Returns the editor record on success, None
    otherwise. None of the comparison branches reveal which token we
    matched against — only that *something* matched."""
    if not token:
        return None
    tokens = _load_tokens()
    if not tokens:
        return None
    matched: Optional[dict[str, Any]] = None
    for known, row in tokens.items():
        # secrets.compare_digest avoids timing leaks; we run it for
        # every entry so an attacker can't probe by length either.
        if secrets.compare_digest(token, known):
            matched = row
    return matched


# ─── Registry I/O ────────────────────────────────────────────────────────

def _read_registry() -> list[dict[str, Any]]:
    return json.loads(REGISTRY_PATH.read_text(encoding="utf-8"))


@contextlib.contextmanager
def _registry_lock() -> Iterator[None]:
    """Process-wide exclusive lock on a sidecar lockfile, so two
    concurrent saves (e.g. from a multi-worker uvicorn deploy) can't
    interleave a read-modify-write."""
    lock_path = REGISTRY_PATH.with_suffix(".lock")
    lock_path.parent.mkdir(parents=True, exist_ok=True)
    with open(lock_path, "w") as f:
        fcntl.flock(f, fcntl.LOCK_EX)
        try:
            yield
        finally:
            fcntl.flock(f, fcntl.LOCK_UN)


def _atomic_write_registry(records: list[dict[str, Any]]) -> Path:
    """Write registry with .bak rotation + tmp+rename atomicity.

    Caller must hold the _registry_lock() — we don't grab it here so
    that read-modify-write callers can do both under one lock.

    Returns the .bak path so the caller can log it. We keep up to 5
    backups (.bak.<unixts>) to bound disk usage; older ones are pruned
    silently.
    """
    payload = json.dumps(records, ensure_ascii=False, indent=2)
    tmp = REGISTRY_PATH.with_suffix(".json.tmp")
    bak = REGISTRY_PATH.with_suffix(f".json.bak.{int(time.time())}")

    if REGISTRY_PATH.exists():
        # Use copy-then-replace for the bak so a reader currently
        # holding the file open isn't surprised by a rename. The
        # subsequent atomic replace handles the actual write.
        bak.write_bytes(REGISTRY_PATH.read_bytes())
    tmp.write_text(payload, encoding="utf-8")
    tmp.replace(REGISTRY_PATH)

    # Prune old backups (keep newest 5).
    baks = sorted(REGISTRY_PATH.parent.glob(REGISTRY_PATH.name + ".bak.*"))
    for old in baks[:-5]:
        try:
            old.unlink()
        except OSError:
            pass
    return bak


def _diff_record(before: dict[str, Any], after: dict[str, Any]) -> dict[str, Any]:
    """Shallow diff of changed fields. resources[] is reported as a
    {added, removed, kept_count} summary so the audit log doesn't
    bloat with full URL lists on every edit."""
    diff: dict[str, Any] = {}
    keys = set(before) | set(after)
    for k in keys:
        b = before.get(k)
        a = after.get(k)
        if b == a:
            continue
        if k == "resources":
            b_keys = {(r.get("kind"), r.get("url")) for r in (b or [])}
            a_keys = {(r.get("kind"), r.get("url")) for r in (a or [])}
            diff["resources"] = {
                "added":   sorted(["::".join(k_ or "" for k_ in t)
                                   for t in (a_keys - b_keys)]),
                "removed": sorted(["::".join(k_ or "" for k_ in t)
                                   for t in (b_keys - a_keys)]),
                "kept_count": len(a_keys & b_keys),
            }
        else:
            diff[k] = {"before": b, "after": a}
    return diff


def _append_editlog(entry: dict[str, Any]) -> None:
    EDITLOG_PATH.parent.mkdir(parents=True, exist_ok=True)
    with EDITLOG_PATH.open("a", encoding="utf-8") as f:
        f.write(json.dumps(entry, ensure_ascii=False) + "\n")


# ─── Form parsing ────────────────────────────────────────────────────────

# Allow only a small set of resource kinds so the tag classes in CSS
# stay finite. "other" is the catch-all when a kind doesn't fit.
ALLOWED_KINDS = (
    "atwiki", "mizuumi", "wiki", "homepage", "wayback", "discord",
    "gamefaqs", "store", "download", "video", "tutorial", "other",
)

_URL_RE = re.compile(r"^https?://", re.IGNORECASE)


def _normalize_resources(form: dict[str, Any]) -> list[dict[str, Any]]:
    """Pull resource_<i>_<field> entries out of the form dict and
    reassemble them into a clean list. Drops rows where url is empty
    so 'add a row' UX doesn't litter the registry with blanks."""
    rows: dict[int, dict[str, Any]] = {}
    for key, val in form.items():
        m = re.match(r"^resource_(\d+)_(\w+)$", key)
        if not m:
            continue
        idx = int(m.group(1))
        field = m.group(2)
        rows.setdefault(idx, {})[field] = val.strip() if isinstance(val, str) else val

    out: list[dict[str, Any]] = []
    for idx in sorted(rows.keys()):
        r = rows[idx]
        url = r.get("url", "")
        if not url:
            continue  # drop blank rows
        if not _URL_RE.match(url):
            # Reject non-http(s) — javascript: / data: shouldn't ever
            # land in the registry.
            continue
        kind = r.get("kind", "other")
        if kind not in ALLOWED_KINDS:
            kind = "other"
        clean: dict[str, Any] = {
            "kind":   kind,
            "url":    url,
            "source": r.get("source") or "manual",
        }
        # Optional fields — only persist when non-empty.
        for fld in ("name", "label", "desc", "archive_url"):
            v = r.get(fld, "")
            if v:
                clean[fld] = v
        out.append(clean)
    return out


# ─── Router ──────────────────────────────────────────────────────────────

router = APIRouter(prefix="/admin", tags=["admin"])

# templates is set by app.py at mount time so we share the same Jinja
# environment (globals like friendly_name, banner_color, etc.).
_templates: Optional[Jinja2Templates] = None


def attach_templates(tpls: Jinja2Templates) -> None:
    global _templates
    _templates = tpls


def _editor_or_redirect(request: Request, fm2k_admin: Optional[str]
                        ) -> tuple[Optional[dict[str, Any]],
                                   Optional[RedirectResponse]]:
    """Return (editor_record, None) on auth, (None, redirect) when the
    cookie is missing or invalid. A bearer header overrides the cookie
    so future API clients work without setting cookies."""
    bearer = request.headers.get("authorization", "")
    if bearer.lower().startswith("bearer "):
        token = bearer.split(" ", 1)[1].strip()
    else:
        token = fm2k_admin or ""
    editor = _check_token(token)
    if not editor:
        return None, RedirectResponse(
            url=f"/admin/login?next={request.url.path}",
            status_code=303)
    return editor, None


# ─── JSON API helpers ────────────────────────────────────────────────────

def _editor_or_401(request: Request, fm2k_admin: Optional[str]
                   ) -> dict[str, Any]:
    """API-friendly auth: returns the editor record on success, raises
    HTTPException(401) on auth failure. Form-based routes use the
    cookie-redirect helper; this one is for fetch() callers that want
    a JSON error instead of a 303."""
    bearer = request.headers.get("authorization", "")
    if bearer.lower().startswith("bearer "):
        token = bearer.split(" ", 1)[1].strip()
    else:
        token = fm2k_admin or ""
    editor = _check_token(token)
    if not editor:
        raise HTTPException(401, "auth required")
    return editor


# Fields the inline editor is allowed to PATCH on a game record. Any
# others in the request body are silently ignored — keeps the API
# surface narrow even if a confused client sends extra junk.
_ALLOWED_PATCH_FIELDS = (
    # Display
    "name", "description", "year", "developer", "publisher",
    "alt_names",
    # Canonical URLs
    "homepage", "wayback_homepage", "purchase_url", "download_url",
    "banner_url", "thumb_url",
    # Engine + atwiki link
    "engine", "atwiki_id", "atwiki_url", "atwiki_match_method",
    # Identity
    "kgt_filename", "exe_stems", "sources", "characters", "stages",
    "aliased_ids",
    # Status / provenance
    "archive_status", "imported_from", "created_by",
    # Community-held copies — list of editor nicks who have local bytes
    # and are willing to share. Distinct from archive_status which is
    # the auto-audit verdict for the SERVER's filesystem only.
    "held_by",
)

# List-typed fields parse the same way as alt_names (CSV / newline /
# JSON array). Keeps the form input simple — editors paste a comma-
# separated list and we split it.
_LIST_PATCH_FIELDS = ("alt_names", "exe_stems", "sources",
                      "characters", "stages", "aliased_ids", "held_by")

# Manual archive_status overrides editors can pick. Computed values
# from the auto-audit (HAVE_LOCAL, HAVE_ARCHIVE_BYTES, etc.) are also
# valid — anything in this set passes the PATCH validator.
_ALLOWED_STATUSES = (
    "HAVE_LOCAL", "HAVE_ARCHIVE_BYTES", "KNOWN_RECOVERABLE",
    "NEEDS_HUNT", "NO_OUTBOUND",
)


def _normalize_alt_names(value: Any) -> list[str]:
    """alt_names can come in as a list (JSON array) or a string
    (comma/newline-separated). Normalize to list[str], dropping
    blanks."""
    if isinstance(value, list):
        return [str(s).strip() for s in value if str(s).strip()]
    if isinstance(value, str):
        return [s.strip() for s in re.split(r"[,\n]", value) if s.strip()]
    return []


def _to_diff(before: dict[str, Any], after: dict[str, Any]) -> dict[str, Any]:
    """Compact field-level diff for the audit log. Different from
    _diff_record (which summarizes resources[]); this one is for the
    PATCH path where only a few scalar fields change at a time."""
    diff: dict[str, Any] = {}
    for k in set(before) | set(after):
        if before.get(k) != after.get(k):
            diff[k] = {"before": before.get(k), "after": after.get(k)}
    return diff


@router.patch("/api/g/{game_id}")
async def api_patch_game(request: Request, game_id: str,
                         payload: dict = Body(...),
                         fm2k_admin: Optional[str] = Cookie(None)):
    """Partial update of a game record. Body is a JSON object of
    {field: new_value} pairs; only fields in _ALLOWED_PATCH_FIELDS are
    applied. Returns {ok, changed: [field, ...]} or {ok, noop: true}."""
    editor = _editor_or_401(request, fm2k_admin)

    with _registry_lock():
        records = _read_registry()
        idx = next((i for i, r in enumerate(records)
                    if r.get("game_id") == game_id), -1)
        if idx < 0:
            raise HTTPException(404, "game_id not in registry")
        before = records[idx]
        after = dict(before)

        for k, v in payload.items():
            if k not in _ALLOWED_PATCH_FIELDS:
                continue
            if k in _LIST_PATCH_FIELDS:
                after[k] = _normalize_alt_names(v)
            elif k == "archive_status":
                # Reject unknown statuses; CSS classes + filter chips
                # only handle the canonical 5. Allow empty string to
                # clear the field back to "no audit verdict".
                vstr = str(v).strip().upper() if v is not None else ""
                if vstr and vstr not in _ALLOWED_STATUSES:
                    raise HTTPException(400,
                        f"archive_status must be one of {_ALLOWED_STATUSES}")
                after[k] = vstr
            elif isinstance(v, str):
                after[k] = v.strip()
            else:
                after[k] = v

        diff = _to_diff(before, after)
        if not diff:
            return {"ok": True, "noop": True}

        records[idx] = after
        bak = _atomic_write_registry(records)
        _append_editlog({
            "ts":        datetime.now(timezone.utc).isoformat(),
            "editor":    editor.get("nick", "?"),
            "action":    "patch",
            "game_id":   game_id,
            "diff":      diff,
            "bak":       bak.name,
            "client_ip": request.client.host if request.client else "",
        })
    return {"ok": True, "changed": list(diff.keys())}


@router.post("/api/g/{game_id}/resource")
async def api_add_resource_json(request: Request, game_id: str,
                                payload: dict = Body(...),
                                fm2k_admin: Optional[str] = Cookie(None)):
    """Append a resource. Body: {kind, url, label?, desc?, archive_url?}.
    Idempotent on (kind, url). Returns {ok, idx, resource}."""
    editor = _editor_or_401(request, fm2k_admin)

    url = (payload.get("url") or "").strip()
    if not url or not _URL_RE.match(url):
        raise HTTPException(400, "url must be http(s)://...")
    kind = payload.get("kind") or "other"
    if kind not in ALLOWED_KINDS:
        kind = "other"
    new: dict[str, Any] = {"kind": kind, "url": url, "source": "manual"}
    for fld in ("label", "desc", "archive_url"):
        v = (payload.get(fld) or "").strip() if isinstance(
            payload.get(fld), str) else payload.get(fld)
        if v:
            new[fld] = v

    with _registry_lock():
        records = _read_registry()
        idx = next((i for i, r in enumerate(records)
                    if r.get("game_id") == game_id), -1)
        if idx < 0:
            raise HTTPException(404, "game_id not in registry")
        rec = records[idx]
        existing = rec.get("resources") or []
        for j, ex in enumerate(existing):
            if ex.get("kind") == kind and ex.get("url") == url:
                return {"ok": True, "idx": j, "resource": ex,
                        "duplicate": True}
        existing.append(new)
        rec["resources"] = existing
        records[idx] = rec
        bak = _atomic_write_registry(records)
        _append_editlog({
            "ts":        datetime.now(timezone.utc).isoformat(),
            "editor":    editor.get("nick", "?"),
            "action":    "add_resource",
            "game_id":   game_id,
            "added":     new,
            "bak":       bak.name,
            "client_ip": request.client.host if request.client else "",
        })
    return {"ok": True, "idx": len(existing) - 1, "resource": new}


@router.delete("/api/g/{game_id}/resource/{ridx}")
async def api_delete_resource(request: Request, game_id: str, ridx: int,
                              fm2k_admin: Optional[str] = Cookie(None)):
    editor = _editor_or_401(request, fm2k_admin)
    with _registry_lock():
        records = _read_registry()
        gidx = next((i for i, r in enumerate(records)
                     if r.get("game_id") == game_id), -1)
        if gidx < 0:
            raise HTTPException(404, "game_id not in registry")
        existing = records[gidx].get("resources") or []
        if ridx < 0 or ridx >= len(existing):
            raise HTTPException(404, "resource index out of range")
        removed = existing.pop(ridx)
        records[gidx]["resources"] = existing
        bak = _atomic_write_registry(records)
        _append_editlog({
            "ts":        datetime.now(timezone.utc).isoformat(),
            "editor":    editor.get("nick", "?"),
            "action":    "remove_resource",
            "game_id":   game_id,
            "removed":   removed,
            "bak":       bak.name,
            "client_ip": request.client.host if request.client else "",
        })
    return {"ok": True, "removed": removed}


@router.patch("/api/g/{game_id}/resource/{ridx}")
async def api_patch_resource(request: Request, game_id: str, ridx: int,
                             payload: dict = Body(...),
                             fm2k_admin: Optional[str] = Cookie(None)):
    """Partial update of a resource by index. Body: {field: value}.
    Allowed fields: kind, url, label, desc, archive_url."""
    editor = _editor_or_401(request, fm2k_admin)
    allowed = ("kind", "url", "label", "desc", "archive_url")
    with _registry_lock():
        records = _read_registry()
        gidx = next((i for i, r in enumerate(records)
                     if r.get("game_id") == game_id), -1)
        if gidx < 0:
            raise HTTPException(404, "game_id not in registry")
        existing = records[gidx].get("resources") or []
        if ridx < 0 or ridx >= len(existing):
            raise HTTPException(404, "resource index out of range")
        before = dict(existing[ridx])
        after = dict(before)
        for k, v in payload.items():
            if k not in allowed:
                continue
            if k == "url" and v and not _URL_RE.match(str(v)):
                raise HTTPException(400, "url must be http(s)://...")
            if k == "kind" and v not in ALLOWED_KINDS:
                v = "other"
            if isinstance(v, str):
                v = v.strip()
            if v:
                after[k] = v
            else:
                after.pop(k, None)
        diff = _to_diff(before, after)
        if not diff:
            return {"ok": True, "noop": True}
        existing[ridx] = after
        records[gidx]["resources"] = existing
        bak = _atomic_write_registry(records)
        _append_editlog({
            "ts":        datetime.now(timezone.utc).isoformat(),
            "editor":    editor.get("nick", "?"),
            "action":    "patch_resource",
            "game_id":   game_id,
            "ridx":      ridx,
            "diff":      diff,
            "bak":       bak.name,
            "client_ip": request.client.host if request.client else "",
        })
    return {"ok": True, "resource": after}


@router.post("/api/g")
async def api_create_game(request: Request,
                          payload: dict = Body(...),
                          fm2k_admin: Optional[str] = Cookie(None)):
    """Create a new registry row. Body: {game_id, name, ...}."""
    editor = _editor_or_401(request, fm2k_admin)
    name = (payload.get("name") or "").strip()
    gid = (payload.get("game_id") or "").strip()
    if not name:
        raise HTTPException(400, "name required")
    if not gid or not _GAMEID_RE.match(gid):
        raise HTTPException(400, "game_id must match [a-z0-9-]+")

    with _registry_lock():
        records = _read_registry()
        if any(r.get("game_id") == gid for r in records):
            raise HTTPException(409, "game_id already exists")
        new_row: dict[str, Any] = {
            "game_id":      gid,
            "name":         name,
            "alt_names":    _normalize_alt_names(payload.get("alt_names")),
            "engine":       (payload.get("engine") or "FM2K"),
            "year":         (payload.get("year") or "").strip()
                            if isinstance(payload.get("year"), str)
                            else (payload.get("year") or ""),
            "developer":    (payload.get("developer") or "").strip()
                            if isinstance(payload.get("developer"), str)
                            else "",
            "publisher":    (payload.get("publisher") or "").strip()
                            if isinstance(payload.get("publisher"), str)
                            else "",
            "exe_stems":    [gid],
            "kgt_filename": "",
            "homepage":     (payload.get("homepage") or "").strip()
                            if isinstance(payload.get("homepage"), str)
                            else "",
            "download_url": "",
            "banner_url":   "",
            "thumb_url":    "",
            "sources":      ["manual"],
            "characters":   [],
            "stages":       [],
            "_raw":         {},
            "description":  (payload.get("description") or "").strip()
                            if isinstance(payload.get("description"), str)
                            else "",
            "purchase_url": (payload.get("purchase_url") or "").strip()
                            if isinstance(payload.get("purchase_url"), str)
                            else "",
            "resources":    [],
            "versions":     [],
            "created_by":   editor.get("nick", "?"),
            "imported_from": "manual",
        }
        aid = (payload.get("atwiki_id") or "").strip() if isinstance(
            payload.get("atwiki_id"), str) else ""
        if aid and aid.isdigit():
            new_row["atwiki_id"] = aid
            new_row["atwiki_url"] = (
                f"https://w.atwiki.jp/arunau32167/pages/{aid}.html")
        records.append(new_row)
        bak = _atomic_write_registry(records)
        _append_editlog({
            "ts":        datetime.now(timezone.utc).isoformat(),
            "editor":    editor.get("nick", "?"),
            "action":    "create",
            "game_id":   gid,
            "row":       {"name": name, "engine": new_row["engine"]},
            "bak":       bak.name,
            "client_ip": request.client.host if request.client else "",
        })
    return {"ok": True, "game_id": gid, "row": new_row}


@router.delete("/api/g/{game_id}")
async def api_delete_game(request: Request, game_id: str,
                          confirm: str = "",
                          fm2k_admin: Optional[str] = Cookie(None)):
    editor = _editor_or_401(request, fm2k_admin)
    if confirm != game_id:
        raise HTTPException(400, "confirm must equal game_id")
    with _registry_lock():
        records = _read_registry()
        gidx = next((i for i, r in enumerate(records)
                     if r.get("game_id") == game_id), -1)
        if gidx < 0:
            raise HTTPException(404, "game_id not in registry")
        removed = records.pop(gidx)
        bak = _atomic_write_registry(records)
        _append_editlog({
            "ts":        datetime.now(timezone.utc).isoformat(),
            "editor":    editor.get("nick", "?"),
            "action":    "delete",
            "game_id":   game_id,
            "removed":   {"name": removed.get("name"),
                          "atwiki_id": removed.get("atwiki_id")},
            "bak":       bak.name,
            "client_ip": request.client.host if request.client else "",
        })
    return {"ok": True}


@router.post("/api/g/{game_id}/merge")
async def api_merge(request: Request, game_id: str,
                    payload: dict = Body(...),
                    fm2k_admin: Optional[str] = Cookie(None)):
    editor = _editor_or_401(request, fm2k_admin)
    target_id = (payload.get("target_id") or "").strip()
    direction = payload.get("direction", "into_target")
    if game_id == target_id:
        raise HTTPException(400, "cannot merge a row with itself")
    if direction not in ("into_target", "into_self"):
        raise HTTPException(400, "bad direction")
    with _registry_lock():
        records = _read_registry()
        si = next((i for i, r in enumerate(records)
                   if r.get("game_id") == game_id), -1)
        ti = next((i for i, r in enumerate(records)
                   if r.get("game_id") == target_id), -1)
        if si < 0 or ti < 0:
            raise HTTPException(404, "one or both game_ids missing")
        keep_idx, drop_idx = (ti, si) if direction == "into_target" else (si, ti)
        keep = records[keep_idx]
        drop = records[drop_idx]
        merged = _merge_records(keep, drop)
        records[keep_idx] = merged
        drop_gid = drop.get("game_id")
        di = next((i for i, r in enumerate(records)
                   if r.get("game_id") == drop_gid), -1)
        if di >= 0:
            records.pop(di)
        bak = _atomic_write_registry(records)
        _append_editlog({
            "ts":        datetime.now(timezone.utc).isoformat(),
            "editor":    editor.get("nick", "?"),
            "action":    "merge",
            "kept":      keep.get("game_id"),
            "dropped":   drop.get("game_id"),
            "bak":       bak.name,
            "client_ip": request.client.host if request.client else "",
        })
    return {"ok": True, "kept_id": merged["game_id"]}


# ─── Versions API ────────────────────────────────────────────────────────
#
# Versions track distinct builds of a game. Two sources:
#   1. Hash-tracked: `versions[]` rows added by tools/atwiki_crossref.py
#      with xxh64 + size_bytes + exe_path. The crossref pipeline runs
#      against archive bytes we pulled or local installs we scanned.
#   2. Manual: editors add `{label: "v0.1"}` for builds they've seen but
#      we don't have bytes for. xxh64 is optional and gets backfilled
#      if/when bytes show up.
#
# Each version also has its own held_by[] so editors can record
# "AndreaJens has v0.1, wak has v2" — finer-grained than the per-game
# held_by which just says "someone has SOME version".

_VERSION_FIELDS = ("label", "xxh64", "size_bytes", "source",
                   "exe_path", "from_archive", "match_known", "notes")


def _normalize_version_payload(payload: dict[str, Any]) -> dict[str, Any]:
    out: dict[str, Any] = {}
    for k in _VERSION_FIELDS:
        if k not in payload:
            continue
        v = payload[k]
        if isinstance(v, str):
            v = v.strip()
        if v in ("", None):
            continue
        if k == "size_bytes":
            try:
                v = int(v)
            except (TypeError, ValueError):
                continue
        if k == "xxh64":
            # Normalize to "0x" + uppercase hex; accept "abcdef" or
            # "0xABCDEF" or even with spaces.
            s = str(v).strip().replace(" ", "").upper()
            if s.startswith("0X"):
                s = s[2:]
            if not all(c in "0123456789ABCDEF" for c in s):
                raise HTTPException(400, "xxh64 must be hex digits")
            v = "0x" + s.zfill(16)
        out[k] = v
    return out


@router.post("/api/g/{game_id}/version")
async def api_add_version(request: Request, game_id: str,
                          payload: dict = Body(...),
                          fm2k_admin: Optional[str] = Cookie(None)):
    """Append a manual version row. Body: {label, xxh64?, size_bytes?,
    source?, notes?}. The label is required; everything else is
    optional. Returns {ok, idx, version}."""
    editor = _editor_or_401(request, fm2k_admin)
    clean = _normalize_version_payload(payload)
    if not clean.get("label"):
        raise HTTPException(400, "label is required (e.g. 'v0.1', 'final')")
    clean.setdefault("source", "manual")
    clean.setdefault("held_by", [])
    clean["added_by"] = editor.get("nick", "?")

    with _registry_lock():
        records = _read_registry()
        idx = next((i for i, r in enumerate(records)
                    if r.get("game_id") == game_id), -1)
        if idx < 0:
            raise HTTPException(404, "game_id not in registry")
        rec = records[idx]
        existing = rec.get("versions") or []
        # Idempotency: collapse on (xxh64) if provided, else on (label).
        if clean.get("xxh64"):
            for j, ex in enumerate(existing):
                if ex.get("xxh64") == clean["xxh64"]:
                    return {"ok": True, "idx": j, "version": ex,
                            "duplicate": True}
        for j, ex in enumerate(existing):
            if ex.get("label") == clean["label"] and not ex.get("xxh64"):
                return {"ok": True, "idx": j, "version": ex,
                        "duplicate": True}
        existing.append(clean)
        rec["versions"] = existing
        records[idx] = rec
        bak = _atomic_write_registry(records)
        _append_editlog({
            "ts":        datetime.now(timezone.utc).isoformat(),
            "editor":    editor.get("nick", "?"),
            "action":    "add_version",
            "game_id":   game_id,
            "added":     clean,
            "bak":       bak.name,
            "client_ip": request.client.host if request.client else "",
        })
    return {"ok": True, "idx": len(existing) - 1, "version": clean}


@router.patch("/api/g/{game_id}/version/{vidx}")
async def api_patch_version(request: Request, game_id: str, vidx: int,
                            payload: dict = Body(...),
                            fm2k_admin: Optional[str] = Cookie(None)):
    editor = _editor_or_401(request, fm2k_admin)
    delta = _normalize_version_payload(payload)
    if not delta:
        return {"ok": True, "noop": True}
    with _registry_lock():
        records = _read_registry()
        gidx = next((i for i, r in enumerate(records)
                     if r.get("game_id") == game_id), -1)
        if gidx < 0:
            raise HTTPException(404, "game_id not in registry")
        existing = records[gidx].get("versions") or []
        if vidx < 0 or vidx >= len(existing):
            raise HTTPException(404, "version index out of range")
        before = dict(existing[vidx])
        after = dict(before)
        for k, v in delta.items():
            after[k] = v
        if after == before:
            return {"ok": True, "noop": True}
        existing[vidx] = after
        records[gidx]["versions"] = existing
        bak = _atomic_write_registry(records)
        _append_editlog({
            "ts":        datetime.now(timezone.utc).isoformat(),
            "editor":    editor.get("nick", "?"),
            "action":    "patch_version",
            "game_id":   game_id,
            "vidx":      vidx,
            "diff":      _to_diff(before, after),
            "bak":       bak.name,
            "client_ip": request.client.host if request.client else "",
        })
    return {"ok": True, "version": after}


@router.delete("/api/g/{game_id}/version/{vidx}")
async def api_delete_version(request: Request, game_id: str, vidx: int,
                             fm2k_admin: Optional[str] = Cookie(None)):
    editor = _editor_or_401(request, fm2k_admin)
    with _registry_lock():
        records = _read_registry()
        gidx = next((i for i, r in enumerate(records)
                     if r.get("game_id") == game_id), -1)
        if gidx < 0:
            raise HTTPException(404, "game_id not in registry")
        existing = records[gidx].get("versions") or []
        if vidx < 0 or vidx >= len(existing):
            raise HTTPException(404, "version index out of range")
        removed = existing.pop(vidx)
        records[gidx]["versions"] = existing
        bak = _atomic_write_registry(records)
        _append_editlog({
            "ts":        datetime.now(timezone.utc).isoformat(),
            "editor":    editor.get("nick", "?"),
            "action":    "remove_version",
            "game_id":   game_id,
            "removed":   removed,
            "bak":       bak.name,
            "client_ip": request.client.host if request.client else "",
        })
    return {"ok": True, "removed": removed}


@router.post("/api/g/{game_id}/version/{vidx}/held/toggle")
async def api_toggle_version_held(request: Request, game_id: str, vidx: int,
                                  fm2k_admin: Optional[str] = Cookie(None)):
    """Per-version "I hold this build" toggle. Distinct from the
    per-game held_by — lets editors flag exactly which builds they
    have when multiple coexist (v0.1 vs v2 etc)."""
    editor = _editor_or_401(request, fm2k_admin)
    nick = editor.get("nick") or "?"
    with _registry_lock():
        records = _read_registry()
        gidx = next((i for i, r in enumerate(records)
                     if r.get("game_id") == game_id), -1)
        if gidx < 0:
            raise HTTPException(404, "game_id not in registry")
        existing = records[gidx].get("versions") or []
        if vidx < 0 or vidx >= len(existing):
            raise HTTPException(404, "version index out of range")
        ver = existing[vidx]
        held = list(ver.get("held_by") or [])
        if nick in held:
            held = [n for n in held if n != nick]
            you_hold = False
            action = "release_version_held"
        else:
            held.append(nick)
            you_hold = True
            action = "claim_version_held"
        ver["held_by"] = held
        existing[vidx] = ver
        records[gidx]["versions"] = existing
        bak = _atomic_write_registry(records)
        _append_editlog({
            "ts":        datetime.now(timezone.utc).isoformat(),
            "editor":    nick,
            "action":    action,
            "game_id":   game_id,
            "vidx":      vidx,
            "held_by":   held,
            "bak":       bak.name,
            "client_ip": request.client.host if request.client else "",
        })
    return {"ok": True, "held_by": held, "you_hold": you_hold}


@router.post("/api/g/{game_id}/rename")
async def api_rename_game(request: Request, game_id: str,
                          payload: dict = Body(...),
                          fm2k_admin: Optional[str] = Cookie(None)):
    """Rename the primary game_id of a row.

    The old game_id is preserved in aliased_ids[] so existing match
    history (matches.json keys by game_id; we don't rewrite it) keeps
    aggregating under the renamed entry. Useful for cleaning up
    placeholder slugs like `atwiki-764` to a human-friendly name.

    Body: {new_id: "..."}.

    Validates:
      - new_id matches _GAMEID_RE
      - new_id != current game_id
      - new_id doesn't collide with another row's primary game_id
        OR aliased_ids
    """
    editor = _editor_or_401(request, fm2k_admin)
    new_id = (payload.get("new_id") or "").strip().lower()
    if not new_id or not _GAMEID_RE.match(new_id):
        raise HTTPException(400, "new_id must match [a-z0-9][a-z0-9-]+")
    if new_id == game_id:
        raise HTTPException(400, "new_id is the same as current game_id")

    with _registry_lock():
        records = _read_registry()
        idx = next((i for i, r in enumerate(records)
                    if r.get("game_id") == game_id), -1)
        if idx < 0:
            raise HTTPException(404, "game_id not in registry")
        # Collision check: another row's primary or aliased_ids.
        for j, r in enumerate(records):
            if j == idx:
                continue
            if r.get("game_id") == new_id:
                raise HTTPException(409, f"new_id already in use by {r.get('game_id')}")
            if new_id in (r.get("aliased_ids") or []):
                raise HTTPException(409,
                    f"new_id already aliased on {r.get('game_id')}")

        rec = records[idx]
        old_id = rec.get("game_id")
        aliased = list(rec.get("aliased_ids") or [])
        if old_id and old_id not in aliased:
            aliased.append(old_id)
        rec["game_id"] = new_id
        rec["aliased_ids"] = aliased
        records[idx] = rec
        bak = _atomic_write_registry(records)
        _append_editlog({
            "ts":        datetime.now(timezone.utc).isoformat(),
            "editor":    editor.get("nick", "?"),
            "action":    "rename",
            "old_id":    old_id,
            "new_id":    new_id,
            "aliased":   aliased,
            "bak":       bak.name,
            "client_ip": request.client.host if request.client else "",
        })
    return {"ok": True, "old_id": old_id, "new_id": new_id,
            "aliased_ids": aliased}


@router.post("/api/g/{game_id}/held/toggle")
async def api_toggle_held(request: Request, game_id: str,
                          fm2k_admin: Optional[str] = Cookie(None)):
    """Toggle the current editor's nick in held_by[] for this game.
    Each editor calls this to flag "I have local bytes of this game".
    Returns the updated held_by list and whether the caller now holds.

    held_by is a community-owned counterpart to archive_status —
    archive_status reflects what the auto-audit sees on the SERVER's
    filesystem, while held_by reflects which editors out in the world
    have copies they could share. Multiple editors can hold the same
    game; the list is order-preserving.
    """
    editor = _editor_or_401(request, fm2k_admin)
    nick = editor.get("nick") or "?"

    with _registry_lock():
        records = _read_registry()
        idx = next((i for i, r in enumerate(records)
                    if r.get("game_id") == game_id), -1)
        if idx < 0:
            raise HTTPException(404, "game_id not in registry")
        rec = records[idx]
        held = list(rec.get("held_by") or [])
        you_hold = nick in held
        if you_hold:
            held = [n for n in held if n != nick]
            you_hold = False
            action = "release_held"
        else:
            held.append(nick)
            you_hold = True
            action = "claim_held"
        rec["held_by"] = held
        records[idx] = rec
        bak = _atomic_write_registry(records)
        _append_editlog({
            "ts":        datetime.now(timezone.utc).isoformat(),
            "editor":    nick,
            "action":    action,
            "game_id":   game_id,
            "held_by":   held,
            "bak":       bak.name,
            "client_ip": request.client.host if request.client else "",
        })
    return {"ok": True, "held_by": held, "you_hold": you_hold}


@router.get("/api/search")
async def api_search(request: Request, q: str = "",
                     fm2k_admin: Optional[str] = Cookie(None)):
    """Lightweight search for the merge picker — returns up to 20
    {game_id, name, atwiki_id} tuples matching `q`."""
    _ = _editor_or_401(request, fm2k_admin)
    if not q or len(q) < 2:
        return {"results": []}
    qn = q.lower()
    records = _read_registry()
    out: list[dict[str, Any]] = []
    for r in records:
        hay = ((r.get("name") or "") + " " + (r.get("game_id") or "") + " " +
               " ".join(r.get("alt_names") or [])).lower()
        if qn in hay:
            out.append({"game_id": r.get("game_id"),
                        "name": r.get("name"),
                        "atwiki_id": r.get("atwiki_id"),
                        "n_resources": len(r.get("resources") or [])})
            if len(out) >= 20:
                break
    return {"results": out}


# ─── Login ───────────────────────────────────────────────────────────────

@router.get("/login", response_class=HTMLResponse)
def login_page(request: Request, next: str = "/admin", err: str = ""):
    return _templates.TemplateResponse(
        "admin_login.html",
        {"request": request, "next": next, "err": err})


@router.post("/login")
def login_submit(request: Request,
                 token: str = Form(...),
                 next: str = Form("/admin")):
    editor = _check_token(token.strip())
    if not editor:
        return RedirectResponse(
            url=f"/admin/login?next={next}&err=invalid",
            status_code=303)
    # Sanitize next so an open-redirect can't bounce off /admin/login.
    if not next.startswith("/") or next.startswith("//"):
        next = "/admin"
    resp = RedirectResponse(url=next, status_code=303)
    # HTTPS detection: if behind a reverse proxy with X-Forwarded-Proto
    # set, honor it; else fall back to request.url.scheme. The deploy
    # is expected to terminate TLS at nginx so Secure should be set in
    # production. For local dev the cookie still works without Secure.
    proto = request.headers.get("x-forwarded-proto", request.url.scheme)
    resp.set_cookie(
        key="fm2k_admin",
        value=token.strip(),
        httponly=True,
        samesite="strict",
        secure=(proto == "https"),
        max_age=60 * 60 * 24 * 30,  # 30 days
        path="/admin",
    )
    return resp


@router.post("/logout")
def logout(request: Request):
    resp = RedirectResponse(url="/admin/login", status_code=303)
    resp.delete_cookie("fm2k_admin", path="/admin")
    return resp


# ─── Index — list editable games ─────────────────────────────────────────

# Allowlist for sort columns — anything else falls back to "name".
# Mapped to the sort key so we never eval user-supplied strings.
_SORT_KEYS: dict[str, Any] = {
    "game_id":        lambda r: r["game_id"].lower(),
    "name":           lambda r: (r["name"] or r["game_id"]).lower(),
    "engine":         lambda r: r["engine"] or "",
    "year":           lambda r: r["year"] or "",
    "developer":      lambda r: (r.get("developer") or "").lower(),
    "publisher":      lambda r: (r.get("publisher") or "").lower(),
    "n_resources":    lambda r: r["n_resources"],
    "n_versions":     lambda r: r["n_versions"],
    "atwiki_id":      lambda r: int(r["atwiki_id"]) if str(r["atwiki_id"]).isdigit() else 0,
    "archive_status": lambda r: r["archive_status"] or "",
}


# Status values rendered as filter chips. Order is stable so the
# chip row doesn't reshuffle on every render.
_FILTER_STATUSES = (
    "HAVE_LOCAL",
    "HAVE_ARCHIVE_BYTES",
    "KNOWN_RECOVERABLE",
    "NEEDS_HUNT",
    "NO_OUTBOUND",
)


@router.get("", response_class=HTMLResponse)
@router.get("/", response_class=HTMLResponse)
def admin_index(request: Request,
                fm2k_admin: Optional[str] = Cookie(None),
                q: str = "",
                sort: str = "name",
                dir: str = "asc",
                status: str = "",
                dev: str = "",
                engine: str = ""):
    editor, redirect = _editor_or_redirect(request, fm2k_admin)
    if redirect:
        return redirect
    try:
        records = _read_registry()
    except (OSError, json.JSONDecodeError):
        records = []
    # Parse status filter — comma-separated list. Empty means "show
    # everything". Allowed values come from _FILTER_STATUSES; unknowns
    # are silently dropped to keep the URL surface narrow.
    status_set: set[str] = set()
    if status:
        status_set = {s for s in status.split(",")
                      if s in _FILTER_STATUSES}

    # Compute developer + engine option lists from the FULL registry
    # (so dropdowns don't shrink as you narrow the result set). Sorted
    # by count desc so prolific devs surface first.
    dev_counts: Counter = Counter()
    engine_counts: Counter = Counter()
    for r in records:
        d = (r.get("developer") or "").strip()
        if d:
            dev_counts[d] += 1
        e = r.get("engine") or ""
        if e:
            engine_counts[e] += 1
    dev_options = [{"name": d, "count": c}
                   for d, c in dev_counts.most_common()]
    engine_options = [{"name": e, "count": c}
                      for e, c in engine_counts.most_common()]

    rows = []
    for r in records:
        gid = r.get("game_id", "")
        if q:
            # Search across name + id + alt_names + developer + publisher
            # so editors can find an entry by author or studio name.
            hay = (r.get("name", "") + " " + gid + " " +
                   " ".join(r.get("alt_names") or []) + " " +
                   (r.get("developer") or "") + " " +
                   (r.get("publisher") or "")).lower()
            if q.lower() not in hay:
                continue
        if status_set and r.get("archive_status", "") not in status_set:
            continue
        if dev and (r.get("developer") or "") != dev:
            continue
        if engine and (r.get("engine") or "") != engine:
            continue
        # Compact resource view for the index — group by kind, keep
        # only those a reviewer cares to see at a glance (store /
        # purchase / mizuumi / discord / homepage). The full list is
        # always one click away on the per-game editor.
        res_compact: list[dict[str, str]] = []
        for src in (r.get("resources") or []):
            res_compact.append({
                "kind":        src.get("kind") or "other",
                "url":         src.get("url") or "",
                "label":       src.get("label") or src.get("name") or "",
                "desc":        src.get("desc") or "",
                "archive_url": src.get("archive_url") or "",
                "preferred":   src.get("preferred") or "",
            })
        rows.append({
            "game_id":         gid,
            "name":            r.get("name", gid),
            "engine":          r.get("engine", ""),
            "year":            r.get("year", ""),
            "developer":       r.get("developer", ""),
            "publisher":       r.get("publisher", ""),
            "n_resources":     len(r.get("resources") or []),
            "n_versions":      len(r.get("versions") or []),
            "atwiki_id":       r.get("atwiki_id", ""),
            "archive_status":  r.get("archive_status", ""),
            "resources":       res_compact,
            "purchase_url":    r.get("purchase_url", ""),
            "held_by":         r.get("held_by") or [],
            "versions":        r.get("versions") or [],
        })

    # Sort. Unknown column → name; unknown dir → asc.
    if sort not in _SORT_KEYS:
        sort = "name"
    if dir not in ("asc", "desc"):
        dir = "asc"
    rows.sort(key=_SORT_KEYS[sort], reverse=(dir == "desc"))

    return _templates.TemplateResponse(
        "admin_index.html",
        {"request":         request,
         "rows":            rows,
         "q":               q,
         "editor":          editor,
         "editor_nick":     editor.get("nick", "?") if editor else "?",
         "sort":            sort,
         "dir":             dir,
         "dev":             dev,
         "engine":          engine,
         "columns":         list(_SORT_KEYS.keys()),
         "filter_statuses":  _FILTER_STATUSES,
         "active_statuses":  status_set,
         "dev_options":     dev_options,
         "engine_options":  engine_options,
         "ALLOWED_KINDS":    ALLOWED_KINDS,
         "ALLOWED_STATUSES": _ALLOWED_STATUSES})


# ─── Create new game ─────────────────────────────────────────────────────

# game_id must be a clean slug — primary key, lower-case, hyphens, no
# punctuation. Same shape we generate for atwiki imports so manual
# additions blend in with the rest of the registry.
_GAMEID_RE = re.compile(r"^[a-z0-9][a-z0-9-]{1,79}$")


@router.get("/new", response_class=HTMLResponse)
def new_form(request: Request,
             fm2k_admin: Optional[str] = Cookie(None),
             err: str = "",
             prefill_name: str = ""):
    editor, redirect = _editor_or_redirect(request, fm2k_admin)
    if redirect:
        return redirect
    return _templates.TemplateResponse(
        "admin_new_game.html",
        {"request": request, "editor": editor, "err": err,
         "prefill_name": prefill_name})


@router.post("/new")
async def new_submit(request: Request,
                     fm2k_admin: Optional[str] = Cookie(None)):
    editor, redirect = _editor_or_redirect(request, fm2k_admin)
    if redirect:
        return redirect
    form = await request.form()
    f = {k: (v.strip() if isinstance(v, str) else v) for k, v in form.items()}

    name = f.get("name", "")
    game_id = f.get("game_id", "")
    if not name:
        return RedirectResponse(
            url=f"/admin/new?err=name+required",
            status_code=303)
    if not game_id or not _GAMEID_RE.match(game_id):
        return RedirectResponse(
            url=f"/admin/new?err=game_id+must+match+%5Ba-z0-9-%5D&prefill_name={name}",
            status_code=303)

    with _registry_lock():
        records = _read_registry()
        if any(r.get("game_id") == game_id for r in records):
            return RedirectResponse(
                url=f"/admin/new?err=game_id+already+exists&prefill_name={name}",
                status_code=303)

        # Build a registry-shaped row. Same shape as atwiki imports +
        # a `created_by` nick so we can grep manual additions later.
        alt_names = [s.strip() for s in re.split(r"[,\n]",
                     f.get("alt_names", "")) if s.strip()]
        new_row: dict[str, Any] = {
            "game_id":      game_id,
            "name":         name,
            "alt_names":    alt_names,
            "engine":       f.get("engine") or "FM2K",
            "year":         f.get("year", ""),
            "developer":    f.get("developer", ""),
            "publisher":    f.get("publisher", ""),
            "exe_stems":    [game_id],
            "kgt_filename": "",
            "homepage":     f.get("homepage", ""),
            "download_url": "",
            "banner_url":   "",
            "thumb_url":    "",
            "sources":      ["manual"],
            "characters":   [],
            "stages":       [],
            "_raw":         {},
            "description":  f.get("description", ""),
            "purchase_url": f.get("purchase_url", ""),
            "resources":    [],
            "versions":     [],
            "created_by":   editor.get("nick", "?") if editor else "?",
            "imported_from": "manual",
        }
        # Optional atwiki link — only stored if the user provided a
        # numeric ID. We don't fetch the page; the merge tool can
        # backfill the rest later.
        aid = f.get("atwiki_id", "")
        if aid and aid.isdigit():
            new_row["atwiki_id"] = aid
            new_row["atwiki_url"] = (
                f"https://w.atwiki.jp/arunau32167/pages/{aid}.html")

        records.append(new_row)
        bak = _atomic_write_registry(records)
        _append_editlog({
            "ts":        datetime.now(timezone.utc).isoformat(),
            "editor":    editor.get("nick", "?") if editor else "?",
            "action":    "create",
            "game_id":   game_id,
            "row":       {"name": name, "engine": new_row["engine"],
                          "atwiki_id": new_row.get("atwiki_id")},
            "bak":       bak.name,
            "client_ip": request.client.host if request.client else "",
        })

    return RedirectResponse(url=f"/admin/g/{game_id}?saved=1&created=1",
                            status_code=303)


# ─── Per-game editor ─────────────────────────────────────────────────────

@router.get("/g/{game_id}", response_class=HTMLResponse)
def edit_form(request: Request, game_id: str,
              fm2k_admin: Optional[str] = Cookie(None),
              saved: int = 0):
    editor, redirect = _editor_or_redirect(request, fm2k_admin)
    if redirect:
        return redirect
    try:
        records = _read_registry()
    except (OSError, json.JSONDecodeError):
        records = []
    rec: Optional[dict[str, Any]] = None
    for r in records:
        if r.get("game_id") == game_id:
            rec = r
            break
    if rec is None:
        raise HTTPException(404, f"game_id {game_id!r} not in registry")
    return _templates.TemplateResponse(
        "admin_edit_game.html",
        {
            "request":        request,
            "rec":            rec,
            "editor":         editor,
            "saved":          bool(saved),
            "ALLOWED_KINDS":  ALLOWED_KINDS,
            # Pad resources with 3 empty rows so the form has slots
            # for new entries without needing JS to "add row".
            "resources_padded":
                (rec.get("resources") or []) + [{} for _ in range(3)],
        })


@router.post("/g/{game_id}/save")
async def edit_save(request: Request, game_id: str,
                    fm2k_admin: Optional[str] = Cookie(None)):
    editor, redirect = _editor_or_redirect(request, fm2k_admin)
    if redirect:
        return redirect
    form = await request.form()
    form_dict = {k: v for k, v in form.items()}

    with _registry_lock():
        records = _read_registry()
        idx = next((i for i, r in enumerate(records)
                    if r.get("game_id") == game_id), -1)
        if idx < 0:
            raise HTTPException(404, f"game_id {game_id!r} not in registry")

        before = records[idx]
        after  = dict(before)  # shallow copy; we replace specific keys

        # Scalar fields — empty form value means "clear it" (we store ""
        # rather than deleting the key, to keep schema stable).
        for field in ("name", "description", "year", "developer", "publisher",
                      "homepage", "wayback_homepage", "purchase_url"):
            v = form_dict.get(field, None)
            if v is not None:
                after[field] = v.strip() if isinstance(v, str) else v

        # alt_names: comma- or newline-separated single textarea.
        raw_alts = form_dict.get("alt_names", "")
        if isinstance(raw_alts, str):
            alts = [s.strip() for s in re.split(r"[,\n]", raw_alts) if s.strip()]
            after["alt_names"] = alts

        # resources[]: rebuild from form rows.
        after["resources"] = _normalize_resources(form_dict)

        # Compute diff before write so an empty-edit is a no-op.
        diff = _diff_record(before, after)
        if not diff:
            return RedirectResponse(
                url=f"/admin/g/{game_id}?saved=0&noop=1", status_code=303)

        records[idx] = after
        bak = _atomic_write_registry(records)

        _append_editlog({
            "ts":         datetime.now(timezone.utc).isoformat(),
            "editor":     editor.get("nick", "?") if editor else "?",
            "action":     "save",
            "game_id":    game_id,
            "diff":       diff,
            "bak":        bak.name,
            "client_ip":  request.client.host if request.client else "",
        })

    return RedirectResponse(
        url=f"/admin/g/{game_id}?saved=1", status_code=303)


# ─── Inline resource-add ─────────────────────────────────────────────────

@router.post("/g/{game_id}/add_resource")
async def add_resource(request: Request, game_id: str,
                       kind: str = Form("other"),
                       url: str = Form(...),
                       label: str = Form(""),
                       desc: str = Form(""),
                       fm2k_admin: Optional[str] = Cookie(None)):
    """Append a single resource row from the inline widget on the
    index page. Skips the full diff/edit machinery so the round-trip
    is fast — just lock, append, write, audit, redirect."""
    editor, redirect = _editor_or_redirect(request, fm2k_admin)
    if redirect:
        return redirect

    url = (url or "").strip()
    if not url or not _URL_RE.match(url):
        # Bounce back to the same filtered view; non-fatal — editors
        # see the row unchanged.
        return RedirectResponse(
            url=str(request.headers.get("referer") or "/admin/"),
            status_code=303)
    if kind not in ALLOWED_KINDS:
        kind = "other"

    new_row: dict[str, Any] = {
        "kind":   kind,
        "url":    url,
        "source": "manual",
    }
    if label.strip():
        new_row["label"] = label.strip()
    if desc.strip():
        new_row["desc"] = desc.strip()

    with _registry_lock():
        records = _read_registry()
        idx = next((i for i, r in enumerate(records)
                    if r.get("game_id") == game_id), -1)
        if idx < 0:
            raise HTTPException(404, f"game_id {game_id!r} not in registry")
        rec = records[idx]
        existing = rec.get("resources") or []
        # Idempotency: if a resource with the same (kind, url) already
        # exists, no-op.
        if any(r.get("kind") == kind and r.get("url") == url
               for r in existing):
            return RedirectResponse(
                url=str(request.headers.get("referer") or "/admin/"),
                status_code=303)
        existing.append(new_row)
        rec["resources"] = existing
        records[idx] = rec
        bak = _atomic_write_registry(records)

        _append_editlog({
            "ts":        datetime.now(timezone.utc).isoformat(),
            "editor":    editor.get("nick", "?") if editor else "?",
            "action":    "add_resource",
            "game_id":   game_id,
            "added":     {"kind": kind, "url": url, "label": label,
                          "desc": desc},
            "bak":       bak.name,
            "client_ip": request.client.host if request.client else "",
        })

    # Preserve filters/sort/search from referer when bouncing back.
    return RedirectResponse(
        url=str(request.headers.get("referer") or "/admin/"),
        status_code=303)


# ─── Delete ──────────────────────────────────────────────────────────────

@router.post("/g/{game_id}/delete")
async def delete_game(request: Request, game_id: str,
                      confirm: str = Form(""),
                      fm2k_admin: Optional[str] = Cookie(None)):
    """Hard-delete a registry row. The caller must POST `confirm` set
    to the target game_id — protects against accidental clicks and
    cross-game button presses.

    Match history (in matches.json) is NOT touched; orphaned matches
    will still aggregate under their game_id with no display name. If
    the goal is to merge two entries, use /merge instead so the kept
    entry inherits the source's match history via aliased_ids.
    """
    editor, redirect = _editor_or_redirect(request, fm2k_admin)
    if redirect:
        return redirect
    if confirm != game_id:
        raise HTTPException(400, "confirm must equal game_id")

    with _registry_lock():
        records = _read_registry()
        idx = next((i for i, r in enumerate(records)
                    if r.get("game_id") == game_id), -1)
        if idx < 0:
            raise HTTPException(404, f"game_id {game_id!r} not in registry")

        removed = records.pop(idx)
        bak = _atomic_write_registry(records)
        _append_editlog({
            "ts":        datetime.now(timezone.utc).isoformat(),
            "editor":    editor.get("nick", "?") if editor else "?",
            "action":    "delete",
            "game_id":   game_id,
            "removed":   {"name": removed.get("name"),
                          "atwiki_id": removed.get("atwiki_id"),
                          "n_resources": len(removed.get("resources") or []),
                          "n_versions": len(removed.get("versions") or [])},
            "bak":       bak.name,
            "client_ip": request.client.host if request.client else "",
        })

    return RedirectResponse(url="/admin/?deleted=" + game_id, status_code=303)


# ─── Merge ───────────────────────────────────────────────────────────────

def _merge_records(keep: dict[str, Any], drop: dict[str, Any]
                   ) -> dict[str, Any]:
    """Fold `drop` into `keep`. The kept row absorbs:
      - aliased_ids: kept.aliased_ids ∪ {drop.game_id} ∪ drop.aliased_ids
      - alt_names:   union (preserves keep order, adds drop's new ones)
      - resources[]: dedup by (kind, url)
      - versions[]:  dedup by xxh64
      - sources[]:   union
      - scalar fields: keep wins; drop fills only where keep is falsy

    Why aliased_ids: matches.json keys by game_id and we DON'T rewrite
    match history. Instead, aggregate_game() in the public app can
    resolve aliased_ids → primary game_id when computing /g/{id}, so
    `/g/<dropped_id>` redirects (or just renders) under the kept row's
    page. (Wiring that lookup into app.py is a follow-up — for now the
    field is recorded for downstream use.)
    """
    out = dict(keep)

    # aliased_ids
    aids = list(keep.get("aliased_ids") or [])
    drop_aids = list(drop.get("aliased_ids") or [])
    drop_gid = drop.get("game_id")
    for a in drop_aids + ([drop_gid] if drop_gid else []):
        if a and a not in aids and a != keep.get("game_id"):
            aids.append(a)
    out["aliased_ids"] = aids

    # alt_names + drop's name as alt if it differs from kept name
    alts = list(keep.get("alt_names") or [])
    extras = list(drop.get("alt_names") or [])
    if drop.get("name") and drop["name"] != keep.get("name"):
        extras.insert(0, drop["name"])
    for a in extras:
        if a and a not in alts:
            alts.append(a)
    out["alt_names"] = alts

    # resources — dedup by (kind, url)
    seen: set[tuple] = set()
    merged_res: list[dict[str, Any]] = []
    for r in (keep.get("resources") or []) + (drop.get("resources") or []):
        key = (r.get("kind"), r.get("url"))
        if key in seen:
            continue
        seen.add(key)
        merged_res.append(r)
    out["resources"] = merged_res

    # versions — dedup by xxh64
    seen_h: set[str] = set()
    merged_v: list[dict[str, Any]] = []
    for v in (keep.get("versions") or []) + (drop.get("versions") or []):
        h = v.get("xxh64")
        if h and h in seen_h:
            continue
        if h:
            seen_h.add(h)
        merged_v.append(v)
    out["versions"] = merged_v

    # sources — union
    src = list(keep.get("sources") or [])
    for s in drop.get("sources") or []:
        if s not in src:
            src.append(s)
    out["sources"] = src

    # Scalar backfill: if keep is empty/None, take from drop.
    for fld in ("year", "developer", "publisher", "homepage",
                "wayback_homepage", "purchase_url", "description",
                "engine", "atwiki_id", "atwiki_url", "archive_status"):
        if not out.get(fld) and drop.get(fld):
            out[fld] = drop[fld]
    return out


@router.get("/g/{game_id}/merge", response_class=HTMLResponse)
def merge_form(request: Request, game_id: str,
               fm2k_admin: Optional[str] = Cookie(None)):
    editor, redirect = _editor_or_redirect(request, fm2k_admin)
    if redirect:
        return redirect
    records = _read_registry()
    src = next((r for r in records if r.get("game_id") == game_id), None)
    if src is None:
        raise HTTPException(404, f"game_id {game_id!r} not in registry")
    # Suggest candidates with the same atwiki_id, then by normalized
    # name match.  Keeps the picker short instead of dumping all 443.
    candidates: list[dict[str, Any]] = []
    seen_gids: set[str] = {game_id}

    aid = src.get("atwiki_id")
    if aid:
        for r in records:
            if r.get("atwiki_id") == aid and r.get("game_id") not in seen_gids:
                candidates.append(r)
                seen_gids.add(r.get("game_id"))

    src_norm = re.sub(r"[^a-z0-9]+", "",
                      (src.get("name") or "").lower())
    if src_norm:
        for r in records:
            if r.get("game_id") in seen_gids:
                continue
            cand_norm = re.sub(r"[^a-z0-9]+", "",
                               (r.get("name") or "").lower())
            if cand_norm and (src_norm in cand_norm or cand_norm in src_norm):
                candidates.append(r)
                seen_gids.add(r.get("game_id"))

    return _templates.TemplateResponse(
        "admin_merge.html",
        {"request": request, "src": src, "candidates": candidates,
         "editor": editor})


@router.post("/g/{game_id}/merge")
async def merge_submit(request: Request, game_id: str,
                       target_id: str = Form(...),
                       direction: str = Form("into_target"),
                       fm2k_admin: Optional[str] = Cookie(None)):
    """Merge two registry rows into one.

    direction:
      - "into_target": keep target_id, drop {game_id}  (default)
      - "into_self":   keep {game_id},  drop target_id

    The dropped row's game_id lands in keep.aliased_ids[] so existing
    match-history references are recoverable downstream.
    """
    editor, redirect = _editor_or_redirect(request, fm2k_admin)
    if redirect:
        return redirect
    if game_id == target_id:
        raise HTTPException(400, "cannot merge a row with itself")
    if direction not in ("into_target", "into_self"):
        raise HTTPException(400, "bad direction")

    with _registry_lock():
        records = _read_registry()
        src_idx = next((i for i, r in enumerate(records)
                        if r.get("game_id") == game_id), -1)
        tgt_idx = next((i for i, r in enumerate(records)
                        if r.get("game_id") == target_id), -1)
        if src_idx < 0 or tgt_idx < 0:
            raise HTTPException(404, "one or both game_ids not in registry")

        if direction == "into_target":
            keep_idx, drop_idx = tgt_idx, src_idx
        else:
            keep_idx, drop_idx = src_idx, tgt_idx

        keep = records[keep_idx]
        drop = records[drop_idx]
        merged = _merge_records(keep, drop)
        records[keep_idx] = merged
        # drop_idx may shift after the keep replacement — recompute
        drop_gid_now = drop.get("game_id")
        drop_idx_now = next((i for i, r in enumerate(records)
                             if r.get("game_id") == drop_gid_now), -1)
        if drop_idx_now >= 0:
            records.pop(drop_idx_now)

        bak = _atomic_write_registry(records)
        _append_editlog({
            "ts":        datetime.now(timezone.utc).isoformat(),
            "editor":    editor.get("nick", "?") if editor else "?",
            "action":    "merge",
            "kept":      keep.get("game_id"),
            "dropped":   drop.get("game_id"),
            "n_resources_after": len(merged.get("resources") or []),
            "n_versions_after":  len(merged.get("versions") or []),
            "bak":       bak.name,
            "client_ip": request.client.host if request.client else "",
        })

    return RedirectResponse(
        url=f"/admin/g/{merged['game_id']}?saved=1&merged=1",
        status_code=303)
