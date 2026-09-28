#!/usr/bin/env bash
# usage: scripts/tidy.sh   Runs clang-tidy (checks in .clang-tidy) over every library, tool and
# benchmark source using the debug build's compile_commands.json. Exit status is non-zero on
# any finding (WarningsAsErrors), so CI fails on new issues.
set -euo pipefail
cd "$(dirname "$0")/.."
cmake --preset debug > /dev/null
files=$(ls engine/src/*.cpp replay/src/*.cpp apps/*.cpp bench/*.cpp)
status=0
for f in $files; do
  clang-tidy -p build/debug --quiet "$f" 2>/dev/null || status=1
done
[ "$status" = 0 ] && echo "clang-tidy: clean ($(echo $files | wc -w) files)"
exit $status
