#!/usr/bin/env bash
# =============================================================================
# stage-data.sh — turn the upstream data set into the one that goes on the disc.
# =============================================================================
#   bash tools/fetch-data.sh              # -> ./data      (upstream, untouched)
#   bash tools/stage-data.sh              # -> ./data-dc   (what to burn)
#   TR_DATA=./data-dc bash dc/build-dc.sh
#
# Only the sound effects are rewritten; everything else is copied byte for byte.
#
# WHY (measured, kb/design-audio.md):
#
#   Every WAV in data/sounds/ is 44100 Hz stereo 16-bit. The AICA can only play
#   a sample of at most 65534 samples — 1.486 s at 44.1 kHz — and KOS does not
#   refuse a longer one: snd_sfx_load only warns (snd_sfxmgr.c:409) and
#   snd_sfx_play_ex silently TRUNCATES (snd_sfxmgr.c:766).
#
#   Three of the seven are over that limit, and they are exactly the three
#   continuously-looping terrain sounds:
#
#       tux_on_snow1.wav  2.12 s      tux_on_ice1.wav  2.40 s
#       tux_on_rock1.wav  4.31 s
#
#   Left alone they would each be cut at an arbitrary point and click once per
#   loop, while still costing full sound RAM for audio that can never play.
#
#   The whole set also costs 1,678,784 B of a 2,097,152 B AICA pool, leaving no
#   headroom at all for the streaming ring buffer plus anything added later.
#
# WHAT this does about it: mono, 22050 Hz, and the three loops trimmed to
# 2.90 s (63,945 samples, under the limit with margin). They are wind/scrape
# noise with no melodic content, so a shorter loop is inaudible as a change and
# the resampling is not either through the Dreamcast's audio path.
#
# This belongs in the data pipeline, not in dc/src: the fix is a property of
# the asset, and a runtime resampler would cost CPU every load to produce the
# same bytes.
# =============================================================================
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IN="${1:-$REPO/data}"
OUT="${2:-$REPO/data-dc}"

RATE=22050
MAX_SECONDS=2.90        # 63,945 samples at 22050 Hz; the AICA limit is 65,534

if [ ! -d "$IN" ]; then
    echo "ERROR: '$IN' is not a directory. Run tools/fetch-data.sh first." >&2
    exit 2
fi
if ! command -v ffmpeg >/dev/null 2>&1; then
    echo "ERROR: ffmpeg not found. brew install ffmpeg" >&2
    exit 2
fi

rm -rf "$OUT"
mkdir -p "$OUT"

# Copy everything first, then overwrite the WAVs in place. Simpler than a
# per-file dispatch, and it guarantees nothing is silently dropped.
( cd "$IN" && tar cf - . ) | ( cd "$OUT" && tar xf - )

echo "-- converting sound effects: mono ${RATE} Hz, max ${MAX_SECONDS}s"
total=0
while IFS= read -r rel; do
    src="$IN/$rel"
    dst="$OUT/$rel"
    ffmpeg -nostdin -loglevel error -y -i "$src" \
        -ac 1 -ar "$RATE" -c:a pcm_s16le -t "$MAX_SECONDS" "$dst.tmp.wav"
    mv "$dst.tmp.wav" "$dst"

    bytes=$(wc -c < "$dst")
    samples=$(( (bytes - 44) / 2 ))     # 16-bit mono, 44-byte canonical header
    total=$(( total + bytes ))
    if [ "$samples" -gt 65534 ]; then
        echo "ERROR: $rel is $samples samples, over the AICA's 65534." >&2
        echo "       It would be silently truncated at playback. Lower RATE" >&2
        echo "       or MAX_SECONDS in this script." >&2
        exit 1
    fi
    printf '   %-24s %6d samples  %8d B\n' "$rel" "$samples" "$bytes"
done < <(cd "$IN" && find sounds -name '*.wav' | sort)

echo "-- sfx total: $total B of the 2,097,152 B AICA pool"
echo "-- staged: $OUT ($(find "$OUT" -type f | wc -l | tr -d ' ') files, $(du -sh "$OUT" | cut -f1))"
echo "   next: TR_DATA=$OUT bash dc/build-dc.sh"
