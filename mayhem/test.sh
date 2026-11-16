#!/usr/bin/env bash
#
# mayhem/test.sh — runs ejdb's own test suites (ejdb_test1..6, jql_test1, jbr_test1; built by mayhem/build.sh
# with the project's normal flags, no sanitizer) plus a known-answer probe (mayhem/kat.c) whose
# output is compared with mayhem/kat.expected. Counts are reported as CTRF; exit 0 iff nothing failed.
set -uo pipefail
[ -n "${SOURCE_DATE_EPOCH:-}" ] || unset SOURCE_DATE_EPOCH
: "${MAYHEM_JOBS:=$(nproc)}"   # build parallelism; env-overridable, falls back to nproc (use -j"$MAYHEM_JOBS")
cd "$SRC"

# emit_ctrf <tool> <passed> <failed> [skipped] [pending] [other]
# Writes a CTRF report (file + stdout `CTRF {...}` marker) and returns non-zero iff failed>0.
emit_ctrf() {
  local tool="$1" passed="$2" failed="$3" skipped="${4:-0}" pending="${5:-0}" other="${6:-0}"
  local tests=$(( passed + failed + skipped + pending + other ))
  cat > "${CTRF_REPORT:-$SRC/ctrf-report.json}" <<JSON
{
  "results": {
    "tool": { "name": "$tool" },
    "summary": {
      "tests": $tests,
      "passed": $passed,
      "failed": $failed,
      "pending": $pending,
      "skipped": $skipped,
      "other": $other
    }
  }
}
JSON
  printf 'CTRF {"results":{"tool":{"name":"%s"},"summary":{"tests":%d,"passed":%d,"failed":%d,"pending":%d,"skipped":%d,"other":%d}}}\n' \
    "$tool" "$tests" "$passed" "$failed" "$pending" "$skipped" "$other"
  [ "$failed" -eq 0 ]
}


TEST_CACHE="$SRC/autark-cache-test"
passed=0
failed=0
skipped=0

# run_suite <name> <cwd> <binary>: the test framework prints
#   "  P passed[, F failed][, S skipped], A assertion(s), T.TTs  PASSED|FAILED"
run_suite() {
  local name="$1" dir="$2" bin="$3" out
  if [ ! -x "$bin" ]; then
    echo "missing test binary $bin (mayhem/build.sh should have built it)" >&2
    failed=$((failed + 1))
    return
  fi
  out="$(cd "$dir" && timeout 900 "$bin" 2>&1)"
  local rc=$?
  printf '%s\n' "$out" | tail -n 25
  local re='^[[:space:]]+([0-9]+) passed(, ([0-9]+) failed)?(, ([0-9]+) skipped)?, [0-9]+ assertion'
  local line p=0 f=0 s=0 found=0
  while IFS= read -r line; do
    if [[ $line =~ $re ]]; then
      p=${BASH_REMATCH[1]}
      f=${BASH_REMATCH[3]:-0}
      s=${BASH_REMATCH[5]:-0}
      found=1
    fi
  done <<< "$out"
  if [ "$found" -eq 0 ] || [ "$p" -eq 0 ]; then
    echo "$name: no passing summary (rc=$rc)" >&2
    f=$((f + 1))
  elif [ "$rc" -ne 0 ] && [ "$f" -eq 0 ]; then
    echo "$name: exit status $rc despite the summary" >&2
    f=$((f + 1))
  fi
  passed=$((passed + p))
  failed=$((failed + f))
  skipped=$((skipped + s))
}

WORK="$(mktemp -d "${TMPDIR:-/tmp}/ejdb-test.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

for n in 1 2 3 4 5 6; do
  mkdir -p "$WORK/t$n"
  run_suite "ejdb_test$n" "$WORK/t$n" "$TEST_CACHE/src/tests/ejdb_test$n"
done
# jql_test1 reads its fixtures from ./data, which the build copies next to the binary.
run_suite jql_test1 "$TEST_CACHE/src/jql/tests" "$TEST_CACHE/src/jql/tests/jql_test1"
# jbr_test1 starts the HTTP server on 127.0.0.1 and talks to it through libcurl (loopback only).
mkdir -p "$WORK/jbr"
run_suite jbr_test1 "$WORK/jbr" "$TEST_CACHE/src/jbr/tests/jbr_test1"

# Known-answer probe: fixed documents, queries and patches, output compared with the golden text.
KAT="$TEST_CACHE/mayhem-kat"
if [ ! -x "$KAT" ]; then
  echo "missing $KAT (mayhem/build.sh should have built it)" >&2
  failed=$((failed + 1))
else
  mkdir -p "$WORK/kat"
  if timeout 120 "$KAT" "$WORK/kat/db" 2>/dev/null > "$WORK/kat.out" \
     && diff -u "$SRC/mayhem/kat.expected" "$WORK/kat.out"; then
    echo "known-answer probe: output matches mayhem/kat.expected"
    passed=$((passed + 1))
  else
    echo "known-answer probe: output differs from mayhem/kat.expected" >&2
    failed=$((failed + 1))
  fi
fi

emit_ctrf "ejdb" "$passed" "$failed" "$skipped"
