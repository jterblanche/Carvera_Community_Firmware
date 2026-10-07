#!/usr/bin/env bash
# Compiles the file-command gate for the host and runs its tests.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
CXX="${CXX:-c++}"

OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

"$CXX" -std=c++17 -Wall -Wextra -Werror -O1 -g \
    -I"$ROOT/src" -I"$ROOT/src/libs" \
    "$ROOT/src/libs/FileCommandGate.cpp" \
    "$HERE/test_file_command_gate.cpp" \
    -o "$OUT/test_file_command_gate"

"$OUT/test_file_command_gate"
