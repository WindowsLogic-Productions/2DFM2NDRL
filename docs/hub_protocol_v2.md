# FM2K Hub Protocol v2 — additions for the AppShell

**Status:** Design proposal. Hub server side is unimplemented; this doc is the
contract the new launcher AppShell expects, so hub.2dfm.org can implement
incrementally.

**Complements:** `FM2K_Matchmaking_Design.md` (NAT + matchmaking infra).
This doc is about **client-visible state** that the new AppShell renders:
joined rooms, chat, game catalog. It does NOT touch netcode or NAT.

**Audience:** whoever owns hub.2dfm.org backend; secondarily, the launcher M2-M5
implementation work.

---

## 1. Why

The new AppShell (under `frontend/`) is a 640×448-min, IRC/Fightcade-style hub
client. The visual language assumes the user can:

- See the rooms they're **subscribed to** in the LeftRail (Fightcade
  IRC channel model — present + challenge-eligible in many rooms at
  once, see §4.1).
- **Chat** in any subscribed room — the lobby view shows chat from the
  *focused* room, with click-to-switch between subscribed rooms.
- **Browse a game catalog** with cover art / popularity / metadata for games
  they don't have installed.
- See **per-game leaderboards** in the RANK view.
- See **upcoming/active tournaments** in the EVENTS view.

None of these are exposed by the current `fm2k::HubClient` API. This doc
specs the additions needed.

The launcher will keep mocking these client-side until the corresponding hub
features ship — see §6 for the migration plan. Decoupled iteration: the
launcher visual fidelity work doesn't block on hub work and vice versa.

---

## 2. Current state — what hub.2dfm.org already supports

From `FM2K_HubClient.h:86` (HubEvent::Kind enum):

```
Connected, Disconnected, RoomList, RoomJoined, RoomLeft,
UserJoined, UserLeft, UserStatus, UserRtt,
ChallengeReceived, ChallengeFailed, ChallengeCancelled, ChallengeDeclined,
MatchStart, MatchRotated, PeerDisconnected,
SpectateGranted, SpectateDenied,
RecordReceived, RecentMatchesReceived, CurrentMatchesReceived,
MatchInProgressStarted, MatchInProgressUpdated, MatchInProgressEnded,
Error
```

Outbound API (`HubClient::*` member methods):

```
Connect / Disconnect / Poll
JoinRoom / LeaveRoom              ← single active room
ListRooms
Challenge / CancelChallenge / AcceptChallenge / DeclineChallenge
MatchEnded / MatchResult
QueryRecord / RequestRecentMatches / RequestCurrentMatches
RequestSpectate
```

**Mental model today:** one user = one active room at a time. Rooms are
discovered, but room-list never re-broadcasts (you call `ListRooms()`
on demand). No chat, no presence outside your active room, no catalog
metadata beyond `room_id == game_id`.

---

## 3. Gap analysis

| AppShell feature                   | Current hub support           | Gap                                                    |
|------------------------------------|-------------------------------|--------------------------------------------------------|
| LeftRail "joined rooms" (4 chips)  | Single active room            | Multi-room subscription — see §4.1                     |
| Lobby chat log                     | None                          | Chat send/receive events — see §4.2                    |
| Hub-event log on lobby             | Some via existing events      | Surface in launcher; no new hub work needed            |
| BrowseView game grid (with art)    | Only your local installs known | Catalog endpoint with display-name / banner — §4.3     |
| RankView ELO ladder                | `query_record` (per-user)     | Leaderboard endpoint, paginated — §4.4 (optional)      |
| EventsView tournaments             | None                          | Events feed — §4.5 (optional, can be Discord-bridged)  |
| Roster country flag                | Not in `HubUser`              | Optional `region` field on user — §4.6                 |
| Spectator presence in room         | Not separate from "in match"  | `is_spectator` flag on user — §4.6 (optional)          |

---

## 4. Proposed additions

Encoding follows the existing hub WS JSON conventions (each frame is a
`{type, ...}` object). Existing event names are camelCase / lowercase per
the current protocol; new events keep that style.

### 4.1 Multi-room subscription

**Concept:** a user has any number of *subscribed* rooms; all
subscriptions are equal-citizen — the user is present, visible, AND
challenge-eligible in every subscribed room simultaneously. There is
NO "active vs lurker" distinction on the hub side. The launcher tracks
a *focused* room locally (which room's chat fills the lobby view) but
that's pure UI state and not sent to the hub.

Subscriptions are persistent on hub server side across reconnects,
keyed by `discord_user_id`.

**Implications for `HubUser::status`:** stays global per-user (one of
`idle`/`challenging`/`in_match`), not per-room. When a user accepts a
challenge in room X, their status becomes `in_match` in *all* rooms
they're subscribed to — which is correct: the user is in one match,
that match is visible everywhere they're known.

**Cross-room challenge UX:** if user A (subscribed to KOF98+SF3) sees
user B (subscribed to KOF98 only) in the SF3 roster — wait, B isn't
in the SF3 roster, that's the point. Roster is per-room. A challenges
B in KOF98 (the room they share). If A doesn't have KOF98 installed
locally, the challenge button is hidden / shows an "INSTALL TO PLAY"
badge that links to BrowseView pre-filtered to KOF98.

**New outbound:**

```json
// Add a room to my subscription set. Idempotent.
{ "type": "subscribe_room", "room_id": "kof98" }

// Remove. Idempotent.
{ "type": "unsubscribe_room", "room_id": "kof98" }

// Reset to a specific list (used on connect to assert client-side state).
{ "type": "set_subscriptions", "room_ids": ["kof98", "3s", "kof02"] }
```

**New inbound:**

```json
// Server confirms subscription state after subscribe / set_subscriptions.
{ "type": "subscriptions", "room_ids": ["kof98", "3s", "kof02"] }

// User-X subscribed/unsubscribed from room-Y. Used to update each
// subscriber's presence count for that room. Optional (clients can poll
// `RoomList` if too chatty).
{ "type": "room_presence",
  "room_id": "kof98",
  "added": ["user_id_a"],
  "removed": ["user_id_b"] }
```

**Existing event tweaks:**

- `RoomJoined` / `RoomLeft` semantics change: they fire on *every*
  subscription add/remove, not just on the legacy single-active-room
  switch. Each carries a `room_id` so the launcher can update the right
  per-room roster. Legacy clients that expect "one room at a time" can
  still treat the most recent `RoomJoined` as their effective room.

- `UserJoined` / `UserLeft` events MUST carry a `room_id` field so the
  launcher can associate them with the right subscribed room. Today
  they're implicitly scoped to the user's active room; v2 expands that.

- `RoomActiveChanged` is **dropped** from the proposed events — the
  "active room" concept is gone server-side. Launcher's local "focused
  room" state never roundtrips to the hub.

**Persistence on hub:** subscription list keyed by Discord user id.
Survives WS disconnect. Hub re-subscribes user automatically on
reconnect (so client doesn't need to re-issue every connect).

**Client persistence:** as a back-stop, launcher writes the same list
to `%APPDATA%\FM2K_Rollback\settings.ini` under `subscribed_rooms=...`
(planned in M2). On reconnect: launcher issues `set_subscriptions` with
its locally-cached list as authoritative source-of-truth, hub overrides
its server-side state to match. This makes "edit subscription list"
purely a client action.

**HubEvent::Kind additions:**
- `SubscriptionsChanged`  (carries `room_ids` vector)
- `RoomPresenceUpdated`   (room_id, added, removed)

### 4.2 Chat per room

**Concept:** ephemeral chat broadcasts within a room. Hub does NOT
persist chat history (out-of-scope for first ship — IRC also doesn't).
Client renders an in-memory ring buffer of last N messages per room.

**New outbound:**

```json
{ "type": "chat_send", "room_id": "kof98", "text": "ggs" }
```

Hub validates `text` (length, control chars, etc.) and broadcasts to
every user subscribed to that room.

**New inbound:**

```json
{ "type": "chat",
  "room_id": "kof98",
  "user_id": "abc123",
  "nick": "elbebe",
  "text": "ggs",
  "ts": 1715260800,        // unix seconds, hub-authoritative
  "system": false           // true for hub-generated lines (joins/leaves/etc.)
}
```

**System chat** (hub-generated, `system: true`):

- New user joined an active room → broadcast to that room's subscribers
- User started a match in the room (mirrors existing `MatchInProgressStarted`
  but as a chat line for visibility)
- User left
- Hub announcements (event reminders, etc.)

Could be sent as `chat` events with `system: true`, or as separate
`system_chat` events. Recommend the `system: true` flag — clients render
both into the same chat log with different styling.

**Rate-limiting:** hub-side, ~1 msg/sec/user, 4 burst. Drop excess
silently, send the user a `chat_rate_limited` event.

**Filtering / mute / ignore:** out-of-scope server-side. Client maintains
local ignore list keyed by `user_id`.

**HubEvent::Kind additions:**
- `ChatReceived` (room_id, user_id, nick, text, ts, system)
- `ChatRateLimited` (last_send_ts, retry_after_ms)

### 4.3 Game catalog

**Concept:** hub publishes a canonical list of every game it knows about,
not just rooms with active users. Powers the BrowseView grid + LeftRail
chip metadata for games-not-installed-locally. Read-only, low-update-
frequency — fits a REST endpoint better than a WS event.

**Source data — current state is WIP:** a curated collection lives at
`/mnt/d/games/`, organized as `<engine>/<developer>/<game>/`:

```
/mnt/d/games/fm2k/<developer>/<game>/         # FM2K-engine titles
/mnt/d/games/fm95/<developer>/<game>/         # FM95-engine titles
/mnt/d/games/fm2k_sdk/                        # SDKs, installers, refs
```

A small number of entries already carry a per-game `metadata.json`
(example: `/mnt/d/games/fm2k_sdk/fm-95-install/metadata.json`) following
this schema:

```json
{
  "name":              "...",
  "alternative names": [],
  "year":              "1997",
  "publisher":         "",
  "developer":         "ASCII",
  "url":               "https://archive.org/details/<id>",
  "download":          "https://archive.org/download/<id>",
  "source":            "archive.org",
  "ia_identifier":     "fm-95",
  "ia_kgt":            "<original kgt filename>",
  "ia_exe_stem":       "<original exe stem>"
}
```

**This schema is the *target* — most game folders don't have it yet.**
The base set is mid-cleanup: many original archives are self-extractors
or ship broken executables, so the metadata + a clean re-upload to IA
won't be done until those are cleaned up. Until then, the hub's
`/api/games` will be a partial catalog that grows as entries are
metadata'd. Launcher-side: treat catalog as a hint not a source of
truth — local installs (discovered from disk) remain the canonical
"what can I actually play" list.

The IA fields (`ia_identifier`, `ia_kgt`, `ia_exe_stem`, `download`)
will eventually bind each entry to its archive.org artifact for
launcher-side download. Spec for that flow is deferred until the
base set + metadata is ready.

**New REST endpoint:**

Schema is a superset of the per-game `metadata.json` curated under
`/mnt/d/games/<engine>/<dev>/<game>/`, plus a few hub-side fields
(`id`, `engine`, `popular_rank`, `banner`/`icon`, `exe_aliases`).

```
GET https://hub.2dfm.org/api/games

Response (200):
{
  "version":    1,
  "updated_at": "2026-05-09T12:00:00Z",
  "games": [
    {
      // hub-canonical
      "id":             "kof98",
      "engine":         "fm2k",                     // or "fm95"
      "popular_rank":   1,                          // 1-based, lower = more popular
      "tags":           ["snk", "team-3v3"],
      "exe_aliases":    ["KOF98.EXE", "KOF98_REBORN.EXE"],
      // banner/icon nullable until art ships — see §7 Q3
      "banner":         null,
      "icon":           null,

      // mirrored from metadata.json
      "name":               "King of Fighters '98",
      "alternative_names":  [],
      "year":               "2026",
      "publisher":          "",
      "developer":          "Border Violation Taisei",
      "url":                "https://archive.org/details/...",
      "download":           "https://archive.org/download/...",
      "source":             "archive.org",
      "ia_identifier":      "...",
      "ia_kgt":             "...",
      "ia_exe_stem":        "..."
    },
    ...
  ]
}
```

**Client behavior:**
- Fetch on launcher start, cache for 24h (or until `updated_at` differs)
- LeftRail / BrowseView lookup `id → entry`; fallback to local exe stem
  when entry is missing (private game, not in catalog)
- `exe_aliases` lets the launcher map a discovered EXE on disk to its
  hub `game_id` even when the filename is non-canonical

**No new HubEvent kinds** — REST endpoint, polled.

### 4.4 Per-game leaderboards (optional / later)

Powers RankView. Could be done as either:

**Option A — REST poll (simpler, recommended for first):**

```
GET /api/leaderboards/{game_id}?limit=100

{
  "game_id": "kof98",
  "updated_at": "...",
  "entries": [
    { "rank": 1, "user_id": "...", "nick": "...", "rating": 2153,
      "wins": 412, "losses": 240, "main_char": "TERRY" },
    ...
  ]
}
```

Refresh on tab focus, manual refresh button, or every 5 min while RankView
is open.

**Option B — WS event:** `RequestLeaderboard {game_id, limit}` →
`LeaderboardReceived {game_id, entries}`. Saves a TCP setup but adds a
HubEvent::Kind. Skip for v2.

### 4.5 Events / tournaments (optional / later)

EventsView shows things like "BRASILEIRO 30 starts in 13d". Two reasonable
sources:

1. **Hub-managed feed:** `GET /api/events?after=<ts>&limit=20`
2. **Discord webhook bridge:** hub watches a Discord events channel, mirrors
   posts into the API. Cheaper to operate.

Either way, response shape:

```json
{
  "events": [
    {
      "id":         "brasileiro-30",
      "title":      "BRASILEIRO 30",
      "game_id":    "kof98",
      "starts_at":  "2026-06-01T20:00:00Z",
      "url":        "https://challonge.com/brasileiro30",
      "blurb":      "Single-elim, FT3 → FT5 finals",
      "entry_open": true,
      "entrants":   62,
      "entry_cap":  100
    },
    ...
  ]
}
```

Skip for v2; render placeholder with "BACKEND PENDING" tag in the AppShell
until shipped.

### 4.6 User profile fields (small)

Existing `HubUser` (`FM2K_HubClient.h:32`):

```
{ id, nick, room_id, status, opponent_id, rtt_ms, tier }
```

Suggested additions:

- `region`: 2-letter ISO ("MX", "BR", "JP", ...) — derived from IP geo at
  hub session start. Drives the FlagDot in roster rows.
- `is_spectator`: bool — distinguishes lobby roster from "users actually
  playing FM2K." Useful for IN-MATCH list rendering.

These are presence info, not stats. No server work beyond enriching
`UserJoined` / `UserStatus` payloads.

---

## 5. Versioning + capability negotiation

The launcher needs to know which features the hub it's connecting to
supports. Existing `Connected` event payload could grow:

```json
{ "type": "hello_ack",
  "user_id": "...",
  "rooms": [...],
  "capabilities": [
    "subscribe_v1",   // §4.1
    "chat_v1",        // §4.2
    "leaderboard_v1", // §4.4
    "events_v1"       // §4.5
  ],
  "protocol_version": 2
}
```

Launcher branches on capability presence:
- `subscribe_v1` absent → fall back to single-room model, ignore
  `subscribed_rooms` setting beyond using its first entry as the active
  room.
- `chat_v1` absent → suppress chat send button, render "// chat unavailable
  on this hub" placeholder.
- Catalog endpoint absent (404 on `/api/games`) → use local install names
  only.

---

## 6. Migration plan — launcher side

The M-plan in `~/.claude/plans/what-i-want-us-parsed-rabin.md` already gates
shell features on M-milestones. Updated to align with this protocol doc:

| Milestone   | Hub work needed                      | Launcher fallback when absent              |
|-------------|--------------------------------------|---------------------------------------------|
| M1 (done)   | None                                 | Render real rooms + roster + matches        |
| M1.5 (done) | None                                 | Mock chat; one-room-at-a-time presence      |
| M2          | Optional: §4.3 catalog (BrowseView)  | Local installs only                         |
| M3          | None for challenge/match flow        | Existing events sufficient                  |
| M4          | None for motion                      | —                                           |
| post-M5     | §4.1 multi-subscribe                 | Single subscribed room (legacy `JoinRoom`)  |
| post-M5     | §4.2 chat                            | Mock chat (in-memory, scrollback-free)      |
| post-M5     | §4.4 leaderboards, §4.5 events       | "BACKEND PENDING" stubs                     |

**Implication:** the launcher is shippable through M5 without any of the
v2 protocol work. The protocol additions are unlocks for richer chat /
discovery / community features.

---

## 7. Open questions + decisions

### Decided

1. **Chat scrollback on hub-side: NO, intentionally ephemeral.**
   Won't be added even later. IRC-style "you had to be there" semantics.
   The launcher keeps an in-memory ring buffer (~200 lines per
   subscribed room) for the duration of the session; on disconnect /
   restart, that's gone. No `RoomJoined` history payload, no `/api/chat`
   endpoint.

2. **Catalog mutability: GitHub repo + admin endpoint (later).** v2
   ships with a flat `games.json` in a public hub repo, mirrored from
   the curated `/mnt/d/games/` tree (see §4.3). PRs for additions /
   corrections. **Phase 2:** an admin endpoint (`POST /api/games/{id}`
   with hub-admin auth) lets us correct typos / add new releases
   without a redeploy. Out of scope for v2 first ship but plan the
   schema with the admin path in mind.

3. **Banner art hosting: temp / placeholder for v2.** Real banner art
   is a separate art-pipeline problem. v2 ships with `banner` /
   `icon` URLs nullable; the launcher renders gradient-tint chips
   when art is missing (already implemented). When art exists, point
   `banner` at GitHub raw URLs against the hub repo's `assets/games/`
   subdirectory — zero infrastructure cost, CDN-cached by GitHub.
   `webp` preferred for size; `png` accepted as fallback.

4. **Subscription cap: 8 rooms.** Cap subscribed-rooms at 8 per user
   to start. Each subscribe means hub broadcasts presence + chat to
   that user — 8 is enough headroom for the active fighting-game
   audience without exploding fanout. Revisit if users hit it.

5. **Anti-spam beyond rate-limiting: deferred.** v2 ships with the
   per-user 1-msg/sec / 4-burst rate limit only. Shadow-mute / slow-
   mode / room-mod tools land later if abuse becomes a problem.
   Client-side ignore list (mute by `user_id`) is the v2 fallback.

6. **Identity model: Discord OAuth + user-set nick.** Current launcher
   model is the v2 model. Hub stores `discord_user_id` as canonical
   identity (immutable Discord ID); `nick` is an ephemeral display
   label the user can override per-session. The launcher's existing
   `use_discord_name` toggle (`FM2K_DiscordAuth.h:28`) decides whether
   the outgoing nick on connect is the Discord `global_name` or the
   user's custom string. No new identity work for v2.

### Still open

7. **Cross-room challenge UX when challenger lacks the game.** Long-
   term plan: integrate IA-archived game downloads (we re-upload our
   curated set; IA hosts free, we don't pay storage). User clicks
   challenge → toast/notification offers install → user confirms →
   download starts. Explicitly **NOT auto-install**.

   For v2 first ship, the IA pipeline isn't ready (see §4.3 — base set
   is mid-cleanup), so cross-room challenge from "subscribed but not
   installed" rooms hides the challenge button and shows a soft
   "install required" tooltip. Once IA pipeline lands, the same button
   becomes the install-prompt entry point.

8. **Notification / toast system shape.** Related to Q7 — the launcher
   needs an in-shell toast surface (matchmaking events, install
   prompts, hub disconnects, peer-disconnect-during-match) that's
   distinct from modal popups. Design TBD; leans toward a stack of
   transient cards anchored bottom-right with auto-dismiss + click-
   through. Out of scope for hub protocol; flagging it here so we
   don't bolt it on as an afterthought when wiring §7 Q7 later.

---

## Appendix A — HubEvent::Kind enum, post-v2

```cpp
enum class Kind {
    // === existing ===
    Connected, Disconnected,
    RoomList, RoomJoined, RoomLeft,
    UserJoined, UserLeft, UserStatus, UserRtt,
    ChallengeReceived, ChallengeFailed, ChallengeCancelled, ChallengeDeclined,
    MatchStart, MatchRotated, PeerDisconnected,
    SpectateGranted, SpectateDenied,
    RecordReceived, RecentMatchesReceived, CurrentMatchesReceived,
    MatchInProgressStarted, MatchInProgressUpdated, MatchInProgressEnded,
    Error,

    // === v2 additions ===
    SubscriptionsChanged,    // §4.1
    RoomPresenceUpdated,     // §4.1 (optional, polling fallback OK)
    ChatReceived,            // §4.2
    ChatRateLimited,         // §4.2
};
```

## Appendix B — REST endpoints, post-v2

```
GET  /api/games                            # §4.3 catalog
GET  /api/leaderboards/{game_id}           # §4.4 (optional)
GET  /api/events                           # §4.5 (optional)
```

## Appendix C — Reference: Fightcade's model (informally observed)

Fightcade exposes (from public client behavior, not source — their backend
is closed):

- Multi-channel IRC-style chat (each game = a channel; users idle in
  multiple channels simultaneously, all challenge-eligible — matches
  our §4.1)
- Persistent identity (FC username, no OAuth)
- Per-game leaderboards refreshed periodically
- ELO + W/L records stored hub-side
- Replay upload + share (out of scope for our v2)
- In-channel match challenges from any subscribed channel

Our v2 reaches feature parity on multi-room + chat + leaderboards. We
intentionally diverge on identity (Discord OAuth instead of FC accounts)
and replays (local-only for now).
