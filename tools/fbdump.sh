#!/usr/bin/env bash
# =============================================================================
# fbdump.sh — build with the framebuffer dump armed, boot it, decode the PNG.
# =============================================================================
#   bash tools/fbdump.sh                 # frame 120 (~2 s in), -> /tmp/tr-fb.png
#   bash tools/fbdump.sh 600 out.png     # a specific frame and destination
#
# Exists because there is no other way to see the screen: Flycast has no
# headless mode and host-side capture is blocked on this machine by the Screen
# Recording and Accessibility TCC prompts (harness/dc/README.md). The guest
# ships the framebuffer over the serial console instead
# (dc/src/dc_fbdump.c), and this decodes it.
#
# It rebuilds, because -DTR_FBDUMP_FRAME is a compile-time knob. Nothing else
# in the build changes, so the next ordinary build is a normal incremental one.
# =============================================================================
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FRAME="${1:-120}"
OUT="${2:-/tmp/tr-fb.png}"
DATA="${TR_DATA:-$REPO/data-dc}"

# make tracks source timestamps, not compiler flags, so changing
# -DTR_FBDUMP_FRAME alone rebuilds NOTHING and the run silently produces no
# dump — which reads as "the dump is broken". Touch the two TUs that consume
# the define so the flag change actually lands.
touch "$REPO/dc/src/dc_winsys.c" "$REPO/dc/src/dc_fbdump.c"

echo "-- building with TR_FBDUMP_FRAME=$FRAME"
TR_FBDUMP_FRAME="$FRAME" TR_DATA="$DATA" bash "$REPO/dc/build-dc.sh" \
    | grep -E 'CDI:|error' || true

# The dump is ~205 KB of base64 at ~150 KB/s, so allow generously more wall
# clock than the frame number alone suggests.
echo "-- booting"
# rend.EmulateFramebuffer=yes is NOT optional and NOT a tuning knob.
# MEASURED: Flycast's hardware renderer draws into a host GPU surface and never
# writes the result back into emulated VRAM, so the guest reads real, writable,
# genuinely empty memory. Same test program, same frame:
#     without  ->  FBPROBE nonzero=0/307200
#     with     ->  FBPROBE nonzero=307200/307200
# Without this flag every dump is black and looks exactly like a renderer bug.
# It costs frame rate, which is why it lives here and not in _runner.py's
# BASE_CONFIG.
bash "$REPO/harness/dc/console.sh" "$REPO/dc/build/TuxRacer.cdi" \
     -c config:rend.EmulateFramebuffer=yes \
     --timeout "${TR_FBDUMP_TIMEOUT:-120}" >/dev/null 2>&1 || true

LOG="$(ls -td "$HOME"/.cache/tr-dc-harness/runs/* | head -1)/console.log"
echo "-- log: $LOG"
python3 "$REPO/tools/fbdump-to-png.py" "$LOG" "$OUT"
