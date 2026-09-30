cmake_minimum_required(VERSION 3.10)
project(setBFree-Schwung C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(CMAKE_C_FLAGS "-fPIC -O3 -Wall -Wno-unused-parameter -Wno-sign-compare")

# Only move_plugin_init_v2 is exported (it carries an explicit
# visibility("default") attribute). Everything else stays internal so the host
# cannot accidentally bind to a setBfree symbol.
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fvisibility=hidden")

# setBfree is liberally sprinkled with assert(), e.g. tonegen.c:2504
#     assert (setting < 9);
# inside setDrawBar. An assert firing on the SPI callback calls abort(), which
# takes down the whole audio process — strictly worse than clamping to a wrong
# value. Every input this module feeds setBfree is range-clamped in
# schwung_bfree.c before the call, so the asserts have nothing left to catch.
# Configure with -DCMAKE_BUILD_TYPE=Debug to keep them live for host testing.
if(NOT CMAKE_BUILD_TYPE MATCHES Debug)
    add_definitions(-DNDEBUG)
endif()

# Include directories - order matters!
# Add setbfree first so #include "cfgParser.h" finds it
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/src/setbfree)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/src/host)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/src/schwung_bfree)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/src)

# Collect setBfree source files.
#
# Upstream's memstream.c is not part of this tree: it is a reimplementation of
# open_memstream() for platforms lacking it, glibc provides both functions
# natively, and it was the only GPL-3.0 file in an otherwise GPL-2.0-or-later
# codebase. Its header stays — pgmParser.h includes it, and the declarations
# are inert behind #ifndef HAVE_MEMSTREAM.
file(GLOB SETBFREE_SOURCES
    src/setbfree/*.c
)

set(SCHWUNG_BFREE_SOURCES
    src/schwung_bfree/schwung_wrapper.c
    src/schwung_bfree/schwung_bfree.c
    src/schwung_bfree/setbfree_stubs.c
)

# Create shared library (module)
add_library(dsp SHARED
    ${SETBFREE_SOURCES}
    ${SCHWUNG_BFREE_SOURCES}
)

# Ensure the .so has no undefined symbols: the Move host dlopen()s this with
# RTLD_NOW, so an unresolved symbol is a load failure at slot-load time, on the
# audio thread, with nothing but "dlopen failed" to go on.
target_link_options(dsp PRIVATE -Wl,--no-undefined)

# m: pow/sin/cos throughout the tone generator.
# pthread: the deferred-init worker in schwung_bfree.c, which exists because
#          create_instance runs on the SPI callback and setBfree's init does
#          not fit in a 2.9 ms audio block.
find_package(Threads REQUIRED)
target_link_libraries(dsp PRIVATE m Threads::Threads)

# Set output directory and name to dsp.so. The chain host resolves the module
# by the "dsp" field in module.json; keep the two in step.
set_target_properties(dsp PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/modules"
    PREFIX ""
    SUFFIX ".so"
)

# Strip symbols for production build.
#
# CMAKE_STRIP must be an ABSOLUTE path. Passing -DCMAKE_STRIP=aarch64-linux-gnu-strip
# makes CMake treat the bare name as a path relative to the build directory, and
# the POST_BUILD rule fails with
#     make[2]: ./aarch64-linux-gnu-strip: No such file or directory
# after a completely successful compile and link. So resolve it here rather than
# trusting whatever the caller passed.
if(NOT CMAKE_BUILD_TYPE MATCHES Debug)
    if(NOT IS_ABSOLUTE "${CMAKE_STRIP}")
        find_program(STRIP_EXECUTABLE NAMES "${CMAKE_STRIP}" strip)
    else()
        set(STRIP_EXECUTABLE "${CMAKE_STRIP}")
    endif()

    if(STRIP_EXECUTABLE)
        add_custom_command(TARGET dsp POST_BUILD
            COMMAND "${STRIP_EXECUTABLE}" $<TARGET_FILE:dsp>
            COMMENT "Stripping dsp.so with ${STRIP_EXECUTABLE}"
        )
    else()
        message(WARNING "strip not found (CMAKE_STRIP=${CMAKE_STRIP}); shipping unstripped")
    endif()
endif()

# Print build info
message(STATUS "setBFree module build configuration:")
message(STATUS "  C Compiler: ${CMAKE_C_COMPILER}")
message(STATUS "  C Flags: ${CMAKE_C_FLAGS}")
message(STATUS "  Build Type: ${CMAKE_BUILD_TYPE}")
message(STATUS "  Output: ${CMAKE_BINARY_DIR}/modules/dsp.so")
