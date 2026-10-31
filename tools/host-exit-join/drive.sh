#!/bin/sh
# A/B driver for the host-exit-join repair (doc/c2-s6-cross-agent-waitasync.md).
# Usage: sh tools/host-exit-join/drive.sh <ch-binary> [runs]
# Classifies each run of agents100.js:
#   assert  - an ASSERTION line on stdout/stderr (debug-profile failure mode)
#   signal  - exit code >= 128 (crash: SIGTRAP/SIGSEGV/... after the assert,
#             or a use-after-free on profiles where Assert is compiled out)
#   other   - any other non-zero exit
#   clean   - exit 0 and no ASSERTION anywhere
set -u
BIN=${1:?usage: drive.sh <ch-binary> [runs]}
RUNS=${2:-50}
DIR=$(dirname "$0")

CLEAN=0; ASSERTS=0; SIGNALS=0; OTHER=0
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

i=0
while [ "$i" -lt "$RUNS" ]; do
    "$BIN" -Test262 "$DIR/agents100.js" >"$WORK/out" 2>"$WORK/err"
    rc=$?
    if grep -q ASSERTION "$WORK/out" "$WORK/err" 2>/dev/null; then
        ASSERTS=$((ASSERTS + 1))
    elif [ "$rc" -eq 0 ]; then
        CLEAN=$((CLEAN + 1))
    elif [ "$rc" -ge 128 ]; then
        SIGNALS=$((SIGNALS + 1))
    else
        OTHER=$((OTHER + 1))
        cp "$WORK/out" "$WORK/err" "$WORK/other-$i." 2>/dev/null || true
    fi
    i=$((i + 1))
done

echo "bin=$BIN runs=$RUNS clean=$CLEAN asserts=$ASSERTS signals=$SIGNALS other=$OTHER"
# Exit-code contract: 0 only when every run was clean, so this driver can
# gate (an A/B run against a pre-fix binary is SUPPOSED to exit 1 while
# still printing its assert/signal tally above).
[ "$ASSERTS" -eq 0 ] && [ "$SIGNALS" -eq 0 ] && [ "$OTHER" -eq 0 ] && [ "$CLEAN" -eq "$RUNS" ]
