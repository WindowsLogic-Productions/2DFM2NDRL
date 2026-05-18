#!/usr/bin/env bash
# Orchestrator for the atwiki -> Wayback -> archive pipeline.
#
# Phases run in dependency order. Each phase is idempotent and resumable
# — re-running the script after a partial failure picks up where the
# previous run left off. Set PULL=1 to actually download the archives in
# phase 4 (default is metadata-only, since 339 games × ~50 MB averages
# ~17 GB and you probably want to spot-check the plan first).
#
# Phase order:
#   1) atwiki_extract_index.py      — index.json from cached HTML
#   2) atwiki_crawl_pages.py        — per-game HTML via Playwright
#   3) atwiki_parse_pages.py        — HTML -> structured per-game JSON
#   4) atwiki_resolve_archives.py   — outbound URLs -> Wayback CDX
#   5) atwiki_fetch_downloads.py    — discover + (optional) pull archives
#   6) atwiki_crossref.py           — match downloaded exes to launcher's
#                                     known-clean registry + local cache

set -euo pipefail
cd "$(dirname "$0")/.."

PULL=${PULL:-0}
LIMIT=${LIMIT:-0}     # smoke-test cap; 0 = no limit

LIMIT_FLAG=()
[[ $LIMIT -gt 0 ]] && LIMIT_FLAG=(--limit "$LIMIT")

echo "==> phase 1: extract index"
python3 tools/atwiki_extract_index.py

echo "==> phase 2: crawl pages (Playwright + CF stealth)"
python3 tools/atwiki_crawl_pages.py "${LIMIT_FLAG[@]}"

echo "==> phase 2b: parse cached HTML"
python3 tools/atwiki_parse_pages.py "${LIMIT_FLAG[@]}"

echo "==> phase 3: resolve outbound URLs to Wayback"
python3 tools/atwiki_resolve_archives.py "${LIMIT_FLAG[@]}"

echo "==> phase 4: discover download URLs from snapshots"
if [[ $PULL == "1" ]]; then
    echo "    (pulling archives — may consume disk + time)"
    python3 tools/atwiki_fetch_downloads.py --pull "${LIMIT_FLAG[@]}"
else
    echo "    (metadata-only — pass PULL=1 to actually download)"
    python3 tools/atwiki_fetch_downloads.py "${LIMIT_FLAG[@]}"
fi

if [[ $PULL == "1" ]]; then
    echo "==> phase 5: cross-reference exes with launcher registry"
    python3 tools/atwiki_crossref.py "${LIMIT_FLAG[@]}"
else
    echo "==> phase 5 skipped (no archives pulled — re-run with PULL=1)"
fi

echo
echo "done. inspect: data/fm2k_wiki/games/*.json"
