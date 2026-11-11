#!/bin/sh
# MilesVR 0.1.80, v24: builds and runs tests/voice_pool_test.cpp on Linux (src/voice_pool.cpp against a pretend audio
# device, tests/winshim). Not part of the Windows build.
set -e
cd "$(dirname "$0")/.."
out="${TMPDIR:-/tmp}/voice_pool_test"
g++ -std=c++20 -O1 -g -Wall -Wextra -pthread -Itests/winshim -Iinclude \
    tests/voice_pool_test.cpp src/voice_pool.cpp -o "$out" "$@"
"$out"
