#!/usr/bin/env bash
# Post a Discord webhook embed for a release. Called by cut_release.sh
# and promote_release.sh; also runnable standalone to retry a missed
# webhook for an existing release tag.
#
# Standalone usage:
#   ./scripts/post_release_webhook.sh stable 0.2.54
#   ./scripts/post_release_webhook.sh dev    0.2.54
#   ./scripts/post_release_webhook.sh promote 0.2.54   # stable-channel post,
#                                                       # promotion-flavored copy
#
# Env vars (loaded from ~/.config/fm2k-release.env if present, then
# inherited from the parent environment so the cut_release.sh /
# promote_release.sh wrappers can pre-set them too):
#   FM2K_RELEASE_WEBHOOK_STABLE     full Discord webhook URL
#   FM2K_RELEASE_WEBHOOK_DEV        full Discord webhook URL
#   FM2K_RELEASE_ROLE_STABLE        numeric Discord role id (optional)
#   FM2K_RELEASE_ROLE_DEV           numeric Discord role id (optional)
#
# Notes are read from stdin OR the optional 3rd positional arg. If both
# are absent, falls back to "Released v<version>".
#
# The webhook URL for the appropriate channel must be set, otherwise
# the script exits 1. (Wrappers can opt into silent-skip by setting
# FM2K_RELEASE_WEBHOOK_OPTIONAL=1.)

set -euo pipefail

if [ $# -lt 2 ]; then
    echo "usage: $0 <stable|dev|promote> <version> [notes]" >&2
    echo "       echo 'notes' | $0 stable 0.2.54" >&2
    exit 1
fi

KIND="$1"   # stable | dev | promote (promote = stable webhook with promotion copy)
VERSION="$2"
shift 2

# Notes: arg if given, else stdin if piped, else default.
if [ $# -gt 0 ]; then
    NOTES="$1"
elif [ ! -t 0 ]; then
    NOTES="$(cat)"
else
    NOTES="Released v${VERSION}"
fi

# Load env file if present. Variables already in the environment win
# (e.g. wrappers can override on a per-call basis).
ENV_FILE="${FM2K_RELEASE_ENV_FILE:-$HOME/.config/fm2k-release.env}"
if [ -f "$ENV_FILE" ]; then
    set -a
    # shellcheck disable=SC1090
    . "$ENV_FILE"
    set +a
fi

case "$KIND" in
    stable|promote)
        WEBHOOK_URL="${FM2K_RELEASE_WEBHOOK_STABLE:-}"
        ROLE_ID="${FM2K_RELEASE_ROLE_STABLE:-}"
        COLOR=3066993        # green (#2ECC71)
        TITLE_PREFIX=""
        TITLE_SUFFIX=""
        if [ "$KIND" = "promote" ]; then
            TITLE_SUFFIX=" — promoted to stable"
        fi
        ;;
    dev)
        WEBHOOK_URL="${FM2K_RELEASE_WEBHOOK_DEV:-}"
        ROLE_ID="${FM2K_RELEASE_ROLE_DEV:-}"
        COLOR=15105570       # orange (#E67E22)
        # Construction-zone emoji book-ends so the embed reads
        # "🚧 v0.2.54 — dev pre-release ⚠️" — visually distinct from
        # the plain green stable cards in the same channel, hard to
        # mistake for a stable announcement at a glance.
        TITLE_PREFIX="🚧 "
        TITLE_SUFFIX=" — dev pre-release ⚠️"
        ;;
    *)
        echo "$0: unknown KIND '$KIND' (want stable | dev | promote)" >&2
        exit 1
        ;;
esac

if [ -z "$WEBHOOK_URL" ]; then
    if [ "${FM2K_RELEASE_WEBHOOK_OPTIONAL:-0}" = "1" ]; then
        echo "$0: no webhook URL for '$KIND', skipping (optional mode)" >&2
        exit 0
    fi
    echo "$0: no webhook URL for '$KIND'. Set FM2K_RELEASE_WEBHOOK_${KIND^^}" >&2
    echo "    in $ENV_FILE or in the environment." >&2
    exit 1
fi

if ! command -v jq >/dev/null 2>&1; then
    echo "$0: jq required (apt-get install -y jq)" >&2
    exit 1
fi

CONTENT=""
if [ -n "$ROLE_ID" ]; then
    CONTENT="<@&${ROLE_ID}>"
fi
DOWNLOAD_URL="https://github.com/Armonte/fm2ktest/releases/download/v${VERSION}/fm2k_v${VERSION}.zip"
RELEASE_PAGE="https://github.com/Armonte/fm2ktest/releases/tag/v${VERSION}"

PAYLOAD="$(jq -n \
    --arg content "$CONTENT" \
    --arg title   "${TITLE_PREFIX}v${VERSION}${TITLE_SUFFIX}" \
    --arg desc    "$NOTES" \
    --arg url     "$RELEASE_PAGE" \
    --arg dl_name "fm2k_v${VERSION}.zip" \
    --arg dl_url  "$DOWNLOAD_URL" \
    --argjson color "$COLOR" \
    '{
      content: $content,
      embeds: [{
        title: $title,
        url: $url,
        description: $desc,
        color: $color,
        fields: [
          { name: "Download", value: ("[" + $dl_name + "](" + $dl_url + ")"), inline: false }
        ]
      }],
      allowed_mentions: { parse: ["roles"] }
    }')"

RESP_FILE="$(mktemp)"
HTTP_CODE="$(curl -s -o "$RESP_FILE" -w "%{http_code}" \
    -X POST -H "Content-Type: application/json" \
    -d "$PAYLOAD" "$WEBHOOK_URL")"

if [ "$HTTP_CODE" = "204" ] || [ "$HTTP_CODE" = "200" ]; then
    echo "Discord webhook posted (HTTP ${HTTP_CODE})"
    rm -f "$RESP_FILE"
    exit 0
else
    echo "Discord webhook FAILED (HTTP ${HTTP_CODE})" >&2
    echo "  payload preview: $(echo "$PAYLOAD" | head -c 200)" >&2
    echo "  response: $(head -c 400 "$RESP_FILE")" >&2
    rm -f "$RESP_FILE"
    exit 1
fi
