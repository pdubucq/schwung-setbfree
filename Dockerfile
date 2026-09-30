# setBfree Organ Plugin Build Environment
# Target: Ableton Move (aarch64 Linux)

FROM debian:bookworm

# Enable arm64 architecture for cross-compilation libraries
RUN dpkg --add-architecture arm64

RUN apt-get update && apt-get install -y \
    cmake \
    make \
    gcc-aarch64-linux-gnu \
    g++-aarch64-linux-gnu \
    binutils-aarch64-linux-gnu \
    file \
    gcc \
    libc6-dev \
    python3 \
    libpython3-stdlib \
    && rm -rf /var/lib/apt/lists/*

# python3 on Debian is split: python3.11-minimal has the interpreter,
# libpython3.11-stdlib has json, argparse, ast. Without the second package
# scripts/check-help.py dies with "No module named 'json'", which reads like a
# broken script rather than a half-installed interpreter. Fail the image build
# instead of the test run.
RUN python3 -c "import json, ast, argparse" \
    || { echo "python3 stdlib incomplete"; exit 1; }

# /work, not /build: scripts/build.sh bind-mounts the repo here, and the repo's
# own build tree is build/. Mounting at /build would shadow the toolchain file
# below with the build output, or vice versa.
WORKDIR /work

# NOTE: CC / CXX / CMAKE_TOOLCHAIN_FILE are deliberately NOT exported.
#
# They were, and it silently broke the native test build. CMake honours
# CMAKE_TOOLCHAIN_FILE, and the toolchain file's set(CMAKE_C_COMPILER
# aarch64-linux-gnu-gcc) BEATS a -DCMAKE_C_COMPILER=gcc on the command line.
# So scripts/test.sh configured what it believed was a native build,
# got an aarch64 dsp.so, and the x86-64 harness could not dlopen it — the
# exact defect the old test_setBfree.c had.
#
# Both build scripts name their compiler explicitly, so nothing here needs
# these. CROSS_PREFIX is informational only.
ENV CROSS_PREFIX=aarch64-linux-gnu-

# Convenience copy for anyone configuring CMake by hand:
#   cmake -DCMAKE_TOOLCHAIN_FILE=/opt/toolchain-arm64.cmake ...
# scripts/build.sh does NOT use this — it generates its own into build/ so the
# script and the image cannot disagree about the path.
RUN cat > /opt/toolchain-arm64.cmake << 'EOF'
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)

set(CMAKE_FIND_ROOT_PATH /usr/aarch64-linux-gnu)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
EOF

# Default build command
CMD ["bash", "scripts/build.sh"]
