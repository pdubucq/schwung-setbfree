#!/usr/bin/env bash
# Build the setBfree organ module for Ableton Move (aarch64).
#
# Usage:  ./scripts/build.sh            # release
#         ./scripts/build.sh --debug    # keep asserts, no strip
#
# Needs Docker, or an aarch64 cross-toolchain already on PATH. With Docker the
# script builds its own image and re-runs itself inside it, so there is nothing
# to install and no editor or dev-container involved.
#
# Output:
#   build/modules/dsp.so   the compiled module
#   dist/setBfree-organ/   the directory that goes onto the device
#   dist/setBfree-organ.tar.gz

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
cd "$REPO_ROOT"

IMAGE=b5-builder

# ---------------------------------------------------------------------
# Docker bootstrap.
#
# Skipped when the cross-compiler is already present — that covers both "we
# are already inside the container" and "the toolchain is installed natively".
# ---------------------------------------------------------------------
if ! command -v aarch64-linux-gnu-gcc >/dev/null 2>&1; then
    command -v docker >/dev/null 2>&1 || {
        echo "FAIL: neither aarch64-linux-gnu-gcc nor docker found."
        echo "      Install Docker, or an aarch64 cross-toolchain."
        exit 1
    }

    echo "=== no cross-toolchain on PATH — building in Docker ==="
    docker image inspect "$IMAGE" >/dev/null 2>&1 || {
        echo "--- building image $IMAGE (first run only) ---"
        docker build -t "$IMAGE" "$REPO_ROOT"
    }

    # The repo mounts at /work, NOT /build: the image keeps a convenience
    # toolchain file under /opt and the build tree is /build inside the repo,
    # so a mount at /build would shadow one with the other.
    #
    # -u maps the caller's uid/gid so build/ and dist/ do not come back owned
    # by root on the host.
    exec docker run --rm \
        --network host \
        -v "$REPO_ROOT:/work" \
        -u "$(id -u):$(id -g)" \
        -w /work \
        "$IMAGE" ./scripts/build.sh "$@"
fi

BUILD_TYPE=Release
[ "${1:-}" = "--debug" ] && BUILD_TYPE=Debug

echo "=== setBfree module build (aarch64, $BUILD_TYPE) ==="
echo ""

rm -rf build
mkdir -p build

# The toolchain file is generated here rather than baked into the image, so the
# script cannot disagree with the Dockerfile about where it lives — and so a
# native cross-toolchain (no Docker at all) works identically.
cat > build/toolchain-arm64.cmake << 'EOF'
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)
set(CMAKE_FIND_ROOT_PATH /usr/aarch64-linux-gnu)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
EOF

cd build

# CMAKE_STRIP must be absolute: given a bare name, CMake resolves it relative
# to the build directory and the POST_BUILD rule dies with
#   make[2]: ./aarch64-linux-gnu-strip: No such file or directory
# after the compile and link have both succeeded.
STRIP_BIN="$(command -v aarch64-linux-gnu-strip)"

# The toolchain file is passed explicitly rather than exported. CMake honours
# CMAKE_TOOLCHAIN_FILE from the environment, and the toolchain file's
# set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc) beats -DCMAKE_C_COMPILER=gcc on
# the command line — which silently turned the NATIVE test build into a cross
# build once already.
cmake -DCMAKE_TOOLCHAIN_FILE="$PWD/toolchain-arm64.cmake" \
      -DCMAKE_C_COMPILER="$(command -v aarch64-linux-gnu-gcc)" \
      -DCMAKE_CXX_COMPILER="$(command -v aarch64-linux-gnu-g++)" \
      -DCMAKE_STRIP="$STRIP_BIN" \
      -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
      ..

make -j"$(nproc)"

echo ""
file modules/dsp.so

# The device is aarch64. A host-architecture .so here would deploy, fail to
# dlopen on the Move, and report nothing more useful than "dlopen failed".
if ! file modules/dsp.so | grep -q "ARM aarch64"; then
    echo "FAIL: build/modules/dsp.so is not aarch64 — the cross toolchain was not used."
    exit 1
fi

cd "$REPO_ROOT"
bash scripts/validate.sh

# ---------------------------------------------------------------------
# Stage dist/setBfree-organ/ — the exact directory that goes on the device.
# module.json and help.json come from src/schwung_bfree/, which is the single
# source of truth. An earlier layout kept copies at the deploy root where the
# staging step never picked them up, so the deployed directory had a dsp.so
# and no manifest.
# ---------------------------------------------------------------------
MODULE_DIR="$REPO_ROOT/dist/setBfree-organ"
rm -rf "$REPO_ROOT/dist"
mkdir -p "$MODULE_DIR"

cp build/modules/dsp.so              "$MODULE_DIR/dsp.so"
cp src/schwung_bfree/module.json     "$MODULE_DIR/module.json"
cp src/schwung_bfree/help.json       "$MODULE_DIR/help.json"
chmod +x "$MODULE_DIR/dsp.so"

tar -czf "$REPO_ROOT/dist/setBfree-organ.tar.gz" -C "$REPO_ROOT/dist" setBfree-organ

echo ""
echo "Staged $MODULE_DIR"
ls -lh "$MODULE_DIR"
echo ""
echo "Install with:  ./scripts/install.sh"
