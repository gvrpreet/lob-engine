#!/usr/bin/env bash
# The "definition of done" gate. Run before every commit: tests under ASan/UBSan + release tests.
set -euo pipefail
cd "$(dirname "$0")/.."
scripts/build.sh asan    && ctest --test-dir build/asan    --output-on-failure
scripts/build.sh release && ctest --test-dir build/release --output-on-failure
echo "CHECK PASSED"
