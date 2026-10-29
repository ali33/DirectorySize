#!/bin/sh
# DirectorySize (C++) — Copyright (c) 2026 META.vn
# Author: Nguyễn Đức Sơn
# SPDX-License-Identifier: GPL-2.0-only
# Build and test the Linux port inside Docker.
#   scripts/test-linux.sh          # build + ctest
#   scripts/test-linux.sh tsan     # build with -fsanitize=thread + ctest
#   scripts/test-linux.sh bench    # scan /usr and compare with find/du  (bench [dir])
set -eu
cd "$(dirname "$0")/.."
IMG=dirsize-linux-build
MODE="${1:-test}"
shift 2>/dev/null || true
docker build -q -f docker/Dockerfile.linux -t "$IMG" docker >/dev/null
# MSYS_NO_PATHCONV stops Git Bash on Windows from mangling /src; pwd -W gives a Windows path there.
export MSYS_NO_PATHCONV=1
HERE="$(pwd -W 2>/dev/null || pwd)"
docker run --rm --cap-add=SYS_PTRACE -v "$HERE:/src:ro" "$IMG" bash /src/scripts/container-build.sh "$MODE" "$@"
