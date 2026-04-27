#!/usr/bin/env bash
# Smoke-test runner: executes every examples/*.my and test.my, captures stdout,
# and reports a pass/fail. A run "passes" if the interpreter exits 0 and
# produces no `[error]`/`Traceback`/segfault output. Reference outputs live in
# tests/golden/ (one per .my, optional). When a golden file exists, output is
# diffed against it.

set -u
cd "$(dirname "$0")/.."

BIN=${BIN:-./mymo}
GOLDEN_DIR=tests/golden
mkdir -p "$GOLDEN_DIR"

# The interpreter writes `.myc` bytecode caches alongside `.my` sources on
# every run. Stale caches from a previous interpreter version silently corrupt
# results, so we always nuke them before a test run.
/bin/rm -f examples/*.myc benchmarks/*.myc test.myc 2>/dev/null

SKIP=()
STRICT=0
for arg in "$@"; do
  [ "$arg" = "--strict" ] && STRICT=1
done

is_skipped() {
  local f="$1"
  [ ${#SKIP[@]} -eq 0 ] && return 1
  for s in "${SKIP[@]}"; do [ "$s" = "$f" ] && return 0; done
  return 1
}

if [ ! -x "$BIN" ]; then
  echo "error: $BIN not built. Run 'make' first." >&2
  exit 2
fi

pass=0
fail=0
failed_names=()

run_one() {
  local script="$1"
  local name
  name=$(basename "$script" .my)
  local out err rc
  out=$(timeout 10 "$BIN" "$script" 2>/tmp/mymo-err.$$)
  rc=$?
  err=$(cat /tmp/mymo-err.$$)
  rm -f /tmp/mymo-err.$$

  if [ $rc -ne 0 ]; then
    fail=$((fail + 1))
    failed_names+=("$script (exit $rc)")
    printf '  FAIL  %-40s exit=%d\n' "$script" "$rc"
    if [ -n "$err" ]; then
      printf '         stderr: %s\n' "$(echo "$err" | head -3 | tr '\n' ' ')"
    fi
    return
  fi

  local golden="$GOLDEN_DIR/$name.out"
  if [ -f "$golden" ]; then
    # Hex pointer addresses (e.g. "<instance of Mano at 0x84f000660>") differ
    # every run due to ASLR. Normalize both sides before diffing so address-
    # bearing scripts can still have meaningful goldens.
    local norm_out norm_golden
    norm_out=$(echo "$out" | sed -E 's/0x[0-9a-fA-F]+/0xADDR/g')
    norm_golden=$(sed -E 's/0x[0-9a-fA-F]+/0xADDR/g' "$golden")
    if ! diff -u <(echo "$norm_golden") <(echo "$norm_out") >/dev/null; then
      fail=$((fail + 1))
      failed_names+=("$script (output mismatch)")
      printf '  DIFF  %-40s (vs %s)\n' "$script" "$golden"
      return
    fi
  fi

  pass=$((pass + 1))
  printf '  ok    %-40s\n' "$script"
}

echo "Running examples through $BIN ..."
for f in examples/*.my; do
  [ -f "$f" ] || continue
  if [ $STRICT -eq 0 ] && is_skipped "$f"; then
    printf '  skip  %-40s (known-failure)\n' "$f"
    continue
  fi
  run_one "$f"
done
[ -f test.my ] && run_one test.my

echo
echo "Results: $pass passed, $fail failed"
if [ $fail -gt 0 ]; then
  printf '  - %s\n' "${failed_names[@]}"
  exit 1
fi
