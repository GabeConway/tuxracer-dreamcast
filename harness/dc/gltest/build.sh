#!/usr/bin/env bash
# Builds harness/dc/gltest/gltest.elf in the SDK container. See gltest.c for why.
set -euo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
IMAGE="${TR_DC_TOOLCHAIN_IMAGE:-tuxracer-dc:sdk}"
docker run --rm --platform linux/arm64 -v "$REPO":/work "$IMAGE" bash -c '
  set -e
  cd /work/harness/dc/gltest
  kos-cc -std=gnu99 -O2 -g -c gltest.c -o /tmp/gltest.o -I$KOS_PORTS/include
  kos-cc -o /work/dc/build/gltest.elf /tmp/gltest.o -lGLU -lGL -lm
  sh-elf-size /work/dc/build/gltest.elf'
