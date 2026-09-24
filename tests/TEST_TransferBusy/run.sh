#!/usr/bin/env bash
# Compiles the busy reply sent during a file transfer for the host and runs
# its tests.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
CXX="${CXX:-c++}"

OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

"$CXX" -std=c++17 -Wall -Wextra -Werror -O1 -g \
    -I"$ROOT/src" -I"$ROOT/src/libs" \
    "$ROOT/src/libs/TransferBusy.cpp" \
    "$ROOT/src/libs/MakeraFrame.cpp" \
    "$HERE/test_transfer_busy.cpp" \
    -o "$OUT/test_transfer_busy"

"$OUT/test_transfer_busy"
