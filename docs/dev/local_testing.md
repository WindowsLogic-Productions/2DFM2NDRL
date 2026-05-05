# local testing

How to run development builds without disturbing the live hub or
the public launcher channel.

## Local hub against your own dev launcher

The hub is a Python server in `hub/hub.py`. By default it requires
Discord OAuth (validates patron tokens). For local dev, bypass that:

```bash
cd hub
FM2K_HUB_AUTH_DISABLE=1 python hub.py --port 7799
```

`--port 7799` so it doesn't collide with the production hub on 7711
(in case you have a port-forward or local DNS pointing 2dfm.sytes.net
to your box).

In the launcher: Settings → Hub Server → set host to `127.0.0.1:7799`
(or whatever port you picked). Hub accepts unauthenticated `hello`
when `FM2K_HUB_AUTH_DISABLE=1` is on, so you skip the Discord pill.

## Two local game instances against your own hub

This is the existing local-test path the launcher already supports.

1. Launcher → Developer Mode → enables the dev-only "Launch Local
   Client 1 / 2" buttons.
2. Pick a game → Launch Client 1 → Launch Client 2.
3. Both spawn pointing at `127.0.0.1` for UDP. Hub bypassed, GekkoNet
   talks directly between the two local processes.

For verifying rollback fixes without involving anyone else.

## Running an alternate launcher build alongside the released one

When you want to test a build but ALSO have the released launcher
running on the same box (e.g. one machine for "real" play and dev):

```bash
mkdir -p /mnt/c/games/dev
# put the dev exe + dll in that folder instead of /mnt/c/games/
cp build/FM2K_RollbackLauncher.exe build/FM2KHook.dll \
   build/FM2KUpdater.exe /mnt/c/games/dev/
# launch from /mnt/c/games/dev/ instead.
```

The auto-updater picks the EXE's directory via `GetModuleFileNameA`,
so the dev build self-updates within `/mnt/c/games/dev/` without
touching `/mnt/c/games/` (the released install). Two sandboxes,
zero overlap.

## Skipping the auto-update prompt during dev

Two options:

**A. Hardcode a high version locally** so the launcher thinks it's
already ahead of LatestVersion:

```bash
# Edit scripts/make_version.sh, set FM2K_VERSION="9.9.9"
./go.sh
```

The blue pill won't appear because `9.9.9 > 0.1.Y`.

**B. Point the auto-updater at a private fork** of `fm2ktest`. Edit
`scripts/make_version.sh`'s `kUpdateRepoOwner / kUpdateRepoName`.
Useful when you want to TEST the auto-update flow against staged
zips without polluting the production repo.

## RNG-trace tooling for desync investigations

When a user reports a desync:

1. Have them set `FM2K_RNG_TRACE=1` (and ideally
   `FM2K_PARITY_RECORD_PATH=parity.pty`) before launching.
2. They run the same match that desyncs.
3. They send you both peers' `FM2K_rng_trace_pid<PID>.bin` files.
4. `python3 tools/rng_trace_diff.py peer_a.bin peer_b.bin` —
   shows the first divergent rand call with caller PC. Look up
   PC in IDA → that's the FM2K function whose RNG-call count
   differs between peers. That's the bug.

## Testing the auto-updater path without disturbing public users

1. Make a private repo `Armonte/fm2ktest-staging`.
2. Edit `scripts/make_version.sh` locally — change
   `kUpdateRepoOwner / kUpdateRepoName` to point at staging.
3. Build, push to staging, bump staging's LatestVersion.
4. Run a separate launcher build (also pointed at staging) and
   verify the update flow.
5. When confident, revert `scripts/make_version.sh` to point at
   `fm2ktest` and follow the real release checklist.

## Wine / Linux native testing

The launcher is mingw32 Win32 only. There's no Linux native target.
For testing on Linux: `wine /mnt/c/games/FM2K_RollbackLauncher.exe`.
Most things work; gamepad detection is limited (Wine's xinput is
hit-or-miss, HIDAPI sticks usually do not enumerate).
