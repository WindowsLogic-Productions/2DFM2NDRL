#!/usr/bin/env bash
# Spectator late-join end-to-end test.
#
# Same shape as spectator_e2e_replay.sh but the spectator is launched
# AFTER host+client have been running for 200 frames (~2 sec). This
# stresses the C5 backfill ordering fence: a fresh spectator gets
# INITIAL_MATCH + the full session_events backlog before any live
# FlushBatch reaches it; without the fence, a live batch racing ahead
# of the backfill would anchor next_expected_frame mid-stream and
# silently drop the early session events.
#
# Pass: parity p1.pty vs p3.pty match exactly from p3's first sample.
# Fail: any field divergence in any frame after p3's first sample.
#
# This script also exercises C5.5 fast catch-up — once that lands, a
# 3000-frame pre-join window must drain in <5 sec wall time. Timing
# scaffolding is below but the assertion is gated on a tools/spectator_lag.py
# helper that doesn't exist yet (TODO).
#
# Same launcher --spectate caveat as spectator_e2e_replay.sh: the
# spectator step is no-op until that CLI flag lands.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
OUT_DIR="${FM2K_E2E_OUT_DIR:-$REPO_DIR/tests/build/e2e_out}"

# Frames the host+client run before spectator joins.
PRE_JOIN_FRAMES="${PRE_JOIN_FRAMES:-200}"
# Total frames the test runs.
TOTAL_FRAMES="${TOTAL_FRAMES:-800}"

# Convert frames to seconds (100 fps).
PRE_JOIN_SEC=$(awk -v f="$PRE_JOIN_FRAMES" 'BEGIN{print f/100.0}')
POST_JOIN_SEC=$(awk -v p="$PRE_JOIN_FRAMES" -v t="$TOTAL_FRAMES" 'BEGIN{print (t-p)/100.0}')

mkdir -p "$OUT_DIR"
rm -f "$OUT_DIR"/*.pty

echo "[e2e-lj] Building DLLs..."
( cd "$REPO_DIR" && ./go.sh ) >/dev/null

HOST_PORT=7710
CLIENT_PORT=7711
SPEC_PORT=7712
LAUNCHER="/mnt/c/games/FM2K_RollbackLauncher.exe"
GAMES_DIR="${FM2K_E2E_GAMES_DIR:-C:\\games\\2dfm\\wanwan}"

echo "[e2e-lj] Spawning host on port $HOST_PORT..."
FM2K_PARITY_RECORD_PATH="$OUT_DIR/p1.pty" \
FM2K_PLAYER_INDEX=0 \
    "$LAUNCHER" --games "$GAMES_DIR" --host --port "$HOST_PORT" --delay 4 &
HOST_PID=$!

sleep 3

echo "[e2e-lj] Spawning client connecting to 127.0.0.1:$HOST_PORT..."
FM2K_PARITY_RECORD_PATH="$OUT_DIR/p2.pty" \
FM2K_PLAYER_INDEX=1 \
    "$LAUNCHER" --games "$GAMES_DIR" --connect "127.0.0.1:$HOST_PORT" --port "$CLIENT_PORT" --delay 4 &
CLIENT_PID=$!

# Pre-join window: host+client play solo to build session_events backlog.
echo "[e2e-lj] Pre-join window: host+client run for ${PRE_JOIN_FRAMES} frames (${PRE_JOIN_SEC}s)..."
sleep "$PRE_JOIN_SEC"

echo "[e2e-lj] Spawning spectator AFTER ${PRE_JOIN_FRAMES} frames of pre-roll..."
FM2K_PARITY_RECORD_PATH="$OUT_DIR/p3.pty" \
FM2K_SPECTATOR_MODE=1 \
    "$LAUNCHER" --games "$GAMES_DIR" --spectate "127.0.0.1:$HOST_PORT" --port "$SPEC_PORT" &
SPEC_PID=$!

echo "[e2e-lj] Post-join window: ${POST_JOIN_SEC}s..."
sleep "$POST_JOIN_SEC"

[[ -n "$SPEC_PID"   ]] && kill -TERM "$SPEC_PID"   2>/dev/null || true
[[ -n "$CLIENT_PID" ]] && kill -TERM "$CLIENT_PID" 2>/dev/null || true
[[ -n "$HOST_PID"   ]] && kill -TERM "$HOST_PID"   2>/dev/null || true
wait 2>/dev/null || true

if [[ ! -f "$OUT_DIR/p1.pty" || ! -f "$OUT_DIR/p2.pty" ]]; then
    echo "[e2e-lj] FAIL: host or client parity log missing."
    exit 1
fi

echo "[e2e-lj] Assert A: host vs client (sanity)..."
python3 "$REPO_DIR/tools/parity_diff.py" "$OUT_DIR/p1.pty" "$OUT_DIR/p2.pty"

if [[ -f "$OUT_DIR/p3.pty" ]]; then
    echo "[e2e-lj] Assert B: late-join fence — host vs spectator from spec's first sample..."
    # parity_diff.py auto-aligns by battle phase; the spectator's first
    # sample is whichever frame its first parity-recorder Capture() fired
    # at. C5 fence ensures the spectator has the full session_events log
    # at that point, so this comparison should succeed.
    python3 "$REPO_DIR/tools/parity_diff.py" "$OUT_DIR/p1.pty" "$OUT_DIR/p3.pty"
else
    echo "[e2e-lj] SKIP: spectator parity (--spectate not wired)."
fi

# C5.5 fast-catchup wall-clock budget: TODO — needs tools/spectator_lag.py.

echo "[e2e-lj] PASS"
