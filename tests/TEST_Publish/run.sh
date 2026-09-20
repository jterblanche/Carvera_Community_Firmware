#!/usr/bin/env bash
# Compiles the publish-decision pure functions for the host and runs their
# tests.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
CXX="${CXX:-c++}"

OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

"$CXX" -std=c++17 -Wall -Wextra -Werror -O1 -g \
    -I"$ROOT/src" -I"$ROOT/src/libs" \
    "$ROOT/src/libs/ClientTable.cpp" \
    "$ROOT/src/libs/Publish.cpp" \
    "$HERE/test_publish.cpp" \
    -o "$OUT/test_publish"

"$OUT/test_publish"
