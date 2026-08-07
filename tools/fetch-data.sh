#!/usr/bin/env bash
# =============================================================================
# fetch-data.sh — download and verify the Tux Racer 0.61 data package.
# =============================================================================
# The data (courses, textures, sounds, .it soundtrack, the .tcl scripts that
# drive all of it) is 12 MB unpacked and is NOT committed: it is unmodified
# upstream, it is binary, and git is the wrong place for it. This script is the
# reproducible substitute, and it verifies the sha256 before unpacking.
#
#   bash tools/fetch-data.sh            # -> ./data/
#   TR_DATA=./data bash dc/build-dc.sh  # burn it into the disc image
#
# The upstream source tarball is fetched too, but only its checksum is checked
# against what src/ was vendored from — src/ is already in the repo.
# =============================================================================
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${1:-$REPO/data}"
CACHE="$REPO/upstream/tarballs"

DATA_URL="https://downloads.sourceforge.net/project/tuxracer/tuxracer-data/0.61/tuxracer-data-0.61.tar.gz"
DATA_SHA="3783d204b7bb1ed16aa5e5a1d5944de10fbee05bc7cebb8f616fce84301f3651"

SRC_URL="https://downloads.sourceforge.net/project/tuxracer/tuxracer/0.61/tuxracer-0.61.tar.gz"
SRC_SHA="a311d09080598fe556134d4b9faed7dc0c2ed956ebb10d062e5d4df022f91eff"

sha256_of() {
    if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | cut -d' ' -f1
    else shasum -a 256 "$1" | cut -d' ' -f1; fi
}

fetch() {  # fetch <url> <sha256> <dest>
    local url="$1" want="$2" dest="$3"
    if [ -f "$dest" ] && [ "$(sha256_of "$dest")" = "$want" ]; then
        echo "-- cached: $(basename "$dest")"
        return 0
    fi
    echo "-- downloading $(basename "$dest")"
    curl -fsSL --retry 3 -o "$dest.part" "$url"
    local got; got="$(sha256_of "$dest.part")"
    if [ "$got" != "$want" ]; then
        rm -f "$dest.part"
        echo "ERROR: sha256 mismatch for $url" >&2
        echo "       want $want" >&2
        echo "       got  $got" >&2
        exit 1
    fi
    mv "$dest.part" "$dest"
}

mkdir -p "$CACHE"
fetch "$DATA_URL" "$DATA_SHA" "$CACHE/tuxracer-data-0.61.tar.gz"
fetch "$SRC_URL"  "$SRC_SHA"  "$CACHE/tuxracer-0.61.tar.gz"

# The tarball unpacks to tuxracer-data-0.61/. The disc wants its CONTENTS at
# the root (DATA_DIR is "/cd"), so strip the leading component.
rm -rf "$OUT"
mkdir -p "$OUT"
tar xzf "$CACHE/tuxracer-data-0.61.tar.gz" -C "$OUT" --strip-components=1

echo "-- data: $OUT ($(find "$OUT" -type f | wc -l | tr -d ' ') files, $(du -sh "$OUT" | cut -f1))"
echo "   next: TR_DATA=$OUT bash dc/build-dc.sh"
