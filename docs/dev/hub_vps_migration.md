# Hub VPS migration

Plan for moving the FM2K Rollback hub off your home box and onto a VPS.

The hub is currently a single Python process (`hub/hub.py`) that listens on
three UDP/TCP ports + serves the Discord OAuth callback over HTTP. DDNS
(`2dfm.sytes.net`) points at your home IP, your router forwards the four
ports to your desktop, the hub serves traffic from there. That's fine for
testing but couples the service's uptime to your residential connection's
uptime — unplugging the router takes the entire community offline. Move to
a VPS once the user count crosses the "I can't tolerate downtime when I
restart my PC" threshold.

This doc is written for that future-you. It assumes you've already verified
the hub works end-to-end on the home box.

## Prereqs / shopping

- **VPS provider.** Anything with a public IPv4, root SSH, and a real
  Linux distro works. Hetzner CX11 (€4/mo), DigitalOcean basic droplet
  (~$5/mo), Vultr cheapest, OVH VPS Starter, etc. Avoid AWS/GCP for this —
  you'll pay 5× more for the same capacity and the egress bandwidth pricing
  bites if a dozen people are relaying through you. 1 GB RAM and 1 vCPU is
  ample; the hub is a tiny asyncio process.
- **Region.** Pick a datacenter near the bulk of your players. North
  American players → US east. European players → Frankfurt or Helsinki.
  Latency to the hub mostly only matters for STUN probes (~one-shot per
  match) and for users behind symmetric NAT who fall through to relay (every
  packet adds the round-trip). For relay users it caps how good their
  netcode feels — a Frankfurt VPS adds ~80ms of relay-RTT to a US east-coast
  user's match. Local punch-through users don't care.
- **Domain.** You already have `2dfm.sytes.net` (No-IP). For VPS, point
  that domain at the VPS's static IP — either by switching the No-IP
  hostname's target IP (free), or by pointing your own domain via an `A`
  record. Avoid CNAME chains because some games' DNS resolvers cache poorly.
- **Required ports.** TCP 7700 (OAuth callback), TCP 7711 (WebSocket lobby),
  UDP 7711 (STUN responder, demuxed by first byte from the lobby), UDP 7712
  (relay). All must be open inbound on the VPS firewall. Outbound: TCP 443
  (Discord API), DNS 53.

## Pre-migration: stop the bleeding on home

Before you touch the VPS, do these on the home box so the migration doesn't
break in transit:

1. **Note current state.** Capture `hub/.env`, `hub/tokens.json`, and your
   current No-IP credentials somewhere offline. Tokens are cheap to lose
   (everyone re-OAuths) but `.env` has Discord secrets you don't want to
   leak — copy it via SCP to the VPS, never push to a repo.
2. **Bump the hub-token TTL** so the inevitable downtime during DNS
   propagation doesn't kick everyone offline. Edit `hub/.env`:
   ```
   HUB_TOKEN_TTL_SECONDS=604800  # 7 days, up from 1
   ```
   Restart the home hub, let everyone reconnect with the longer TTL. After
   migration is done you can drop it back to 86400.
3. **Verify the home hub is healthy** with the new advertise-host fix.
   Two unrelated peers should be able to challenge + match successfully
   right now. If it's broken locally, fixing it on the VPS is harder.

## VPS provisioning

1. Spin up the VPS. Pick a recent Debian or Ubuntu LTS — the hub only
   needs Python 3.10+ and a couple of pip deps; no exotic dependencies.
2. SSH in as root, create a non-root user with sudo (you do not want hub.py
   running as root):
   ```bash
   adduser fm2k
   usermod -aG sudo fm2k
   # add your SSH key to /home/fm2k/.ssh/authorized_keys
   ```
3. Lock down SSH — disable root login + password auth in `/etc/ssh/sshd_config`:
   ```
   PermitRootLogin no
   PasswordAuthentication no
   ```
   Reload sshd. Verify you can still get in as `fm2k` BEFORE you log out.
4. Open the firewall:
   ```bash
   sudo ufw default deny incoming
   sudo ufw default allow outgoing
   sudo ufw allow OpenSSH
   sudo ufw allow 7700/tcp
   sudo ufw allow 7711/tcp
   sudo ufw allow 7711/udp
   sudo ufw allow 7712/udp
   sudo ufw enable
   ```

## Hub install

1. Clone the hub repo (when `Armonte/fm2k-hub` exists; until then `scp` the
   `/mnt/c/dev/wanwan/hub/` directory):
   ```bash
   sudo -u fm2k -i
   git clone https://github.com/Armonte/fm2k-hub.git ~/fm2k-hub
   # or, until the private repo lands:
   # scp -r yourhomebox:/mnt/c/dev/wanwan/hub fm2k@vps:~/fm2k-hub
   cd ~/fm2k-hub
   python3 -m venv .venv
   .venv/bin/pip install -r requirements.txt
   ```
2. Copy `.env` from home box (NEVER commit this — has Discord secrets):
   ```bash
   # from home box
   scp /mnt/c/dev/wanwan/hub/.env fm2k@VPS_IP:~/fm2k-hub/.env
   ```
3. Update `.env` on the VPS:
   - `DISCORD_REDIRECT_URI` must match what's registered in the Discord
     developer portal. If you currently have `http://2dfm.sytes.net:7700/oauth/callback`,
     it stays the same — only the IP behind `2dfm.sytes.net` changes.
   - Keep `DISCORD_CLIENT_ID`, `DISCORD_CLIENT_SECRET`, `DISCORD_BOT_TOKEN`,
     `DISCORD_GUILD_ID`, `DISCORD_PATRON_ROLE_IDS`,
     `DISCORD_THANKS_ROLE_IDS`, `DISCORD_MONTE_ROLE_IDS` unchanged.
4. Test-run by hand to verify it boots:
   ```bash
   .venv/bin/python hub.py --advertise-host 2dfm.sytes.net
   ```
   You should see the four startup lines:
   ```
   FM2K Hub WS listening on tcp://0.0.0.0:7711
   FM2K Hub STUN listening on udp://0.0.0.0:7711
   FM2K Hub relay listening on udp://0.0.0.0:7712
     relay advertised to clients as udp://2dfm.sytes.net:7712
   ```
   Connect with a test launcher (set host=2dfm.sytes.net or the VPS IP
   directly, depending on whether DNS has been swapped yet) and verify
   sign-in + a single match works. Ctrl-C the hub when done.

## Run as a service

You want the hub to restart automatically on crash and to come up at boot.
systemd is the standard tool. Create `/etc/systemd/system/fm2k-hub.service`:

```ini
[Unit]
Description=FM2K Rollback hub
After=network.target

[Service]
Type=simple
User=fm2k
WorkingDirectory=/home/fm2k/fm2k-hub
ExecStart=/home/fm2k/fm2k-hub/.venv/bin/python hub.py --advertise-host 2dfm.sytes.net
Restart=on-failure
RestartSec=5
StandardOutput=append:/home/fm2k/fm2k-hub/hub.log
StandardError=append:/home/fm2k/fm2k-hub/hub.log

# Security hardening — non-essential but cheap.
NoNewPrivileges=yes
PrivateTmp=yes
ProtectSystem=strict
ReadWritePaths=/home/fm2k/fm2k-hub
ProtectHome=read-only

[Install]
WantedBy=multi-user.target
```

Enable + start:

```bash
sudo systemctl daemon-reload
sudo systemctl enable fm2k-hub
sudo systemctl start fm2k-hub
sudo systemctl status fm2k-hub   # should print "active (running)"
journalctl -u fm2k-hub -f        # tail logs in real time
```

If you ever need to stop it: `sudo systemctl stop fm2k-hub`.

## DNS swap

This is the single point of "everybody offline" risk. Plan a 5-min window.

1. **Lower DNS TTL** 24 hours BEFORE the swap so the swap propagates
   fast. Log into your No-IP dashboard, find `2dfm.sytes.net`, drop TTL to
   60 seconds. Wait at least an hour for old caches to expire.
2. **Day of:**
   - Stop the home hub: at home, `Ctrl-C` the running `python hub.py`.
   - Confirm VPS hub is up (`systemctl status fm2k-hub`).
   - Update `2dfm.sytes.net` in No-IP to point at the VPS IPv4. Save.
   - Watch propagation:
     ```bash
     # from any external machine
     for i in {1..30}; do nslookup 2dfm.sytes.net; sleep 5; done
     ```
     Within a minute or two, the answer should switch from your home IP to
     the VPS IP.
3. **Smoke test.**
   - From two different machines (NOT the home network, since residential
     ISPs sometimes hairpin DNS oddly), try `nslookup 2dfm.sytes.net` —
     both should return the VPS IP.
   - Launch the launcher on each, sign in, challenge each other. End-to-end
     match should work in under a minute.
4. **Bump TTL back up** to a sane 3600 (1 hour) once you're satisfied.

## Discord OAuth verification

After DNS swap, do one full re-OAuth from a real user account:

- Click "Sign in with Discord" in the launcher.
- Browser pops to Discord's authorize page, you click Authorize.
- Discord redirects to `http://2dfm.sytes.net:7700/oauth/callback?...`
- Browser shows the success HTML page.
- Launcher shows the green "Discord: Armonte" pill.

If anything in this flow 404s or hangs, the redirect URI is misconfigured.
Cross-check `.env`'s `DISCORD_REDIRECT_URI` matches Discord developer
portal's "Redirects" entry exactly (scheme, host, port, path).

## Auto-update for the hub

Hub doesn't auto-update — it's just Python source. The migration plan for
"deploy a hub change":

```bash
# on VPS
cd ~/fm2k-hub
git pull
sudo systemctl restart fm2k-hub
```

That's it. Config changes (`.env`) need a restart too. State (`tokens.json`)
persists across restarts.

For SSL on OAuth (long-term, optional): currently the OAuth callback runs
on plain HTTP at `http://2dfm.sytes.net:7700/`. Discord allows `http://`
redirects for non-public client IDs but they're flagged as insecure.
Promote to `https://` once you're on a VPS by:

1. Buying a domain + pointing it at VPS (No-IP free hostnames don't get
   Let's Encrypt certs reliably; a $10/year domain does).
2. `sudo apt install nginx certbot python3-certbot-nginx`
3. `sudo certbot --nginx -d hub.yourdomain.com`
4. Point nginx to proxy `:443/oauth/*` → `127.0.0.1:7700`. Hub binds
   localhost only at that point; nginx terminates TLS.
5. Update `DISCORD_REDIRECT_URI` to `https://hub.yourdomain.com/oauth/callback`
   in both `.env` and the Discord developer portal.

WebSocket and UDP relay can stay plain — they don't need TLS for this
threat model.

## Rollback plan

If the VPS migration breaks something nasty and you need to bail:

1. **Quick rollback (under 15 min):** point `2dfm.sytes.net` back at your
   home IP via No-IP. Restart the home hub with the same `--advertise-host`
   flag. DNS-cached clients pick it up within minutes.
2. **State recovery:** if the VPS hub modified `tokens.json` since you
   started migration, those issued tokens won't be valid against the home
   `tokens.json`. Worst case, users re-OAuth — minor friction, no data
   loss.
3. **Forensics:** `journalctl -u fm2k-hub --since '1 hour ago' > /tmp/hub.log`
   to grab the systemd-captured stdout/stderr from the failed run. Most
   issues will be visible there.

## Bot migration (later)

The Discord bot (`bot/bot.py`) currently runs on your desktop and welcomes
new patrons via DM. It can move to the same VPS:

1. `git clone Armonte/fm2k-bot ~/fm2k-bot` (or `scp` from home).
2. Copy `bot/.env` (just `DISCORD_BOT_TOKEN` — same value as the hub uses).
3. Add a second systemd service `fm2k-bot.service` that mirrors the hub
   one but runs `bot.py`.

Bot doesn't need any open ports; it's pure outbound to Discord's gateway.

## Operational checklist (post-migration)

- [ ] `journalctl -u fm2k-hub --since '24h ago'` shows no repeated errors.
- [ ] Active token count is reasonable (`wc -l ~/fm2k-hub/tokens.json` plus
      mental math from JSON structure).
- [ ] VPS firewall blocks everything except 22/tcp, 7700/tcp, 7711/tcp,
      7711/udp, 7712/udp.
- [ ] systemd `Restart=on-failure` is doing its job — kill the hub
      manually with `kill <pid>`, watch it come back via systemctl in <5s.
- [ ] CPU / RAM steady-state usage is well under capacity (`top -p $(pgrep
      -f hub.py)` — should be a few percent CPU, under 100MB RAM).
- [ ] Decommission the home box: stop port forwards, optionally stop the
      No-IP client (DDNS no longer needed since VPS has a static IP).
- [ ] Long-lived backup of `~/fm2k-hub/.env` and `~/fm2k-hub/tokens.json`
      stashed somewhere offline (encrypted password manager, paper wallet,
      etc).

## Cost-of-ownership notes

- ~$5–7/month VPS.
- ~$10/year domain (optional, for HTTPS).
- Bandwidth: relay-mode users push their entire match through the VPS.
  Worst case: 8 simultaneous symmetric-NAT relayed matches at 100 fps × ~80
  bytes/frame × 2 directions = ~125 KB/s aggregate, ~325 GB/month. Most
  cheap VPS plans cap at 500 GB to 1 TB/month — well within budget unless
  the player base grows 10×, in which case revisit.
- Disk: hub state is tiny (`tokens.json` grows linearly with active users
  but is ~50 bytes per token; even 10K users = 500KB).

## Future-proofing

When the hub starts to creak:

- **Multiple regional hubs**: spin up `hub-eu.yourdomain.com` and
  `hub-na.yourdomain.com`, let the launcher's hub-server combo pick. Each
  hub is independent — no shared state needed; users in EU get matched
  with users on the EU hub. Add hub-server entries to the Settings → Hub
  Server combo.
- **Move auth out of hub.py**: if Discord OAuth load becomes an issue
  (unlikely until thousands of users), split the OAuth callback into its
  own tiny FastAPI service behind nginx, leave hub.py focused on lobby +
  relay.
- **Metrics**: Prometheus textfile exporter is one Python file away. Once
  you have multiple hubs, you want graphs — active matches, relay packet
  rate, OAuth failures per minute, etc.

## TL;DR for migration day

```bash
# hours before:
#   - lower No-IP TTL to 60s
#   - scp .env from home box to VPS

# on VPS:
sudo systemctl enable --now fm2k-hub

# 5-minute window:
#   - stop home hub (Ctrl-C)
#   - point No-IP `2dfm.sytes.net` at VPS IP
#   - watch DNS propagation (`nslookup 2dfm.sytes.net` from external box)
#   - test-match from two real launchers
#   - bump TTL back to 3600

# if it breaks:
#   - point No-IP back to home IP
#   - restart home hub
#   - debug VPS at leisure
```

That's the playbook. Don't migrate during peak play hours.
