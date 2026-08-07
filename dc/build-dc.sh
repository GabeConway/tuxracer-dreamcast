#!/usr/bin/env bash
# =============================================================================
# build-dc.sh — HOST-side wrapper. Runs dc/build-dc-docker.sh in the SDK image.
# =============================================================================
#   bash dc/build-dc.sh                  # full build -> ELF + unpadded CDI
#   DC_TARGET=objs bash dc/build-dc.sh   # compile only, no link (the M1 signal)
#   TR_DATA=/path/to/data bash dc/build-dc.sh
#                                        # put the game data on the disc
#   TR_OPT=-O0 bash dc/build-dc.sh       # game TUs unoptimised (bisecting)
#   DC_CDI_PAD=1 bash dc/build-dc.sh     # padded 740 MB CDI for a CD-R burn
#   JOBS=8 bash dc/build-dc.sh
#   bash dc/build-dc.sh clean            # rm -rf dc/build
#
# The image is built once by dc/build-dc-image.sh; this script never builds it.
#
# --platform linux/arm64 is explicit on purpose: without it an accidental amd64
# pull silently drops the whole build into qemu, which is slow AND flaky.
# =============================================================================
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMAGE="${TR_DC_SDK_IMAGE:-tuxracer-dc:sdk}"

if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
    echo "ERROR: docker image '$IMAGE' not found." >&2
    echo "       Build it once with: bash $REPO/dc/build-dc-image.sh" >&2
    exit 2
fi

if [ "${1:-}" = "clean" ]; then
    exec docker run --rm --platform linux/arm64 \
        -v "$REPO":/work "$IMAGE" \
        bash -c 'make -C /work/dc clean'
fi

# Forward-only for the knobs that dc/Makefile defaults with `?=`. `make` treats
# an environment variable as already-defined even when it is empty, so a plain
# `-e TR_OPT=` would blank the Makefile's default and let $KOS_CFLAGS' own -O2
# win silently. Variables with a real default on both sides are passed
# unconditionally with an explicit fallback.
ENVARGS=(
    -e JOBS="${JOBS:-4}"
    -e DC_TARGET="${DC_TARGET:-all}"
    -e DC_CDI_PAD="${DC_CDI_PAD:-0}"
)
[ -n "${TR_OPT+x}"    ] && ENVARGS+=(-e TR_OPT="$TR_OPT")
[ -n "${TR_DC_OPT+x}" ] && ENVARGS+=(-e TR_DC_OPT="$TR_DC_OPT")
[ -n "${TR_DEFS+x}"   ] && ENVARGS+=(-e TR_DEFS="$TR_DEFS")
[ -n "${TR_LDFLAGS+x}" ] && ENVARGS+=(-e TR_LDFLAGS="$TR_LDFLAGS")
[ -n "${V+x}"         ] && ENVARGS+=(-e V="$V")
[ -n "${TR_HARNESS+x}" ] && ENVARGS+=(-e TR_HARNESS="$TR_HARNESS")
[ -n "${TR_FBDUMP_FRAME+x}" ] && ENVARGS+=(-e TR_FBDUMP_FRAME="$TR_FBDUMP_FRAME")

# TR_DATA=<dir> puts the game data on the disc. It is mounted read-only at
# /discroot and handed to mkdcdisc with -D (contents, not the directory
# itself), so the files land at the DISC ROOT — which is what DATA_DIR="/cd"
# in dc/include/config.h expects. Get this wrong and every asset open misses
# with no clue why.
#
#   bash tools/fetch-data.sh          # -> ./data/
#   TR_DATA=./data bash dc/build-dc.sh
MOUNTARGS=()
if [ -n "${TR_DATA:-}" ]; then
    if [ ! -d "$TR_DATA" ]; then
        echo "ERROR: TR_DATA='$TR_DATA' is not a directory." >&2
        echo "       Fetch the data first: bash tools/fetch-data.sh" >&2
        exit 2
    fi
    MOUNTARGS+=(-v "$(cd "$TR_DATA" && pwd)":/discroot:ro)
fi

# "${MOUNTARGS[@]}" alone is an UNBOUND VARIABLE under `set -u` in bash 3.2
# (the macOS system bash) when the array is empty. The +"${...}" form expands
# to nothing instead.
exec docker run --rm --platform linux/arm64 \
    -v "$REPO":/work \
    ${MOUNTARGS[@]+"${MOUNTARGS[@]}"} \
    "${ENVARGS[@]}" \
    "$IMAGE" \
    bash /work/dc/build-dc-docker.sh
