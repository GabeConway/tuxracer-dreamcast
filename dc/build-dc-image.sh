#!/usr/bin/env bash
# =============================================================================
# build-dc-image.sh — build the tuxracer-dc:sdk toolchain image. Run once.
# =============================================================================
# Two recipes produce an equivalent image:
#
#   dc/Dockerfile.overlay   FROM opencrossing-dc:sdk, adds libjimtcl and
#                           libmodplug.                       ~2 min
#   dc/Dockerfile           standalone, FROM debian:bookworm, builds the whole
#                           sh-elf toolchain + KOS + GLdc.    ~27 min
#
# The overlay is chosen automatically when the base image is present, because
# rebuilding a 27-minute toolchain to add two ports is pure waste. Force the
# standalone path with TR_DC_FORCE_STANDALONE=1 — do that if you ever need to
# prove the image can be built from nothing.
#
# NOTE: this host has no BuildKit. DOCKER_BUILDKIT=0 is not optional, and
# --progress is unsupported and hard-fails.
# =============================================================================
set -euo pipefail

DC_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IMAGE="${TR_DC_SDK_IMAGE:-tuxracer-dc:sdk}"
BASE="${TR_DC_BASE_IMAGE:-opencrossing-dc:sdk}"

if docker image inspect "$IMAGE" >/dev/null 2>&1 && [ "${TR_DC_REBUILD:-0}" != "1" ]; then
    echo "-- $IMAGE already exists. TR_DC_REBUILD=1 to rebuild."
    exit 0
fi

if [ "${TR_DC_FORCE_STANDALONE:-0}" != "1" ] && docker image inspect "$BASE" >/dev/null 2>&1; then
    echo "-- base image $BASE found: building the overlay (~2 min)."
    DOCKERFILE="$DC_DIR/Dockerfile.overlay"
else
    echo "-- building standalone from debian:bookworm (~27 min)."
    DOCKERFILE="$DC_DIR/Dockerfile"
fi

# --platform linux/arm64 is explicit on purpose: without it an accidental amd64
# pull silently drops the whole build into qemu, which is slow AND flaky.
DOCKER_BUILDKIT=0 exec docker build \
    --platform linux/arm64 \
    -f "$DOCKERFILE" \
    -t "$IMAGE" \
    "$DC_DIR"
