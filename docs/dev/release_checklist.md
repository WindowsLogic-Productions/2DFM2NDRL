# release checklist

The exact sequence to push a new launcher version to all users.
Skip ANY of these and the release is broken in subtle ways.

## Pre-release checks

- [ ] `dev` is merged to `main`.
- [ ] You're on `main`: `git switch main && git status` is clean.
- [ ] Build is clean locally: `./go.sh` succeeds, no warnings about
      new code.
- [ ] You've tested the new build on your own machine. Not just
      "it compiles" — actually launched a game, played a match,
      confirmed nothing visibly broke.
- [ ] If the change touches the hub: hub side is also up to date
      (`hub/hub.py` deployed on the box that runs `2dfm.sytes.net`).

## Cut the release

```bash
# 1. Bump version
sed -i 's/FM2K_VERSION="0.1.X"/FM2K_VERSION="0.1.Y"/' scripts/make_version.sh
# (or edit by hand)

# 2. Build with the new version stamp
./go.sh

# 3. Package
./scripts/package_release.sh
# → produces dist/fm2k_v0.1.Y.zip

# 4. Cut the GitHub release. notes is the user-facing changelog —
#    write what changed and why somebody would want it.
gh release create v0.1.Y dist/fm2k_v0.1.Y.zip \
    --repo Armonte/fm2ktest \
    --title v0.1.Y \
    --notes "<what changed>"

# 5. Bump LatestVersion in the public release repo. THIS IS WHAT
#    TRIGGERS THE AUTO-UPDATE on every running launcher.
cd /tmp/fm2ktest_init   # or wherever your fm2ktest checkout is
git pull --quiet
echo "0.1.Y" > LatestVersion
git commit -am "release v0.1.Y"
git push

# 6. (Optional) Tag and push the source-tree main commit.
cd -
git tag v0.1.Y
git push origin v0.1.Y
```

## Or, the one-shot script

`scripts/cut_release.sh` does all of the above in one command —
preconditioned on `FM2KTEST_REPO_DIR` env var being set:

```bash
export FM2KTEST_REPO_DIR=~/projects/fm2ktest    # add to .bashrc
./scripts/cut_release.sh "Release notes here"
```

## Post-release sanity

- [ ] `curl https://raw.githubusercontent.com/Armonte/fm2ktest/main/LatestVersion`
      returns the new version.
- [ ] Run a launcher you have NOT updated locally (different folder,
      older copy). Within ~5s of opening, the blue update pill
      should appear. Click → download → restart → confirm new
      version in `Settings → menu` shows `vN.N.N (rev <sha>)`.
- [ ] Watch Discord for ~10 min after release for "doesn't work"
      reports.

## If a release breaks for users

```bash
# fast rollback: bump LatestVersion BACK to the previous good version.
cd /tmp/fm2ktest_init
echo "0.1.X" > LatestVersion
git commit -am "rollback to v0.1.X"
git push
# raw.githubusercontent.com cache takes 1-5 min to update.
```

Note that already-updated users stay on the broken version until
you cut a fix. The rollback only stops new updates from spreading.
