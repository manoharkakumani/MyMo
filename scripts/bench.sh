#!/usr/bin/env bash
# Benchmark harness. Runs every script under benchmarks/*.my (3 iterations,
# best of) and prints a one-line-per-bench summary. Used to track perf across
# the redesign phases.

set -u
cd "$(dirname "$0")/.."

BIN=${BIN:-./mymo}
ITERS=${ITERS:-3}

if [ ! -x "$BIN" ]; then
  echo "error: $BIN not built. Run 'make' first." >&2
  exit 2
fi

mkdir -p benchmarks
# If no benchmarks yet, fall back to the cellular-automaton test.my as the
# default workload — it's a tight loop heavy in list ops and arithmetic.
shopt -s nullglob
files=(benchmarks/*.my)
if [ ${#files[@]} -eq 0 ]; then
  files=(test.my)
fi

printf '%-40s %12s\n' 'benchmark' 'best (s)'
printf '%-40s %12s\n' '----------------------------------------' '------------'
for f in "${files[@]}"; do
  best=""
  for i in $(seq 1 "$ITERS"); do
    # Use bash's built-in TIMEFORMAT for reliable timing across macOS/Linux.
    TIMEFORMAT='%R'
    t=$( { time "$BIN" "$f" >/dev/null 2>&1; } 2>&1 )
    if [ -z "$best" ] || awk -v a="$t" -v b="$best" 'BEGIN{exit !(a<b)}'; then
      best="$t"
    fi
  done
  printf '%-40s %12s\n' "$f" "$best"
done
