#!/usr/bin/env bash
# Phase 4 cross-day check: compare bench_match medians across native_run.sh result folders.
# usage: scripts/native_compare.sh [results-root=results]
# For every workload + variant seen in 2+ runs, prints the per-run medians and the spread
# (max-min)/min of those medians; spread above 5% is flagged. Smoke-test (_quick) runs are ignored.
set -euo pipefail
ROOT="${1:-results}"
shopt -s nullglob
files=()
for f in "$ROOT"/*/results.tsv; do
  case "$f" in *_quick/*) ;; *) files+=("$f") ;; esac
done
[ ${#files[@]} -gt 0 ] || { echo "no results.tsv under $ROOT" >&2; exit 1; }
echo "runs: ${#files[@]} (${files[*]%/results.tsv})"
echo
printf "%-16s %-14s %4s %8s %8s %8s  %s\n" workload variant runs min max spread "medians (M ev/s)"
cat "${files[@]}" | awk -F'\t' '
  { key = $3 "\t" $4; n[key]++; med[key] = med[key] " " $5
    if (!(key in lo) || $5 + 0 < lo[key]) lo[key] = $5 + 0
    if (!(key in hi) || $5 + 0 > hi[key]) hi[key] = $5 + 0 }
  END {
    for (k in n) {
      if (n[k] < 2) continue
      split(k, p, "\t"); s = 100 * (hi[k] - lo[k]) / lo[k]
      printf "%-16s %-14s %4d %8.2f %8.2f %7.1f%%%s %s\n", p[1], p[2], n[k], lo[k], hi[k], s, (s > 5 ? " !" : "  "), med[k]
    }
  }' | sort
echo
echo "'!' = spread above the Phase 4 target of ~5% across runs/days."
