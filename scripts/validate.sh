#!/usr/bin/env bash
# Structural validation of the built module. Checks the binary WITHOUT loading
# it, so it works on the cross-compiled aarch64 artifact from an x86-64 host.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
MODULE_PATH="${1:-$REPO_ROOT/build/modules/dsp.so}"

# The chain host resolves the shared object by module.json's "dsp" field. If
# these two ever disagree the host reports only "dlopen failed", on the audio
# thread, with no indication that the name is the problem.
MODULE_JSON="$REPO_ROOT/src/schwung_bfree/module.json"

fail() { echo "FAIL: $*"; exit 1; }

# Debian splits the Python stdlib: python3.11-minimal provides the interpreter,
# libpython3.11-stdlib provides json and ast. `command -v python3` succeeding
# therefore says nothing about whether the checkers below can run, and the
# failure surfaces as "No module named 'json'", which looks like a bug in the
# checker. Probe once, report once.
PY=""
PY_WHY="python3 not installed"
if command -v python3 >/dev/null 2>&1; then
    if python3 -c "import json, ast" >/dev/null 2>&1; then
        PY=python3
    else
        PY_WHY="python3 stdlib incomplete — apt-get install libpython3-stdlib"
    fi
fi

echo "=== setBfree module validation ==="
echo "Target: $MODULE_PATH"
echo ""

[ -f "$MODULE_PATH" ] || fail "module not found at $MODULE_PATH (run scripts/build.sh)"
echo "ok   module exists"

file "$MODULE_PATH" | grep -q "ELF" || fail "not an ELF binary"
echo "ok   ELF binary"

if file "$MODULE_PATH" | grep -q "aarch64"; then
    echo "ok   architecture: aarch64"
    NM=aarch64-linux-gnu-nm
    READELF=aarch64-linux-gnu-readelf
else
    echo "warn architecture is NOT aarch64 — this build will not load on Move"
    echo "     (fine for host-side tests; do not ship it)"
    NM=nm
    READELF=readelf
fi

$READELF -h "$MODULE_PATH" | grep -q "Type:.*DYN" || fail "not a shared object (ET_DYN)"
echo "ok   shared object"

# The one symbol that must be exported. Everything else is hidden by
# -fvisibility=hidden; move_plugin_init_v2 carries an explicit default
# visibility attribute in schwung_wrapper.c.
if ! $NM -D --defined-only "$MODULE_PATH" | grep -q " move_plugin_init_v2$"; then
    echo ""
    echo "exported symbols:"
    $NM -D --defined-only "$MODULE_PATH" | head -20
    fail "move_plugin_init_v2 is not exported"
fi
echo "ok   move_plugin_init_v2 exported"

# -Wl,--no-undefined should already have caught these at link time; a leftover
# here means the flag was dropped.
#
# -D is required: the release build is stripped, so plain `nm -u` reports
# "no symbols" and the check passes vacuously. The dynamic symbol table
# survives stripping and is the one the loader actually resolves against.
#
# Only strong undefined symbols (nm type 'U') count. Every shared object
# carries weak undefined ('w') entries — __gmon_start__ and the two
# _ITM_*registerTMCloneTable stubs are emitted by GCC's crt files and resolve
# to null when absent. Flagging those is noise that trains you to ignore the
# check.
UNDEF_LIST=$($NM -D -u "$MODULE_PATH" 2>/dev/null | awk '$1 == "U" { print $2 }' | grep -v "GLIBC\|@@" || true)
UNDEF=$(printf '%s' "$UNDEF_LIST" | grep -c . || true)
if [ "$UNDEF" -gt 0 ]; then
    echo "warn $UNDEF strong undefined symbols (-Wl,--no-undefined should have caught these):"
    printf '%s\n' "$UNDEF_LIST" | head -10 | sed 's/^/     /'
else
    echo "ok   no strong undefined symbols"
fi

# The name check the old scripts got wrong in both directions: the build
# produced dsp.so while module.json asked for setBFree.so, and the validator
# looked for setBFree.so and exited 1 under `set -e`.
if [ -f "$MODULE_JSON" ]; then
    WANT=$(grep -o '"dsp"[[:space:]]*:[[:space:]]*"[^"]*"' "$MODULE_JSON" | sed 's/.*"\([^"]*\)"$/\1/')
    HAVE=$(basename "$MODULE_PATH")
    if [ "$WANT" != "$HAVE" ]; then
        fail "module.json says \"dsp\": \"$WANT\" but the build produced $HAVE"
    fi
    echo "ok   module.json \"dsp\" matches the artifact ($HAVE)"

    grep -q '"api_version"[[:space:]]*:[[:space:]]*2' "$MODULE_JSON" \
        || fail "module.json must declare \"api_version\": 2 — the chain host rejects v1 synths"
    echo "ok   module.json declares api_version 2"

    if [ -n "$PY" ]; then
        "$PY" -c "import json,sys; json.load(open(sys.argv[1]))" "$MODULE_JSON" \
            || fail "module.json is not valid JSON"
        echo "ok   module.json parses"
    else
        echo "skip module.json JSON parse ($PY_WHY)"
    fi
fi

# help.json ships in the same directory and is read by the Shadow UI. It fails
# silently in two different ways — wrong top-level key, or a line drawn off the
# right edge — so it gets its own checker.
HELP_JSON="$REPO_ROOT/src/schwung_bfree/help.json"
if [ -f "$HELP_JSON" ]; then
    if [ -n "$PY" ]; then
        "$PY" "$SCRIPT_DIR/check-help.py" "$HELP_JSON" || fail "help.json validation failed"
    else
        echo "skip help.json validation ($PY_WHY)"
    fi
fi

SIZE_KB=$(( $(stat -c%s "$MODULE_PATH") / 1024 ))
echo "ok   size: ${SIZE_KB} KB"

echo ""
echo "=== validation passed ==="
