#!/usr/bin/env bash
# Compiles the header-error resync wrapper for the host and runs its tests.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
CXX="${CXX:-c++}"

OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

"$CXX" -std=c++17 -Wall -Wextra -Werror -O1 -g \
    -I"$ROOT/src" -I"$ROOT/src/libs" \
    "$ROOT/src/libs/MakeraFrame.cpp" \
    "$ROOT/src/libs/FrameResync.cpp" \
    "$HERE/test_frame_resync.cpp" \
    -o "$OUT/test_frame_resync"

"$OUT/test_frame_resync"
