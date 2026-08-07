#!/usr/bin/env bash
# Builds harness/dc/audiotest/audiotest.elf in the SDK container. See
# audiotest.c for what it answers. ELF only -- Flycast boots raw ELFs via reios
# and this program touches no disc.
set -euo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
IMAGE="${TR_DC_TOOLCHAIN_IMAGE:-tuxracer-dc:sdk}"
mkdir -p "$REPO/dc/build"
# The ARM7 side is assembled on the host: the SDK image has no ARM compiler.
python3 "$REPO/harness/dc/audiotest/mkfiqprobe.py"
docker run --rm --platform linux/arm64 -v "$REPO":/work "$IMAGE" bash -c '
  set -e
  cd /work/harness/dc/audiotest
  kos-cc -std=gnu99 -O2 -g -fno-omit-frame-pointer -Wall -Wextra \
      -c audiotest.c -o /tmp/audiotest.o
  kos-cc -o /work/dc/build/audiotest.elf /tmp/audiotest.o -lm
  sh-elf-size /work/dc/build/audiotest.elf'
