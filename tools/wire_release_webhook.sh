#!/bin/bash
# Wire a Discord channel webhook to fire on every Armonte/fm2ktest
# release. GitHub natively supports Discord by appending /github to
# the webhook URL — no server-side relay needed.
#
# Usage:
#   ./tools/wire_release_webhook.sh <discord-webhook-url> [<owner/repo>]
#
# Defaults to Armonte/fm2ktest if no repo is given. Override with the
# 2nd positional or RELEASE_REPO=owner/repo env var. Designed to be
# cloned and reused for any release-firing repo (azfight-rollback,
# whatever future games show up).
#
# Where <discord-webhook-url> is the raw URL Discord gives you in
#   Channel → Integrations → Webhooks → New Webhook → Copy URL.
# Format: https://discord.com/api/webhooks/<id>/<token>
# (the /github suffix is added automatically below)
#
# Requires: gh CLI authenticated as a user with admin on the target.

set -eu

if [ $# -lt 1 ]; then
    echo "usage: $0 <discord-webhook-url> [<owner/repo>]" >&2
    echo "  default repo: Armonte/fm2ktest (override via 2nd arg or RELEASE_REPO env var)" >&2
    exit 2
fi

URL="$1"
REPO="${2:-${RELEASE_REPO:-Armonte/fm2ktest}}"
echo "Target repo: $REPO"

# Sanity-check the URL shape.
if ! [[ "$URL" =~ ^https://discord\.com/api/webhooks/[0-9]+/[A-Za-z0-9_-]+$ ]]; then
    echo "URL doesn't look like a Discord webhook (https://discord.com/api/webhooks/<id>/<token>)" >&2
    exit 2
fi

# Append /github so Discord parses GitHub's release payload.
GITHUB_URL="${URL}/github"

# Skip if already wired (idempotent).
EXISTING=$(gh api "repos/${REPO}/hooks" --jq '.[] | select(.config.url | contains("discord.com"))' 2>/dev/null || true)
if [ -n "$EXISTING" ]; then
    echo "A Discord webhook already exists on ${REPO}:"
    echo "$EXISTING" | python3 -c "import json,sys; d=json.loads(sys.stdin.read()); print(f'  hook_id={d[\"id\"]}  events={d[\"events\"]}  url={d[\"config\"][\"url\"][:60]}...')"
    read -r -p "Replace it? [y/N] " ans
    if [[ "${ans:-N}" != [Yy]* ]]; then
        echo "aborted."
        exit 0
    fi
    HOOK_ID=$(echo "$EXISTING" | python3 -c "import json,sys; print(json.loads(sys.stdin.read())['id'])")
    gh api -X DELETE "repos/${REPO}/hooks/${HOOK_ID}"
    echo "  deleted old hook ${HOOK_ID}"
fi

echo "Registering release webhook on ${REPO}..."
gh api "repos/${REPO}/hooks" \
    -f name=web \
    -f "config[url]=${GITHUB_URL}" \
    -f "config[content_type]=json" \
    -F "events[]=release" \
    -F active=true >/dev/null

echo "✓ wired. Test it:"
echo "    gh api repos/${REPO}/hooks --jq '.[].events'"
echo
echo "Next release published on ${REPO} will post to that Discord channel."
echo "Discord renders the GitHub embed natively (release name, body, assets)."
