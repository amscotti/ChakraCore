#!/bin/bash
# -------------------------------------------------------------------------------------------------------
# Copyright (c) ChakraCore Project Contributors. All rights reserved.
# Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
# -------------------------------------------------------------------------------------------------------
# B3.1 legacy baselines: cmake configure + ninja build + ninja check for the
# Debug/static and Release/shared legacy trees. Sequential (CPU courtesy),
# fully logged, exit codes recorded honestly.
#
# Exit-code fix vs the /tmp predecessor: the old runner used
#   (time CMD >step.log 2>&1; echo "exit: $?") 2>step.time
# whose echo went to the subshell's stdout (the terminal, uncaptured), so
# no log ever recorded a code -- and configure was never logged at all.
# Here every step appends "exit: N" to its own step log AND to the
# progress log, and configure is a logged step like the rest.
#
# Usage: run-baselines.sh [phase] [tree]
#   phase: all (default) | configure | build | check
#   tree:  both (default) | debug | release
# Env:
#   LOG  log directory (default /tmp/b3-baselines)
#   SRC  repo root (default: derived from this script's location)
set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SRC="${SRC:-$(cd "$SCRIPT_DIR/../.." && pwd)}"
LOG="${LOG:-/tmp/b3-baselines}"
PHASE="${1:-all}"
TREE="${2:-both}"

case "$PHASE" in all|configure|build|check) ;; *)
  echo "usage: $0 [all|configure|build|check] [both|debug|release]" >&2; exit 2;; esac
case "$TREE" in both|debug|release) ;; *)
  echo "usage: $0 [all|configure|build|check] [both|debug|release]" >&2; exit 2;; esac

cd "$SRC" || exit 1
mkdir -p "$LOG"
PROGRESS="$LOG/00-progress.log"
FAILED=0

note() { echo "$*" | tee -a "$PROGRESS"; }

# run_step <name> <logfile> <cmd...>: run cmd with stdout+stderr to logfile,
# append "exit: N" to the logfile, and mirror the outcome to the progress log.
run_step() {
  local name="$1" log="$2"; shift 2
  local start end code
  start=$(date +%s)
  note ""
  note "===== [$name] started: $(date) ====="
  note "cmd: $*"
  : >"$log"
  "$@" >>"$log" 2>&1
  code=$?
  echo "exit: $code" >>"$log"
  end=$(date +%s)
  note "===== [$name] done: $(date) wall: $((end - start))s exit: $code (log: $log) ====="
  if [ "$code" -ne 0 ]; then FAILED=1; fi
  return "$code"
}

{
echo "===== run-baselines.sh: phase=$PHASE tree=$TREE started: $(date) ====="
echo "src: $SRC"
echo "log: $LOG"
echo "cmake: $(cmake --version | head -1)"
echo "ninja: $(ninja --version)"
echo "clang: $(clang --version | head -1)"
echo "python: $(python3 --version)"
echo "ncpu: $(sysctl -n hw.ncpu)"
echo "git: $(git rev-parse --short HEAD) $(git branch --show-current)"
} | tee -a "$PROGRESS"

want() { # want <phase> <tree>: true if this run covers that combination
  { [ "$PHASE" = "all" ] || [ "$PHASE" = "$1" ]; } &&
  { [ "$TREE" = "both" ] || [ "$TREE" = "$2" ]; }
}

set +e
if want configure debug; then
  run_step "debug-configure" "$LOG/08-debug-configure.log" \
    cmake -S . -B build-legacy-debug -GNinja -DCMAKE_BUILD_TYPE=Debug -DSTATIC_LIBRARY=1 \
      -DDISABLE_JIT=ON -DICU_INCLUDE_PATH=/opt/homebrew/opt/icu4c/include \
      -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER=clang
fi
if want configure release; then
  run_step "release-configure" "$LOG/09-release-configure.log" \
    cmake -S . -B build-legacy-release -GNinja -DCMAKE_BUILD_TYPE=Release \
      -DDISABLE_JIT=ON -DICU_INCLUDE_PATH=/opt/homebrew/opt/icu4c/include \
      -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER=clang
fi
if want build debug; then
  run_step "debug-build" "$LOG/10-debug-build.log" ninja -C build-legacy-debug
fi
if want check debug; then
  run_step "debug-check" "$LOG/11-debug-check.log" ninja -C build-legacy-debug check
fi
if want build release; then
  run_step "release-build" "$LOG/12-release-build.log" ninja -C build-legacy-release
fi
if want check release; then
  run_step "release-check" "$LOG/13-release-check.log" ninja -C build-legacy-release check
fi

note "===== run-baselines.sh finished: $(date) failed=$FAILED ====="
exit "$FAILED"
