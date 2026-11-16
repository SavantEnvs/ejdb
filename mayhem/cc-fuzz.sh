#!/bin/sh
# Compiler driver handed to Autark as CC for the instrumented build. Autark runs $CC as a single
# executable, so the sanitizer, SanCov and DWARF-3 flags ride in here; clang itself is still
# resolved by name through PATH.
# The peg/leg parser generators (built from extra/ by `make` in .../extern_peg) are build tools,
# not fuzzed code: they stay uninstrumented.
# Autark adds -DNDEBUG -DIW_RELEASE to every non-Debug build (Autark, iowow, iwnet, binn-c);
# the trailing -UNDEBUG -UIW_RELEASE keep the projects' assert()s live in the fuzzed code.
case "$PWD" in
  */extern_peg*) exec clang "$@" ;;
esac
exec clang $SANITIZER_FLAGS -fsanitize=fuzzer-no-link $DEBUG_FLAGS "$@" -UNDEBUG -UIW_RELEASE
