# infra map

Where each piece of the FM2K Rollback stack lives, what it owns, what
secrets are involved.

## Repos

| Repo | Visibility | Purpose | What's in it |
|---|---|---|---|
| `Armonte/wanwan` | private | Source of truth for launcher / hook / updater | C++ source, build scripts, this doc |
| `Armonte/fm2ktest` | **public** | Binary release distribution | `LatestVersion` (text), GitHub Releases with `fm2k_v<ver>.zip` per version |
| `Armonte/fm2k-hub` | private | Matchmaking server source | `hub/hub.py`, `hub/auth.py`, `hub/.env` (gitignored), `hub/tokens.json` (gitignored) |
| `Armonte/fm2k-bot` | private | Discord welcome bot | `bot/bot.py`, `bot/.env` (gitignored) |

## Live services

| Service | Address | Process | Notes |
|---|---|---|---|
| Matchmaking hub (WebSocket) | `2dfm.sytes.net:7711` | `python hub/hub.py` | DNS points at your home box; requires port-forwarding 7711 + 7712 (relay) |
| OAuth callback (HTTP) | `2dfm.sytes.net:7700` | same process as above | Discord redirects browser here after Authorize |
| STUN responder (UDP) | `2dfm.sytes.net:7711` | same process | Same port as WS — different protocol, demuxed by first byte |
| Hub relay (UDP) | `2dfm.sytes.net:7712` | same process | Symmetric NAT fallback path for game traffic |
| Discord bot | runs on your desktop | `python bot/bot.py` | Welcomes new patrons via DM, `/check` slash command |

## Secrets

All secrets live in `.env` files that are gitignored. Don't commit them.

`hub/.env`:
- `DISCORD_CLIENT_SECRET` — OAuth secret from Discord developer portal
- `DISCORD_BOT_TOKEN` — same value as bot uses
- `DISCORD_GUILD_ID`, `DISCORD_PATRON_ROLE_IDS` — IDs (not secrets, but live here for convenience)

`bot/.env`:
- `DISCORD_BOT_TOKEN` — copy of the same token

## State files

- `hub/tokens.json` — persisted hub tokens. Lets a hub restart not
  kick everyone offline. Gitignored — leaking it = bypass auth for
  anyone listed. Stays under TTL (`HUB_TOKEN_TTL_SECONDS`) anyway.
- `%APPDATA%\FM2K_Rollback\` — per-user launcher state on each
  client machine:
  - `discord_auth.json` — cached hub token from OAuth pairing
  - `audio.ini` — BGM/SFX mute toggles
  - `fm2k_inputs.ini` — default input bindings
  - `fm2k_inputs_<game_basename>.ini` — per-game override profiles

## Discord-side

- Discord application: `Armonte/FM2K Rollback Launcher` (App ID `232764824210243584`)
- Discord server: `1500596702799728743`
- Patron roles:
  - Tester ($5+): `1500624782188613763` — grants hub access
  - Special Thanks ($10): `1500624820012843218` — grants hub access + golden name (TODO: not implemented yet, see todo.md)
  - Supporter ($3): not granted hub access during testing
- Patreon → Discord integration auto-assigns roles based on tier

## Build artifacts

- `build/` (local) — ninja build output, full debug info DLLs (~33 MB)
- `dist/` (local, gitignored) — release zips packaged by `package_release.sh`
- `/mnt/c/games/` (local) — stripped deploy target for ./go.sh
- GitHub Releases on `fm2ktest` — public binary archive

## Auto-updater contract

Launcher does these HTTP calls on startup and on user click:

```
GET https://raw.githubusercontent.com/Armonte/fm2ktest/main/LatestVersion
    → plain text, current released version code (e.g. "0.1.8")
GET https://github.com/Armonte/fm2ktest/releases/download/v<X>/fm2k_v<X>.zip
    → release artifact bytes
```

Both are CDN-cached. `raw.githubusercontent.com` lag after a push is
1-5 minutes; that's fine for users but factor it into release checks.

## Off-list things to remember

- DDNS (`sytes.net`) — `2dfm.sytes.net` is your No-IP/dynu host
  pointing at your home IP. If your home IP changes the hub is
  unreachable. Keep the No-IP client running on the desktop.
- Port forwards — router has TCP 7711 + UDP 7711 + UDP 7712 + TCP 7700
  forwarded to your desktop. If the desktop's local IP changes (DHCP
  lease shuffle) the forwards break.
- Patreon billing day — patron tier expirations roll Discord roles
  monthly. The bot doesn't currently DM warnings; hub auth just stops
  accepting their token next reconnect.
