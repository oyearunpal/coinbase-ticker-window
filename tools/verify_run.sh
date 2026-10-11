#!/usr/bin/env bash
# Verifies one ticker_feed run from its output directory: checks run.log's
# summary counters, checks the CSV row count against what the writer logged,
# then re-derives every CSV row with verify_csv.py and prints its accuracy summary.
#
# Usage: tools/verify_run.sh [run_dir]    (default: logs/latest, the newest run)
set -euo pipefail

RUN_DIR="${1:-logs/latest}"
LOG="$RUN_DIR/run.log"
TOOLS_DIR="$(dirname "$0")"

if [ ! -f "$LOG" ]; then
  echo "error: $LOG not found" >&2
  exit 1
fi

# Value of the first "key=value" line in run.log, or empty if absent.
field() { sed -n "s/^$1=//p" "$LOG" | head -n 1; }

status=0
fail() { echo "FAIL: $*"; status=1; }

window_ns="$(field window.length_ns)"
policy="$(field window.policy)"
symbols="$(field feed.symbols)"
echo "run: $RUN_DIR  policy=$policy  window=$((window_ns / 1000000000))s  symbols=$symbols"

if ! grep -q "^=== summary ===" "$LOG"; then
  fail "run.log has no summary (the run did not shut down cleanly)"
else
  echo "elapsed_ms=$(field elapsed_ms) non_ticker_frames=$(field non_ticker_frames)" \
       "ring_full_spin_events=$(field ring_full_spin_events)"
  [ "$(field parse_failures)" = 0 ] || fail "parse_failures=$(field parse_failures)"
  [ "$(field unknown_symbol)" = 0 ] || fail "unknown_symbol=$(field unknown_symbol)"
  active="$(field active_symbols)"
  [ "${active%/*}" = "${active#*/}" ] || fail "active_symbols=$active"
fi

if grep -q "^feed error frame:" "$LOG"; then
  grep "^feed error frame:" "$LOG"
  fail "the feed sent error frames"
fi

shopt -s nullglob
csvs=("$RUN_DIR"/stats_*.csv)
if [ "${#csvs[@]}" -eq 0 ]; then
  fail "no stats_*.csv in $RUN_DIR"
else
  written="$(sed -n 's/^writer: \([0-9]*\) rows written$/\1/p' "$LOG")"
  csv_rows=0
  for f in "${csvs[@]}"; do
    csv_rows=$((csv_rows + $(wc -l < "$f") - 1))  # minus the header
  done
  if [ "$written" = "$csv_rows" ]; then
    echo "rows: $csv_rows in ${#csvs[@]} CSVs, as the writer logged"
  else
    fail "the writer logged ${written:-no} rows, the CSVs hold $csv_rows"
  fi

  echo
  python3 "$TOOLS_DIR/verify_csv.py" "${csvs[@]}" --window "$((window_ns / 1000000000))" || status=1
fi

exit "$status"
