#!/usr/bin/env bash
# Compiles player_is_playing() with the real PublicData::get_value() for the
# host and runs its tests.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
CXX="${CXX:-c++}"

OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

# stub/ comes first so that "libs/Kernel.h" resolves to the host stand-in.
"$CXX" -std=c++17 -Wall -Wextra -Werror -O1 -g \
    -I"$HERE/stub" -I"$ROOT/src" -I"$ROOT/src/libs" \
    "$ROOT/src/libs/PublicData.cpp" \
    "$HERE/test_player_is_playing.cpp" \
    -o "$OUT/test_player_is_playing"

"$OUT/test_player_is_playing"
