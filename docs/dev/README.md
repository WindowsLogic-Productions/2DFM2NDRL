# dev workflow

Notes for working on the launcher / hook / hub without disturbing the
public `fm2ktest` release channel. Companion files in this folder:

- `branching.md` — git branch layout + when to merge.
- `release_checklist.md` — exactly what runs to cut a public update.
- `local_testing.md` — running locally against your own hub, with auth
  off, against test games.
- `todo.md` — outstanding feature work (golden names, challenge UX,
  render-script fixes, input-buffer investigation).
- `infra_map.md` — which repo holds what, which secrets live where.

**The one rule:** `LatestVersion` in `Armonte/fm2ktest` is what every
launcher in the wild reads. Don't bump it until the release is genuinely
ready. Everything below is structured around making it easy to develop
and test without touching that file.
