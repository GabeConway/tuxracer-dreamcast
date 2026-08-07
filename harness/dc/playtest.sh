#!/usr/bin/env bash
# =============================================================================
# playtest.sh — the real gate: does the game still reach a race?
# =============================================================================
#   harness/dc/playtest.sh                 # build, boot, assert MARK:RACING
#   TR_PLAY_FRAMES=9000 harness/dc/playtest.sh
#
# smoke.sh cannot do this. A game never exits and neither does Flycast, so
# every run of the shipping build ends in a timeout, which is indistinguishable
# from a hang. This builds with two harness-only knobs instead:
#
#   TR_AUTOKEY   synthesises Enter every N frames, which walks splash ->
#                game type -> event -> race select -> a race
#   TR_AUTOEXIT  ends the run cleanly after N frames so there IS an end marker
#
# and then asserts the guest printed MARK:RACING, which dc/src/dc_winsys.c
# emits the first frame g_game.mode reaches RACING.
#
# Course loading takes ~40 s of emulated time, so the default deadline is
# generous. Do not shorten it below ~200 s: a too-short timeout looks exactly
# like the course-load stall in kb/design-perf.md, and that cost a session.
# =============================================================================
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# 1500 frames: ~590 in the menus at 59 fps, then the course load (which
# renders nothing), then ~900 frames of racing at 12-15 fps. Larger values
# blow past the deadline in the racing view, which is slow by design.
FRAMES="${TR_PLAY_FRAMES:-1500}"
DATA="${TR_DATA:-$REPO/data-dc}"

echo "-- building playtest image (autokey + autoexit at frame $FRAMES)"
TR_DEFS="-DTR_AUTOKEY=120 -DTR_AUTOEXIT=$FRAMES" TR_DATA="$DATA" \
    bash "$REPO/dc/build-dc.sh" | grep -E 'CDI:|error' || true

exec bash "$REPO/harness/dc/smoke.sh" "$REPO/dc/build/TuxRacer.cdi" \
     --timeout "${TR_PLAY_TIMEOUT:-320}" \
     --expect 'MARK:BOOT_OK' \
     --expect 'MARK:FIRST_FRAME' \
     --expect 'MARK:RACING'
