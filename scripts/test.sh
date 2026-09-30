#!/usr/bin/env bash
# Build the module NATIVELY and run the contract test against it.
#
# This is the fast feedback loop the project did not have. The old
# test_setBfree.c was compiled for x86-64 while dsp.so was aarch64, so its
# dlopen could never succeed — it "passed" by never running anything, and
# every real defect had to be found by scp'ing to the device and reading
# debug.log.
#
# The native build is not shippable. It exists so that logic errors — a silent
# organ, a dropped MIDI message, a get_param that returns the wrong sentinel,
# an allocation on the audio path — fail here in a second.
#
# Requires: gcc, libc6-dev  (see Dockerfile)
# Usage:    ./scripts/test.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
cd "$REPO_ROOT"

OUT="$REPO_ROOT/build-host"

command -v gcc >/dev/null \
    || { echo "FAIL: native gcc not found. Install gcc and libc6-dev, or run this inside the build image (see Dockerfile)."; exit 1; }

# ---------------------------------------------------------------------
# Header drift check.
#
# b5 carries its own copy of plugin_api_v1.h. breakbeat's copy drifting from
# the host's is what boot-looped a device: it declared one extra callback, so
# a guarded call read 8 bytes past the struct and jumped into the heap. The
# header even carries a _Static_assert about it.
#
# Line endings are normalised before comparing. Both repos store the file as
# LF, but a Windows checkout has it as CRLF in the working tree; diffing those
# byte-for-byte reports every line as drifted and says nothing about the ABI.
#
# STRICTNESS. Drift is always fatal. A MISSING schwung checkout is fatal only
# when SCHWUNG_ROOT was set explicitly — i.e. someone asked for the check and
# it could not run. Without it the check warns and continues, because anyone
# who clones this repo on its own has no schwung tree and must still be able
# to build and test. Set SCHWUNG_ROOT in your own environment to get the old
# mandatory behaviour back.
# ---------------------------------------------------------------------
# SCHWUNG_ROOT, when set, is AUTHORITATIVE: it is not merely tried first. An
# earlier version listed it as one candidate among several, so a typo'd path
# fell through to ../schwung and reported a pass against a tree the caller
# had not asked for.
if [ -n "${SCHWUNG_ROOT:-}" ]; then
    CANDIDATES=("$SCHWUNG_ROOT")
else
    CANDIDATES=("$REPO_ROOT/../schwung" "/workspaces/schwung")
fi

DRIFT_CHECKED=0
for CAND in "${CANDIDATES[@]}"; do
    if [ -f "$CAND/src/host/plugin_api_v1.h" ]; then
        if diff -q <(tr -d '\r' < "$REPO_ROOT/src/host/plugin_api_v1.h") \
                   <(tr -d '\r' < "$CAND/src/host/plugin_api_v1.h") >/dev/null; then
            echo "ok   plugin_api_v1.h matches $CAND"
        else
            echo "FAIL plugin_api_v1.h has DRIFTED from $CAND"
            diff <(tr -d '\r' < "$REPO_ROOT/src/host/plugin_api_v1.h") \
                 <(tr -d '\r' < "$CAND/src/host/plugin_api_v1.h") | head -40
            exit 1
        fi
        DRIFT_CHECKED=1
        break
    fi
done

if [ "$DRIFT_CHECKED" -eq 0 ]; then
    if [ -n "${SCHWUNG_ROOT:-}" ]; then
        echo "FAIL SCHWUNG_ROOT=$SCHWUNG_ROOT has no src/host/plugin_api_v1.h."
        echo "     The drift check was requested and could not run. A stale copy of"
        echo "     that header is what boot-looped a device via breakbeat, so this"
        echo "     is an error rather than a skip. Fix the path or unset SCHWUNG_ROOT."
        exit 1
    fi
    echo "warn no schwung checkout found — plugin_api_v1.h drift check SKIPPED."
    echo "     Set SCHWUNG_ROOT=/path/to/schwung to enable it."
fi

# ---------------------------------------------------------------------
# Native module build. Debug on purpose: it leaves setBfree's asserts live
# (e.g. tonegen.c's assert(setting < 9)) so a range bug aborts here instead of
# being clamped away by the -DNDEBUG release build and misbehaving on device.
# ---------------------------------------------------------------------
rm -rf "$OUT"
mkdir -p "$OUT"
(
    cd "$OUT"
    # `env -u` is not belt-and-braces, it is the actual fix.
    #
    # -DCMAKE_C_COMPILER=gcc is NOT sufficient on its own: if
    # CMAKE_TOOLCHAIN_FILE is set in the environment, CMake reads the toolchain
    # file and its set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc) overrides the
    # command line. The build then succeeds, produces an aarch64 dsp.so, and
    # the x86-64 harness cannot dlopen it — which is exactly how the old
    # test_setBfree.c managed to "pass" without running anything.
    #
    # Configure output is NOT suppressed. "which compiler, which flags, which
    # include paths" is most of the diagnosis when a build fails in a container
    # you are not sitting in.
    env -u CC -u CXX -u CMAKE_TOOLCHAIN_FILE -u CFLAGS -u LDFLAGS \
        cmake -DCMAKE_C_COMPILER=gcc \
              -DCMAKE_BUILD_TYPE=Debug \
              "$REPO_ROOT"
    env -u CC -u CXX -u CMAKE_TOOLCHAIN_FILE -u CFLAGS -u LDFLAGS \
        make -j"$(nproc)" ${MAKE_VERBOSE:+VERBOSE=1}
)

echo ""
echo "Built native module: $OUT/modules/dsp.so"
file "$OUT/modules/dsp.so" | sed 's/^/     /'
echo ""

# Assert it really is native. Everything below dlopen()s this .so; a silent
# cross build here turns the whole contract test into a no-op that reports
# success, which is worse than no test at all.
case "$(uname -m)" in
    x86_64)  WANT="x86-64" ;;
    aarch64) WANT="ARM aarch64" ;;
    armv7l)  WANT="ARM" ;;
    *)       WANT="" ;;
esac
if [ -n "$WANT" ] && ! file "$OUT/modules/dsp.so" | grep -q "$WANT"; then
    echo "FAIL: build-host/modules/dsp.so is not native ($(uname -m), expected '$WANT')."
    echo "      Something forced a cross compiler — check CC / CMAKE_TOOLCHAIN_FILE:"
    echo "        CC=${CC:-<unset>}"
    echo "        CMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE:-<unset>}"
    echo "      The harness below is built with plain gcc and could not dlopen it."
    exit 1
fi

# ---------------------------------------------------------------------
# Harness + realtime guard
# ---------------------------------------------------------------------
gcc -shared -fPIC -O1 -g -o "$OUT/rt_guard.so" tests/rt_guard.c -ldl
gcc -O1 -g -Wall -I src -o "$OUT/host_test" tests/host_test.c -ldl -lm

echo "=== running contract test ==="
echo ""
LD_PRELOAD="$OUT/rt_guard.so" "$OUT/host_test" "$OUT/modules/dsp.so"
