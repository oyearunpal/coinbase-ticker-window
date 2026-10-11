#!/usr/bin/env bash
# Runs the published benchmark for {3-symbol, all-symbol capture} x {300s, 3600s}:
#   feed_total + memory: ticker_bench --mode=feed, once per policy          -> logs/bench/results.csv
#   stage timings:       ticker_bench --mode=stages, once with the default policy -> logs/bench/results.csv
#   accuracy:            compare_policies (all policies vs exact_map) -> logs/bench/accuracy_<capture>_<window>.txt
#
# Usage: bench/run_benchmarks.sh [--dry-run]
# Env:   THREE_SYM_CAPTURE, ALL_SYM_CAPTURE (captures recorded with ticker_feed's [capture] record),
#        PASSES (default 3), BUILD_DIR (default build), OUT_DIR (default logs/bench)
set -euo pipefail

cd "$(dirname "$0")/.."

DRY_RUN=0
[ "${1:-}" = "--dry-run" ] && DRY_RUN=1

BUILD_DIR="${BUILD_DIR:-build}"
OUT_DIR="${OUT_DIR:-logs/bench}"
PASSES="${PASSES:-3}"
THREE_SYM_CAPTURE="${THREE_SYM_CAPTURE:-captures/three_symbols_3hr.bin}"
ALL_SYM_CAPTURE="${ALL_SYM_CAPTURE:-captures/heavy_all_symbols_3hr.bin}"
POLICIES="exact_map sorted_vector sorted_vector_freq tracked_histogram range_histogram range_histogram_no_rebuild hybrid_histogram"
WINDOWS="300s 3600s"

run() {
  echo "+ $*"
  [ "$DRY_RUN" = 1 ] || "$@"
}

if [ "$DRY_RUN" = 0 ]; then
  mkdir -p "$OUT_DIR"
  rm -f "$OUT_DIR/results.csv"
fi

for capture in "$THREE_SYM_CAPTURE" "$ALL_SYM_CAPTURE"; do
  if [ ! -f "$capture" ] && [ "$DRY_RUN" = 0 ]; then
    echo "skip: $capture not found" >&2
    continue
  fi
  name="$(basename "$capture" .bin)"
  for window in $WINDOWS; do
    for policy in $POLICIES; do
      run "$BUILD_DIR/ticker_bench" --replay="$capture" --window="$window" --policy="$policy" \
          --mode=feed --passes="$PASSES" --out="$OUT_DIR/results.csv"
    done
    run "$BUILD_DIR/ticker_bench" --replay="$capture" --window="$window" --mode=stages \
        --passes="$PASSES" --out="$OUT_DIR/results.csv"
    echo "+ $BUILD_DIR/compare_policies $capture ${window%s} > $OUT_DIR/accuracy_${name}_${window}.txt"
    if [ "$DRY_RUN" = 0 ]; then
      "$BUILD_DIR/compare_policies" "$capture" "${window%s}" > "$OUT_DIR/accuracy_${name}_${window}.txt"
    fi
  done
done
