#!/usr/bin/env bash
# Compiles the control-token gate for the host and runs its tests.
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
    "$HERE/test_control_token.cpp" \
    -o "$OUT/test_control_token"

"$OUT/test_control_token"
