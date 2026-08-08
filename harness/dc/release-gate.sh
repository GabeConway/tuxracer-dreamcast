#!/usr/bin/env bash
# =============================================================================
# release-gate.sh — the check that v0.1.0 did not have.
# =============================================================================
#   harness/dc/release-gate.sh            # build exactly what CI ships, boot it
#   TR_DATA=/path/to/data harness/dc/release-gate.sh
#
# v0.1.0 shipped a black screen. Everything that passed before it was tagged
# was built with extra defines -- playtest.sh adds -DTR_AUTOKEY and
# -DTR_AUTOEXIT, the audio work was verified with -DTR_AUDIO_TRACE -- and this
# port has a latent memory bug whose symptom is decided purely by where things
# land in the heap (kb/design-perf.md). Every one of those defines moves the
# heap. The release image had none of them and hung before its first frame.
#
# So this script builds with NO defines at all, which is byte-for-byte the
# configuration CI produces, and asserts the guest reaches MARK:FIRST_FRAME.
#
# It cannot assert more than that. A shipping build has no TR_AUTOEXIT, so it
# never ends and never presses a key: no end marker, no race. `timeout` is
# therefore the expected runner status and is NOT a failure here -- reaching
# the first frame is. Use playtest.sh for the deeper gate, knowing that it
# tests a different set of bytes than the one you are about to release.
#
# Run this before every tag.
# =============================================================================
set -uo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DATA="${TR_DATA:-$REPO/data-dc}"
TIMEOUT="${TR_GATE_TIMEOUT:-90}"

if [ ! -d "$DATA" ]; then
    echo "release-gate: no data at $DATA (bash tools/fetch-data.sh)" >&2
    exit 2
fi

echo "-- building the shipping configuration (no extra defines)"
env -u TR_DEFS -u TR_AUDIO_TRACE -u TR_FBDUMP_FRAME \
    TR_DATA="$DATA" bash "$REPO/dc/build-dc.sh" >/dev/null 2>&1 || {
        echo "release-gate: build failed" >&2
        exit 1
    }

IMG="$REPO/dc/build/TuxRacer.cdi"
echo "-- booting $IMG"
bash "$REPO/harness/dc/console.sh" "$IMG" --timeout "$TIMEOUT" >/dev/null 2>&1

RUN="$(ls -td "${TR_DC_RUNROOT:-$HOME/.cache/tr-dc-harness/runs}"/* | head -1)"
LOG="$RUN/console.log"

frames=$(grep -c 'MARK:FIRST_FRAME' "$LOG" 2>/dev/null || echo 0)
perf=$(grep -c '^PERF fps=' "$LOG" 2>/dev/null || echo 0)

printf '{\n'
printf '  "harness": "release-gate",\n'
printf '  "image": "%s",\n' "$IMG"
printf '  "console": "%s",\n' "$LOG"
printf '  "first_frame": %s,\n' "$frames"
printf '  "perf_records": %s,\n' "$perf"
printf '  "ok": %s\n' "$([ "$frames" -ge 1 ] && echo true || echo false)"
printf '}\n'

if [ "$frames" -lt 1 ]; then
    echo "RELEASE GATE FAIL -- no MARK:FIRST_FRAME. Last lines:" >&2
    tail -8 "$LOG" >&2
    exit 1
fi

echo "RELEASE GATE PASS -- first frame reached, $perf PERF records" >&2
exit 0
