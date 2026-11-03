#!/usr/bin/env bash
#
# mayhem/build.sh — build this repo's fuzz harness(es). EDIT per repo.
#
# Runs inside the commit image (mayhem/Dockerfile) as `mayhem` in /mayhem. The base image
# (ghcr.io/savantenvs/base) already exports the build contract — use these, don't redefine:
#   CC, CXX             stock clang / clang++
#   LIB_FUZZING_ENGINE  -fsanitize=fuzzer   (link into each harness that has a LLVMFuzzer entry)
#   SANITIZER_FLAGS     -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer
#                       (ASan + UBSan, both set to HALT — so Mayhem catches memory AND UB defects)
#   DEBUG_FLAGS         -g -gdwarf-3   (DWARF debug info — always on for fuzz/standalone builds,
#                       independent of the sanitizer off-switch; DWARF version must be < 4)
#   RUST_DEBUG_FLAGS    -C debuginfo=2 -C force-frame-pointers=yes -C llvm-args=-gdwarf-3
#                       (thread through RUSTFLAGS on every cargo-fuzz build)
#   GO_DEBUG_FLAGS      -gcflags=all=-N -l
#                       (thread through go build / go-fuzz-build so the linked ELF keeps symbols)
#   SRC                 /mayhem (the repo source)
#
# Contract: build EVERYTHING here — one runnable binary per fuzz harness, AND the project's test
# suite (so mayhem/test.sh only has to RUN it, never compile). Keep it ADDITIVE (build upstream as
# upstream documents; don't edit upstream files). IMPORTANT: build the PROJECT ITSELF with
# $SANITIZER_FLAGS and $DEBUG_FLAGS (not just the harness) so the fuzzed code is instrumented
# AND carries DWARF < 4 symbols — otherwise ASan/UBSan only see the harness, not the library
# you're trying to find bugs in, and backtraces won't resolve project source lines. Build the TEST
# suite with the project's NORMAL flags (a clean, independent build) so test.sh stays an honest
# functional oracle and won't false-fail on benign UB. Leave the test binary/runner where test.sh
# expects it.
set -euo pipefail

# clang rejects SOURCE_DATE_EPOCH='' (empty) — it must be unset or a valid integer.
[ -n "${SOURCE_DATE_EPOCH:-}" ] || unset SOURCE_DATE_EPOCH

# Build knobs come from the ENVIRONMENT (overridable), with sane defaults — no if-statements,
# just parameter-expansion fallbacks. Default sanitizers are the base's ASan+UBSan (halting); override
# per build via the Dockerfile's `--build-arg SANITIZER_FLAGS="..."`.
# NB: SANITIZER_FLAGS uses `=` (no colon) on purpose — `=` only fills when the var is UNSET, so an
# explicit EMPTY value (`--build-arg SANITIZER_FLAGS=`) is honored and builds with NO sanitizers
# (useful when you want the program's natural crash / full backtrace, not an ASan report). The other
# knobs use `:=` (default on empty too). MAYHEM_JOBS sets build parallelism (falls back to nproc).
: "${SANITIZER_FLAGS=-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer}"
# DEBUG_FLAGS carries DWARF debug info INDEPENDENTLY of the sanitizer off-switch (so an empty
# SANITIZER_FLAGS still yields DWARF symbols). DWARF MUST be < 4 (Mayhem triage can't read >=4); clang-19's
# plain `-g` emits DWARF-5, so `-gdwarf-3` is explicit. Apply $DEBUG_FLAGS to the fuzz/harness/standalone
# builds (NOT the test/oracle build). Rust/Go carry the same intent via their language flags below.
: "${DEBUG_FLAGS:=-g -gdwarf-3}"
: "${RUST_DEBUG_FLAGS:=-C debuginfo=2 -C force-frame-pointers=yes -C llvm-args=-gdwarf-3}"
: "${GO_DEBUG_FLAGS:=-gcflags=all=-N -l}"
: "${CC:=clang}" ; : "${CXX:=clang++}" ; : "${LIB_FUZZING_ENGINE:=-fsanitize=fuzzer}"
: "${MAYHEM_JOBS:=$(nproc)}"
# COVERAGE_FLAGS: empty by default → no effect on the normal oracle build. Set it via the Dockerfile's
# `--build-arg COVERAGE_FLAGS="-fprofile-instr-generate -fcoverage-mapping"` to instrument the TEST
# build for source-coverage measurement (how much of the project the test suite actually exercises —
# a quality signal for the oracle; complements the anti-reward-hack sabotage check). APPEND it to the
# test build's compile+link flags in step 3 (NOT the fuzz build); after `test.sh` runs, merge with
# `llvm-profdata` and report with `llvm-cov`. Empty value is honored (`=`, not `:=`).
: "${COVERAGE_FLAGS=}"
export SANITIZER_FLAGS DEBUG_FLAGS RUST_DEBUG_FLAGS GO_DEBUG_FLAGS CC CXX LIB_FUZZING_ENGINE MAYHEM_JOBS COVERAGE_FLAGS

cd "$SRC"

# bgfx is built with GENie (which clones sibling bx/bimg repos from the network). The air-gapped build
# can't do that, so bx and bimg are vendored under mayhem/deps/ at pinned commits and the handful of
# bgfx tool sources the harnesses exercise are compiled directly with clang.
BX="$SRC/mayhem/deps/bx"
BIMG="$SRC/mayhem/deps/bimg"
OBJ="$SRC/.build"
OUT="$SRC"
mkdir -p "$OBJ/san" "$OBJ/norm"

INC=(-I"$BX/include" -I"$BX/3rdparty" -I"$BIMG/include" -I"$BIMG/3rdparty" -I"$BIMG/3rdparty/tinyexr/deps"
     -I"$BIMG/3rdparty/astc-encoder/include" -I"$BIMG/3rdparty/libavif" -I"$BIMG/3rdparty/libavif/include"
     -I"$BIMG/3rdparty/libavif/third_party/libyuv/include" -I"$BIMG/3rdparty/dav1d" -I"$BIMG/3rdparty/dav1d/include"
     -I"$SRC/include" -I"$SRC/3rdparty" -I"$SRC/src")
DEFS=(-D__STDC_LIMIT_MACROS -D__STDC_FORMAT_MACROS -D__STDC_CONSTANT_MACROS -DASTCENC_F16C=0 -DASTCENC_NEON=0
      -DASTCENC_SSE=0 -DASTCENC_AVX=0 -DASTCENC_POPCNT=0 -DAVIF_CODEC_DAV1D)
CXXSTD=(-std=c++20 -fPIC -w)
CSTD=(-std=c11 -fPIC -w)
# bx's SSE paths use SSE4.1 intrinsics; upstream's GENie toolchain builds x86 with -msse4.2.
case "$(uname -m)" in x86_64|i?86) CXXSTD+=(-msse4.2); CSTD+=(-msse4.2) ;; esac

# Source lists (each entry: source path relative to $SRC), shared by the sanitized and normal builds.
BX_SRC=(mayhem/deps/bx/src/amalgamated.cpp)
BIMG_SRC=(mayhem/deps/bimg/src/image.cpp mayhem/deps/bimg/src/image_decode.cpp mayhem/deps/bimg/src/image_decode_wic.cpp)
for f in mayhem/deps/bimg/3rdparty/astc-encoder/source/*.cpp; do BIMG_SRC+=("$f"); done
BIMG_C=(mayhem/deps/bimg/3rdparty/dav1d/dav1d-amalgamated.c mayhem/deps/bimg/3rdparty/dav1d/dav1d-bitdepth-8.c
        mayhem/deps/bimg/3rdparty/dav1d/dav1d-bitdepth-16.c mayhem/deps/bimg/3rdparty/libavif/libavif-amalgamated.c)
TOOL_SRC=(tools/shaderc/pp.cpp src/vertexlayout.cpp)
for f in 3rdparty/meshoptimizer/src/*.cpp; do TOOL_SRC+=("$f"); done

# run_jobs: read "cmd..." lines from stdin, run MAYHEM_JOBS at a time, fail if any fails.
run_jobs() { xargs -P "$MAYHEM_JOBS" -I{} bash -c '{}'; }

# compile_set <dir> <extra flags...>: compile every library source into <dir>/<name>.o
compile_set() {
  local dir="$1"; shift
  local flags="$*"
  {
    for s in "${BX_SRC[@]}" "${BIMG_SRC[@]}" "${TOOL_SRC[@]}"; do
      echo "$CXX ${CXXSTD[*]} ${INC[*]} ${DEFS[*]} $flags -c $SRC/$s -o $dir/$(echo "$s" | tr / _).o"
    done
    for s in "${BIMG_C[@]}"; do
      echo "$CC ${CSTD[*]} ${INC[*]} ${DEFS[*]} $flags -c $SRC/$s -o $dir/$(echo "$s" | tr / _).o"
    done
  } | run_jobs
}

# ---- 1) sanitized + DWARF-3 libraries and the four fuzz harnesses ----
# -fsanitize=fuzzer-no-link puts libFuzzer's edge coverage into the LIBRARY code too; without it only
# the harness TU is instrumented and Mayhem sees ~no edges.
SAN="$SANITIZER_FLAGS $DEBUG_FLAGS -O1 -DBX_CONFIG_DEBUG=0 -fsanitize=fuzzer-no-link"
compile_set "$OBJ/san" $SAN

$CXX $SANITIZER_FLAGS $DEBUG_FLAGS -c "$SRC/mayhem/lsan_off.cc" -o "$OBJ/san/lsan_off.o"
$CC $SANITIZER_FLAGS $DEBUG_FLAGS -c "$STANDALONE_FUZZ_MAIN" -o "$OBJ/san/standalone_main.o"

LIBS_SAN=("$OBJ"/san/mayhem_deps_*.o "$OBJ"/san/tools_*.o "$OBJ"/san/src_*.o "$OBJ"/san/3rdparty_*.o)
build_harness() {
  local t="$1" ; shift
  echo "$CXX ${CXXSTD[*]} ${INC[*]} ${DEFS[*]} $SAN -c $SRC/mayhem/$t.cpp -o $OBJ/san/h_$t.o"
}
HARNESSES=(fuzz_pp fuzz_image_container fuzz_image_decode fuzz_geometryc)
for t in "${HARNESSES[@]}"; do build_harness "$t"; done | run_jobs

for t in "${HARNESSES[@]}"; do
  $CXX $SAN $LIB_FUZZING_ENGINE "$OBJ/san/h_$t.o" "${LIBS_SAN[@]}" "$OBJ/san/lsan_off.o" -lpthread -ldl -lm -o "$OUT/$t"
  $CXX $SAN "$OBJ/san/h_$t.o" "$OBJ/san/standalone_main.o" "${LIBS_SAN[@]}" "$OBJ/san/lsan_off.o" -lpthread -ldl -lm -o "$OUT/$t-standalone"
done

# ---- 2) the oracle: normal flags, bx's own Catch2 suite plus a known-answer selftest ----
NORM="-O1 -g -DBX_CONFIG_DEBUG=1 $COVERAGE_FLAGS"
{
  echo "$CXX ${CXXSTD[*]} ${INC[*]} ${DEFS[*]} $NORM -c $SRC/mayhem/deps/bx/src/amalgamated.cpp -o $OBJ/norm/bx.o"
  echo "$CXX ${CXXSTD[*]} ${INC[*]} ${DEFS[*]} $NORM -DCATCH_AMALGAMATED_CUSTOM_MAIN -c $BX/3rdparty/catch/catch_amalgamated.cpp -o $OBJ/norm/catch.o"
  for f in "$BX"/tests/*_test.cpp; do
    echo "$CXX ${CXXSTD[*]} ${INC[*]} ${DEFS[*]} $NORM -DCATCH_AMALGAMATED_CUSTOM_MAIN -c $f -o $OBJ/norm/t_$(basename "$f" .cpp).o"
  done
} | run_jobs
$CXX $NORM "$OBJ"/norm/bx.o "$OBJ"/norm/catch.o "$OBJ"/norm/t_*.o -lpthread -ldl -lm -o "$OUT/bx_tests"

# selftest links the same library sources, normal flags (no sanitizers)
compile_set "$OBJ/norm" -O1 -g -DBX_CONFIG_DEBUG=0 $COVERAGE_FLAGS
$CXX "${CXXSTD[@]}" "${INC[@]}" "${DEFS[@]}" -O1 -g -DBX_CONFIG_DEBUG=0 $COVERAGE_FLAGS \
  "$SRC/mayhem/selftest.cpp" "$OBJ"/norm/mayhem_deps_*.o "$OBJ"/norm/tools_*.o "$OBJ"/norm/src_*.o "$OBJ"/norm/3rdparty_*.o \
  -lpthread -ldl -lm -o "$OUT/bgfx_selftest"
