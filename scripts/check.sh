#!/usr/bin/env bash
# The "definition of done" gate. Run before every commit: tests under ASan/UBSan + release tests.
# Verified working on Ubuntu 24.04, checked ASan/UBSan(Memory Safety) and release builds, and all tests pass.

# also checked the replay fra
set -euo pipefail
cd "$(dirname "$0")/.."
scripts/build.sh asan    && ctest --test-dir build/asan    --output-on-failure
scripts/build.sh release && ctest --test-dir build/release --output-on-failure
echo "CHECK PASSED"
