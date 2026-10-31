#!/bin/sh
# A/B driver for the host-broadcast-type repair
# (doc/c2-s6-cross-agent-waitasync.md, follow-up to host-spawn-check).
# Usage: sh tools/host-broadcast-type/drive.sh <ch-binary> [runs=20]
#
# Each run executes repro.js (WScript.Broadcast of a non-SharedArrayBuffer
# with a live agent child) and is classified:
#   throw  - post-fix mode: exit 0, the rejected broadcast threw a
#            catchable script error ("threw:"), and the process survived
#            to the final line ("survived")
#   crash  - exit code >= 128 (pre-fix mode: children were signaled to
#            consume a stale/null shared content handle; rc=139 SIGSEGV)
#   other  - anything else (diagnostics preserved in $TMP/hbt-other-*)
set -u
BIN=${1:?usage: drive.sh <ch-binary> [runs]}
RUNS=${2:-20}
DIR=$(cd "$(dirname "$0")" && pwd)
TMP=${TMPDIR:-/tmp}/host-broadcast-type
mkdir -p "$TMP"

THREW=0; CRASHES=0; OTHER=0
i=0
while [ "$i" -lt "$RUNS" ]; do
    "$BIN" -nonative -ESBigInt -ESSharedArrayBuffer -Test262 "$DIR/repro.js" >"$TMP/out" 2>"$TMP/err"
    rc=$?
    if [ "$rc" -ge 128 ]; then
        CRASHES=$((CRASHES + 1))
    elif [ "$rc" -eq 0 ] && grep -q "^threw: " "$TMP/out" && grep -q "^survived$" "$TMP/out"; then
        THREW=$((THREW + 1))
    else
        OTHER=$((OTHER + 1))
        cp "$TMP/out" "$TMP/hbt-other-$i.out" 2>/dev/null || true
        cp "$TMP/err" "$TMP/hbt-other-$i.err" 2>/dev/null || true
        echo "  other: run $i rc=$rc; see $TMP/hbt-other-$i.*"
    fi
    i=$((i + 1))
done

echo "bin=$BIN runs=$RUNS threw=$THREW crashes=$CRASHES other=$OTHER"
