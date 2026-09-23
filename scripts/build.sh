#!/usr/bin/env bash
# usage: scripts/build.sh [release|debug|asan|tsan]   (default: release)
set -euo pipefail
cd "$(dirname "$0")/.."
P="${1:-release}"
cmake --preset "$P"
cmake --build "build/$P" -j"$(nproc)"
