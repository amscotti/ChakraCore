#!/bin/sh
# A/B driver for the host-spawn-check repair (doc/c2-s6-cross-agent-waitasync.md).
# Usage: sh tools/host-spawn-check/drive.sh <ch-binary> [runs=10]
#
# Injects a transient thread-exhaustion window (failthreads.c: budget
# covers ch startup + the 5 phase-1 agents, then exactly ONE
# pthread_create — the 6th agent spawn — is refused with EAGAIN, later
# creates succeed) and classifies each run of spawnfail.js:
#   caught  - post-fix clean-error mode: the refused spawn raised a
#             catchable JS exception, the next spawn succeeded
#             (phase2=1) and the process exited 0 joining all agents
#   hang    - the run hit the driver timeout (rc 124): pre-fix mode,
#             main thread stuck in WaitForSingleObject at
#             WScriptJsrt.cpp:879
#   other   - anything else (diagnostics preserved in $TMP/hsc-other-*)
#
# Requirements: cc, coreutils timeout (both present on the evidence
# machine; see README.md).
set -u
BIN=${1:?usage: drive.sh <ch-binary> [runs]}
RUNS=${2:-10}
DIR=$(cd "$(dirname "$0")" && pwd)
TMP=${TMPDIR:-/tmp}/host-spawn-check
mkdir -p "$TMP"

# Build the shim next to the other evidence outputs.
SHIM=$TMP/failthreads.dylib
cc -shared -fPIC -o "$SHIM" "$DIR/failthreads.c" || { echo "drive.sh: cc failed"; exit 1; }

CAUGHT=0; HANGS=0; OTHER=0
i=0
while [ "$i" -lt "$RUNS" ]; do
    # FAILTHREADS_AT=8: 3 startup creates (measured, test + debug
    # profiles) + 5 phase-1 agents; the next create is the spawn under
    # test. FAILTHREADS_DENY=1: exactly one refusal, then creates
    # succeed again (the consistency probe in spawnfail.js needs one).
    FAILTHREADS_AT=8 FAILTHREADS_DENY=1 \
    DYLD_INSERT_LIBRARIES="$SHIM" \
    timeout -k 5 20 "$BIN" -Test262 "$DIR/spawnfail.js" >"$TMP/out" 2>"$TMP/err"
    rc=$?
    if grep -q "^caught=Failed to create the agent thread\.$" "$TMP/out" && \
       grep -q "^phase2=1$" "$TMP/out" && [ "$rc" -eq 0 ]; then
        CAUGHT=$((CAUGHT + 1))
    elif [ "$rc" -eq 124 ] || [ "$rc" -eq 137 ]; then
        HANGS=$((HANGS + 1))
    else
        OTHER=$((OTHER + 1))
        cp "$TMP/out" "$TMP/hsc-other-$i.out" 2>/dev/null || true
        cp "$TMP/err" "$TMP/hsc-other-$i.err" 2>/dev/null || true
        echo "  other: run $i rc=$rc; see $TMP/hsc-other-$i.*"
    fi
    i=$((i + 1))
done

echo "bin=$BIN runs=$RUNS caught=$CAUGHT hangs=$HANGS other=$OTHER"
