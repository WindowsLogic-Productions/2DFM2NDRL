# Public Stats Site (#43) — Design Proposal

Status: **proposal — needs sign-off before implementation**

## Goal

Public read-only website over the hub's match log. Mirrors what
[MeltyStats](https://meltystats.com) (built on the
[solenark/CCCasterStatistics](https://github.com/solenark/CCCasterStatistics)
fork) does for Melty Blood: per-player profiles with W/L/D, character
breakdowns, recent matches, leaderboards.

## Data we already have

Hub writes `matches.json` after every committed match (post-#41):

```jsonc
{
  "id":          "<32-char hex token>",
  "p1_id":       "<hub user id>",
  "p1_dc_id":    "<discord snowflake>",
  "p1_nick":     "Armonté",
  "p2_id":       "<hub user id>",
  "p2_dc_id":    "...",
  "p2_nick":     "...",
  "p1_char_id":  7,            // null if peers disagreed / older client
  "p2_char_id":  12,
  "game_id":     "WonderfulWorld",
  "winner_id":   "<p1_id|p2_id|"">",   // "" = draw
  "started_at":  1714900000.0,
  "finished_at": 1714900615.0,
  "report_p1":   "self_won",
  "report_p2":   "peer_won"
}
```

We do **not** yet have:
- `.player` filename → human name resolution (#51 covers this; site can render `id 7` as a placeholder until then)
- Per-round results (we only know the final winner, not 2-1 vs 2-0). Same as CCCaster's CSV.
- ELO / rating. CCCaster doesn't ship one either; MeltyStats inferred theirs server-side.

## CCCasterStatistics field comparison

| MeltyStats field         | Our equivalent             |
|--------------------------|----------------------------|
| SID (session)            | not tracked — we work per-match (good enough for v1) |
| IDX (match index)        | not tracked (reconstructable from started_at order)  |
| HCH / CCH (char ids)     | `p1_char_id` / `p2_char_id` |
| HMO / CMO (moon)         | n/a — FM2K games don't have a universal moon-equivalent |
| HPN / CPN (player number)| `p1_id` / `p2_id` (id is stable; "P1 vs P2" is just slot) |
| HVI / CVI (round wins)   | not captured — only final winner_id |

We're lighter on round-grain detail but otherwise structurally aligned.

## Pages to ship (v1)

- `GET /` — landing: total matches today/week/all-time, top-N most-active
  players, link to leaderboard.
- `GET /u/<user_id>` — player profile: total W/L/D, last 50 matches,
  char-breakdown table (rows=chars they played, columns=W/L/D).
- `GET /u/<user_id>/vs/<opp_id>` — head-to-head: matches between two
  users, char matchups within them.
- `GET /g/<game_id>` — per-game page: leaderboard for this game,
  most-played character.
- `GET /g/<game_id>/c/<char_id>` — per-character page: usage rate, top
  pilots, best matchups, worst matchups.
- `GET /m/<match_id>` — single match detail (mostly for hot-linking).
- `GET /recent` — last N matches across all users (pageable).
- `GET /api/v1/matches` — JSON feed (same shape as `matches.json`,
  paged + filterable). Lets people build their own dashboards on top.

## Architecture options

### A. FastAPI mount inside `hub.py` (same process)
- **Pros:** in-memory `MATCHES` already there; no extra process; instant
  freshness.
- **Cons:** hub crash takes site down; web-handler exceptions could
  affect realtime users; HTTP scaling competes with WS event loop on the
  same asyncio loop.

### B. Static-site generator on cron
- **Pros:** cheapest infra (nginx serves files); no live runtime; site
  works even if hub crashes.
- **Cons:** stale up to cron interval (5–15 min); per-user pages
  multiply file count; pagination harder.

### C. Separate FastAPI/Starlette service reading `matches.json` (recommended)
- **Pros:** decoupled deploy + scale; can crash without affecting hub;
  read-only mmap-style access keeps it simple; same Python tools as hub.
- **Cons:** small duplication of the JSON parser + aggregation helpers
  (acceptable for v1; refactor into a shared `fm2k_matchlog` package
  later if both grow).

**Recommendation:** **C** for v1. Lives in a sibling repo (`fm2k-stats`,
private during testing). Reads `matches.json` from the same VPS path
the hub writes; uses `watchdog` (or a 30-second poll) to invalidate an
in-process aggregation cache. Reverse-proxied behind nginx alongside
the hub on a sub-path (`stats.2dfm.sytes.net` or
`2dfm.sytes.net/stats`).

## Stack (recommendation)

- Python 3.11+ (matches hub.py)
- FastAPI + Uvicorn — async, type-checked, zero-config OpenAPI for the
  `/api/v1/...` JSON endpoints
- Jinja2 + minimal CSS for HTML templates (mirrors how the hub repo's
  `index.html` already renders) — keep it boring; this is read-only
  data, no JS framework needed
- No database. `matches.json` is the source of truth. Aggregations
  computed in-process with simple dict accumulators on file-change.
- If the match log grows past ~500k rows we revisit (probably switch to
  SQLite with a one-time importer; SQLite handles this easily but adds
  zero value at v1 scale).

## Auth model

Stats are **public read-only**. No auth on the site itself. Discord IDs
shown only as nicknames (already the case in `matches.json` —
`p1_dc_id` is recorded for ops correlation but never rendered).
A `/api/v1/...` rate-limit (per-IP) is the only access control needed.

## Out of scope (v1)

- ELO / TrueSkill rating
- Round-grain results (would need hook + match_result extension)
- Replay download (we have the replay files; site → replay download is
  a fast-follow once we figure out where to store the binaries)
- User-editable profile pages / avatars / linked Discord embeds
- Live "currently playing" view (would need WS subscription to the hub
  — separate work item)

## Decisions (2026-05-05)

1. **Hostname / TLS.** `2dfm.sytes.net/stats` sub-path for now, behind
   the existing nginx in front of the hub. Operator owns 2dfm.com /
   .org / .net / .games for a future production move; subdomain split
   is a deploy-time choice, not a code-time one. Today's public host
   is the no-ip dynamic DNS pointing at the operator's localhost.
2. **Character names block site launch.** #51 (`.player` filename
   resolution) is a prerequisite, not a fast-follow. Site will not
   ship with raw "id 7" anywhere user-facing — wait until the hook
   publishes resolved names + the hub stores them. Filenames are
   often Japanese (Shift-JIS / CP932 in FM2K's file I/O); resolution
   pipeline must be UTF-8 end-to-end so JP names render correctly on
   the site.
3. **Disputes default ops-only.** Public site never surfaces
   `match_disputes.json`. If we want a debug view later it lives
   behind a separate auth gate (basic auth / IP allowlist) or stays a
   `tail -f` workflow off-site.

## Security checklist (mandatory before public launch)

- [ ] Read-only file access. The site process opens `matches.json`
      with `O_RDONLY`; never writes to disk in the data path.
- [ ] No raw user-input rendering. All `nick` / `game_id` / char-name
      output goes through Jinja2's autoescape; no `safe` filter or
      `Markup()` calls anywhere.
- [ ] Discord IDs (`p1_dc_id` / `p2_dc_id`) never rendered in HTML.
      They exist on disk for ops correlation; the public site reads
      only `p1_id` / `p2_id` (the launcher-generated hub user id).
- [ ] Per-IP rate limit on `/api/v1/...` (e.g. 60 req/min). Use
      slowapi or nginx limit_req.
- [ ] No CORS for the JSON API in v1 (same-origin only). Open it up
      later if a third party wants to embed.
- [ ] Strict CSP header: `default-src 'self'; img-src 'self' data:`.
      No inline scripts, no third-party fonts.
- [ ] `matches.json` validation: any record missing `id`, `p1_id`,
      `p2_id`, `started_at`, `winner_id` (allowing `""`) is dropped
      from aggregations rather than crashing the renderer.
- [ ] Process runs as an unprivileged user with read access to the
      hub's data dir; cannot write to it, cannot exec, cannot bind
      privileged ports (nginx fronts it).
