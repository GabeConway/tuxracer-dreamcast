#!/usr/bin/env bash
# =============================================================================
# build-dc-docker.sh — runs INSIDE the tuxracer-dc:sdk container.
# =============================================================================
# The image's dc-env entrypoint has already exported KOS_BASE, KOS_PORTS,
# KOS_CFLAGS, KOS_LDFLAGS and a PATH carrying sh-elf-*, kos-cc/kos-c++ and
# mkdcdisc, so this script does no toolchain setup of its own.
#
# OUTPUTS (inside the bind mount, so they survive the container)
#   /work/dc/build/tuxracer.elf        unstripped ELF — addr2line needs it
#   /work/dc/build/tuxracer.map        link map
#   /work/dc/build/TuxRacer.cdi        Flycast-loadable / burnable image
#   /work/dc/build/TuxRacer.cdi.src.json   ELF provenance sidecar (see below)
# =============================================================================
set -uo pipefail

DC_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$DC_DIR/.." && pwd)"
BUILD="$DC_DIR/build"

JOBS="${JOBS:-4}"
DC_TARGET="${DC_TARGET:-all}"
ELF="$BUILD/tuxracer.elf"
CDI="$BUILD/TuxRacer.cdi"

echo "=============================================================="
echo " tuxracer-dreamcast build"
echo "   repo    : $ROOT"
echo "   target  : $DC_TARGET"
echo "   jobs    : $JOBS"
echo "   KOS_BASE: ${KOS_BASE:-<unset!>}"
echo "   gcc     : $(sh-elf-gcc -dumpversion 2>/dev/null || echo '<missing>')"
echo "=============================================================="

if [ -z "${KOS_BASE:-}" ]; then
    echo "ERROR: KOS_BASE is unset. This script must run inside" >&2
    echo "       tuxracer-dc:sdk (its entrypoint exports the KOS env)." >&2
    exit 2
fi

# ---------------------------------------------------------------------------
# 1. Compile / link
# ---------------------------------------------------------------------------
START=$(date +%s)
make -C "$DC_DIR" -j"$JOBS" "$DC_TARGET"
RC=$?
END=$(date +%s)
echo "-- make $DC_TARGET finished in $((END - START))s (rc=$RC)"
[ $RC -ne 0 ] && { echo "ERROR: build failed." >&2; exit $RC; }

# `objs` deliberately does not link, so there is nothing to package. Same for
# the informational targets.
case "$DC_TARGET" in
    objs|count|sources|clean)
        echo "-- DC_TARGET=$DC_TARGET: no link step, skipping CDI."
        exit 0
        ;;
esac

if [ ! -f "$ELF" ]; then
    echo "ERROR: $ELF was not produced." >&2
    exit 1
fi
sh-elf-size "$ELF" || true

# ---------------------------------------------------------------------------
# 2. CDI
# ---------------------------------------------------------------------------
# -N (no padding) by default. MEASURED on this toolchain:
#   mkdcdisc     -e elf -o out.cdi  ->  740,083,145 B, 15.6 s
#   mkdcdisc -N  -e elf -o out.cdi  ->    1,783,337 B,  0.021 s
# 740 MB per iteration is untenable for the Flycast loop. The padding is not
# "outer track placement" — it is appended AFTER the filesystem, so the game
# data sits on the innermost tracks either way. Its only real use is making a
# timing run read-speed-realistic, and burning a CD-R.
PAD_ARGS=(-N)
PAD_DESC="unpadded (-N; fast Flycast loop)"
if [ "${DC_CDI_PAD:-0}" = "1" ]; then
    PAD_ARGS=()
    PAD_DESC="PADDED (CD-R burn / read-speed-realistic timing)"
fi

# -D, not -d: `-d` includes the root DIRECTORY ITSELF, putting everything at
# /cd/discroot/<name>. DATA_DIR is "/cd" (dc/include/config.h), so every asset
# open would miss with no clue why. `-D` includes its CONTENTS.
DISC_ARGS=()
if [ -d /discroot ]; then
    DISC_ARGS=(-D /discroot)
    echo "-- disc content: /discroot ($(find /discroot -type f | wc -l) files, $(du -sh /discroot 2>/dev/null | cut -f1))"
else
    echo "-- disc content: NONE (ELF only). Every asset open will miss."
    echo "   Fetch the data and rebuild with TR_DATA=./data."
fi

echo "-- mkdcdisc: $PAD_DESC"
START=$(date +%s)
mkdcdisc "${PAD_ARGS[@]}" -e "$ELF" "${DISC_ARGS[@]}" -n "TuxRacer" -o "$CDI"
RC=$?
END=$(date +%s)
[ $RC -ne 0 ] && { echo "ERROR: mkdcdisc failed (rc=$RC)." >&2; exit $RC; }

echo "-- CDI: $CDI  $(wc -c < "$CDI") bytes  ($((END - START))s)"

# ---------------------------------------------------------------------------
# 3. ELF provenance sidecar — REQUIRED of every CDI producer
# ---------------------------------------------------------------------------
# A CDI carries a scrambled, stripped 1ST_READ.BIN, so a crash inside one is
# just hex. harness/dc/crash.sh can only symbolise it if it can find the exact
# ELF the image was built from AND prove that ELF has not been rebuilt since.
# It refuses to guess, and refuses to symbolise against a mismatched sha256,
# because a confidently wrong line number is worse than no answer.
cat > "$CDI.src.json" <<EOF
{
  "image": "$CDI",
  "elf": "$ELF",
  "elf_sha256": "$(sha256sum "$ELF" | cut -d' ' -f1)",
  "elf_size": $(wc -c < "$ELF"),
  "built_utc": "$(date -u +%Y-%m-%dT%H:%M:%SZ)",
  "toolchain_image": "${TR_DC_SDK_IMAGE:-tuxracer-dc:sdk}",
  "producer": "dc/build-dc-docker.sh"
}
EOF
echo "-- sidecar: $CDI.src.json"
echo "-- done."
