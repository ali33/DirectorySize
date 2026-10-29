#!/bin/bash
# DirectorySize (C++) — Copyright (c) 2026 META.vn
# Author: Nguyễn Đức Sơn
# SPDX-License-Identifier: GPL-2.0-only
# Runs INSIDE the Linux container (see test-linux.sh).  Modes: test | tsan | bench
set -euo pipefail
MODE="${1:-test}"
SRC=/src

build() {  # $1=dir  rest=extra cmake args
  local dir="$1"; shift
  cmake -S "$SRC" -B "$dir" -G Ninja "$@" >/dev/null
  cmake --build "$dir"
}

case "$MODE" in
  test)
    build /build -DCMAKE_BUILD_TYPE=Release
    cd /build && ctest --output-on-failure
    ;;
  tsan)
    build /build-tsan -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DCMAKE_CXX_FLAGS="-fsanitize=thread -g" -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread"
    cd /build-tsan && ctest --output-on-failure
    ;;
  bench)
    build /build -DCMAKE_BUILD_TYPE=Release
    T="${2:-/usr}"
    echo "== reference: find (regular files + symlinks, logical size)"
    TIMEFORMAT="find: %R s"; time find "$T" \( -type f -o -type l \) -printf '%s\n' | awk '{s+=$1} END{print "bytes=" s, "files=" NR}'
    echo "== reference: du -sb --apparent-size (includes directory entries and dedups hard links)"
    TIMEFORMAT="du: %R s"; time du -sb --apparent-size "$T" | tail -1
    for j in 1 2 4 8 16; do
      echo "== dirsize-cli -j $j"
      for r in 1 2 3; do /build/dirsize-cli "$T" -t 1 -q -j "$j" 2>&1 | grep -E '^Xong'; done
    done
    ;;
  sigint)
    build /build -DCMAKE_BUILD_TYPE=Release
    # Scan the whole container root (large), interrupt with SIGINT; expect exit 130 and a partial result.
    set +e
    timeout --preserve-status -s INT 0.05 /build/dirsize-cli / -q -t 3
    rc=$?
    echo "exit code: $rc (expected 130)"
    [ "$rc" = 130 ]
    ;;
  *) echo "unknown mode $MODE" >&2; exit 2 ;;
esac
