# branching

## Repo

`Armonte/wanwan` (private) — the dev tree. All code lives here:
launcher (`FM2K_*.cpp`), hook (`FM2KHook/`), updater helper
(`tools/FM2KUpdater.cpp`), build scripts.

## Branches

- **`main`** — what's been released. Tag every release commit
  (`v0.1.7`, `v0.1.8`, ...). Anyone reading the repo expects
  `main` to be runnable code that matches the latest published
  release.
- **`dev`** — your in-progress work. Where unfinished features
  land. Force-pushable while you're iterating; once a feature is
  stable it merges into `main` and goes out as a release.
- **Optional feature branches** (`golden-names`, `challenge-ux`,
  `render-fix`) — branch off `dev` if you want to keep work
  isolated. Merge back to `dev` when ready. Skip these for
  small one-day fixes; they're worth it for multi-day work
  you might want to abandon mid-flight.

## Setting up the dev branch (one-time)

```bash
cd /mnt/c/dev/wanwan
git checkout -b dev
git push -u origin dev   # only if/when you want it on GitHub
```

If you don't push `dev` upstream, it stays purely local. Local-only
is fine — only push when you want a friend to pull your in-progress
work.

## Day-to-day flow

```bash
# starting work
git switch dev
git pull --rebase    # if you push dev to GitHub

# coding...
./go.sh              # builds + deploys to /mnt/c/games/ locally
# test locally — see local_testing.md

# committing
git add <files>
git commit -m "wip: <what you're working on>"

# when a feature is done:
git switch main
git merge --no-ff dev          # or `git rebase main` from dev side
# now release-checklist.md
```

## What goes on `main` vs `dev`

| Change | Branch |
|---|---|
| Bump `FM2K_VERSION` in `scripts/make_version.sh` | `main` only, as part of the release commit |
| New feature work | `dev` |
| Bug fix that blocks all users | `main` (cherry-pick into `dev` if needed) |
| Hub server changes (`hub/`) | tracked separately in `Armonte/fm2k-hub` |
| Bot changes (`bot/`) | tracked separately in `Armonte/fm2k-bot` |
| `docs/dev/` notes | either branch — they're not shipped |

## When to NOT use a release flow

- Showing a friend a new feature: just zip up `dist/fm2k_v0.1.X.zip`
  and DM it. Don't bump LatestVersion. They run from a separate
  folder; auto-updater on `main` builds in their other folder still
  runs unchanged.
- Trying out hub server changes: run `hub/hub.py` on a different
  port and point your local launcher at it via Settings → Hub Server.
- Verifying the auto-updater itself: bump version locally only,
  don't push the LatestVersion bump until you've confirmed the
  update goes through cleanly.
