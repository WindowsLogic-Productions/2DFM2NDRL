#!/usr/bin/env python3
"""Mint or list bearer tokens for the stats admin editor.

Usage:
  tools/admin_mint_token.py mint  <nick>          # creates a new token
  tools/admin_mint_token.py list                  # shows existing nicks (no tokens)
  tools/admin_mint_token.py revoke <token-prefix> # removes any token whose
                                                  # value starts with prefix

Tokens are written to stats/admin_tokens.json, which is gitignored.
The minted token is printed to stdout once — there is no recovery path
if you lose it (just mint a new one).
"""

import argparse
import json
import secrets
import sys
import time
from pathlib import Path


REPO = Path(__file__).resolve().parents[1]
STORE = REPO / "stats" / "admin_tokens.json"


def load() -> dict:
    if not STORE.exists():
        return {"version": 1, "tokens": []}
    try:
        return json.loads(STORE.read_text(encoding="utf-8"))
    except json.JSONDecodeError:
        print(f"WARN: {STORE} is malformed; starting fresh", file=sys.stderr)
        return {"version": 1, "tokens": []}


def save(data: dict) -> None:
    STORE.parent.mkdir(parents=True, exist_ok=True)
    STORE.write_text(json.dumps(data, indent=2), encoding="utf-8")
    # Restrict perms — these are credentials.
    try:
        STORE.chmod(0o600)
    except OSError:
        pass


def cmd_mint(args) -> int:
    data = load()
    token = secrets.token_urlsafe(32)
    data["tokens"].append({
        "token":      token,
        "nick":       args.nick,
        "created_at": int(time.time()),
    })
    save(data)
    print("Token minted. Save this — it won't be shown again:\n")
    print(f"  {token}\n")
    print(f"Tied to nick: {args.nick}")
    print(f"Stored in:    {STORE}")
    return 0


def cmd_list(args) -> int:
    data = load()
    if not data["tokens"]:
        print("(no tokens)")
        return 0
    for t in data["tokens"]:
        ts = time.strftime("%Y-%m-%d %H:%M",
                           time.gmtime(t.get("created_at", 0)))
        # Show only first 6 chars so the list is greppable but the
        # secret stays useless to a shoulder-surfer.
        prefix = (t.get("token") or "")[:6]
        print(f"  {prefix}…  {t.get('nick', '?'):<20s}  created {ts} UTC")
    return 0


def cmd_revoke(args) -> int:
    data = load()
    before = len(data["tokens"])
    data["tokens"] = [t for t in data["tokens"]
                      if not (t.get("token") or "").startswith(args.prefix)]
    removed = before - len(data["tokens"])
    save(data)
    print(f"removed {removed} token(s) matching prefix {args.prefix!r}")
    return 0


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    sub = p.add_subparsers(dest="cmd", required=True)

    pm = sub.add_parser("mint", help="create a new token")
    pm.add_argument("nick")
    pm.set_defaults(func=cmd_mint)

    pl = sub.add_parser("list", help="list existing tokens (prefix only)")
    pl.set_defaults(func=cmd_list)

    pr = sub.add_parser("revoke", help="revoke by token prefix")
    pr.add_argument("prefix")
    pr.set_defaults(func=cmd_revoke)

    args = p.parse_args()
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
