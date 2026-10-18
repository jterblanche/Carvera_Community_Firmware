#!/usr/bin/env bash
# Compiles the USB link's activity tracking for the host and runs its tests.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
CXX="${CXX:-c++}"

OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

"$CXX" -std=c++17 -Wall -Wextra -Werror -O1 -g \
    -I"$ROOT/src" -I"$ROOT/src/libs" \
    "$ROOT/src/libs/ClientTable.cpp" \
    "$ROOT/src/libs/ControlToken.cpp" \
    "$ROOT/src/libs/Hello.cpp" \
    "$HERE/test_usb_activity.cpp" \
    -o "$OUT/test_usb_activity"

"$OUT/test_usb_activity"
