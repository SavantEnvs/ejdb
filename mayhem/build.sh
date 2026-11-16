#!/usr/bin/env bash
#
# mayhem/build.sh — builds ejdb twice with its own Autark build system, and the fuzz harnesses:
#   1. autark-cache-fuzz/  ejdb + iwnet + iowow + binn-c instrumented (ASan/UBSan + SanCov, DWARF 3);
#      the harnesses and their -standalone reproducers link against it.
#   2. autark-cache-test/  ejdb's normal build with its test suite, for mayhem/test.sh.
# Both builds include the HTTP/WebSocket server (src/jbr, on iwnet), upstream's default.
# Offline: iwnet, iowow and binn-c come from the in-image cache /opt/toolchains/deps (see Dockerfile).
set -euo pipefail

[ -n "${SOURCE_DATE_EPOCH:-}" ] || unset SOURCE_DATE_EPOCH

: "${SANITIZER_FLAGS=-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer}"
: "${DEBUG_FLAGS:=-g -gdwarf-3}"
: "${CC:=clang}" ; : "${CXX:=clang++}" ; : "${LIB_FUZZING_ENGINE:=-fsanitize=fuzzer}"
: "${MAYHEM_JOBS:=$(nproc)}"
: "${COVERAGE_FLAGS=}"
export SANITIZER_FLAGS DEBUG_FLAGS CC CXX LIB_FUZZING_ENGINE MAYHEM_JOBS COVERAGE_FLAGS

cd "$SRC"

IOWOW_DIR=/opt/toolchains/deps/iowow-1.7.3/src
BINNC_DIR=/opt/toolchains/deps/binn-c-0.2.0/src
IWNET_DIR=/opt/toolchains/deps/iwnet-1.6.2/src
[ -d "$IOWOW_DIR" ] && [ -d "$BINNC_DIR" ] && [ -d "$IWNET_DIR" ] || { echo "missing pre-fetched dependencies under /opt/toolchains/deps" >&2; exit 1; }
export IOWOW_URL="dir://$IOWOW_DIR" BINNC_URL="dir://$BINNC_DIR" IWNET_URL="dir://$IWNET_DIR"

FUZZ_CACHE="$SRC/autark-cache-fuzz"
TEST_CACHE="$SRC/autark-cache-test"
AUTARK_OPTS=("-J$MAYHEM_JOBS")

# Autark runs $CC as one executable, so the instrumented build goes through mayhem/cc-fuzz.sh,
# which adds $SANITIZER_FLAGS, -fsanitize=fuzzer-no-link (SanCov on the library) and $DEBUG_FLAGS.
# Bootstrap the Autark binary itself with the plain compiler first.
for cache in "$FUZZ_CACHE" "$TEST_CACHE"; do
  CC=clang ./build.sh -H "$cache" --version >/dev/null
done

# 1) Instrumented build.
CC="$SRC/mayhem/cc-fuzz.sh" ./build.sh -H "$FUZZ_CACHE" "${AUTARK_OPTS[@]}"

# 3) Oracle build: the project's NORMAL flags (plus optional coverage), with the test suite.
CFLAGS_TARGET="$COVERAGE_FLAGS" LDFLAGS_TARGET="$COVERAGE_FLAGS" \
  CC=clang ./build.sh -H "$TEST_CACHE" "${AUTARK_OPTS[@]}" -DEJDB_BUILD_TESTS=1

libdir() { dirname "$(find "$1" -name libiowow.a -path '*/lib/*' | head -n1)"; }
libs() { local d; d="$(libdir "$1")"; echo "$1/src/libejdb2.a $d/libiwnet.a $d/libiowow.a $d/libbinnc.a"; }
read -ra FUZZ_LIBS <<< "$(libs "$FUZZ_CACHE")"
read -ra TEST_LIBS <<< "$(libs "$TEST_CACHE")"
for l in "${FUZZ_LIBS[@]}" "${TEST_LIBS[@]}"; do [ -f "$l" ] || { echo "missing library $l" >&2; exit 1; }; done

incs() { echo "-I$1/src -I$SRC/src -I$SRC/src/jql -I$1/include -I$1/include/iowow"; }
DEFS="-std=gnu11 -fsigned-char -D_XOPEN_SOURCE=700 -D_DEFAULT_SOURCE -D_LARGEFILE_SOURCE -D_FILE_OFFSET_BITS=64 -DIW_64 -DIW_NODLL"

# 2) Fuzz harnesses, plus a standalone (non-libFuzzer) reproducer for each.
$CXX $SANITIZER_FLAGS $DEBUG_FLAGS -c "$SRC/mayhem/lsan_off.cc" -o "$FUZZ_CACHE/lsan_off.o"
$CC $SANITIZER_FLAGS $DEBUG_FLAGS -c -x c "$STANDALONE_FUZZ_MAIN" -o "$FUZZ_CACHE/standalone_main.o"

for t in fuzz_jql_parse fuzz_json fuzz_json_patch fuzz_ejdb_query fuzz_jbr; do
  src="$SRC/mayhem/$t/$t.c"
  $CC $SANITIZER_FLAGS $DEBUG_FLAGS $DEFS $(incs "$FUZZ_CACHE") $LIB_FUZZING_ENGINE \
      "$src" "$FUZZ_CACHE/lsan_off.o" "${FUZZ_LIBS[@]}" -lm -lpthread -o "/mayhem/$t"
  $CC $SANITIZER_FLAGS $DEBUG_FLAGS $DEFS $(incs "$FUZZ_CACHE") \
      "$src" "$FUZZ_CACHE/standalone_main.o" "$FUZZ_CACHE/lsan_off.o" "${FUZZ_LIBS[@]}" -lm -lpthread \
      -o "/mayhem/$t-standalone"
done

# Known-answer probe for mayhem/test.sh, linked against the NORMAL build.
$CC -O1 $DEFS $(incs "$TEST_CACHE") $COVERAGE_FLAGS "$SRC/mayhem/kat.c" "${TEST_LIBS[@]}" -lm -lpthread \
    -o "$TEST_CACHE/mayhem-kat"

for t in fuzz_jql_parse fuzz_json fuzz_json_patch fuzz_ejdb_query fuzz_jbr; do
  [ -x "/mayhem/$t" ] && [ -x "/mayhem/$t-standalone" ] || { echo "missing $t" >&2; exit 1; }
done
for t in ejdb_test1 ejdb_test2 ejdb_test3 ejdb_test4 ejdb_test5 ejdb_test6; do
  [ -x "$TEST_CACHE/src/tests/$t" ] || { echo "missing test binary $t" >&2; exit 1; }
done
[ -x "$TEST_CACHE/src/jql/tests/jql_test1" ] && [ -x "$TEST_CACHE/src/jbr/tests/jbr_test1" ] \
  && [ -x "$TEST_CACHE/mayhem-kat" ]
