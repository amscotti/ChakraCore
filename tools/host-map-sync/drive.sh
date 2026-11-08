#!/bin/sh
# A/B driver for the host-map-sync repro (swarm-triage R1, doc/e13-swarm-triage.md).
# Usage: sh tools/host-map-sync/drive.sh <ch-binary> [runs] [agents]
# Builds a scratch workdir with the per-agent dependency modules (agents
# resolve relative specifiers against CWD), runs the repro N times, and
# classifies each run:
#   assert  - an ASSERTION line on stdout/stderr (debug-profile failure mode)
#   signal  - exit code >= 128 (crash: heap corruption from unsynchronized
#             map mutation)
#   other   - any other non-zero exit, or a wrong summary line (import
#             loss / spawn loss / rejection)
#   clean   - exit 0, the expected summary, no ASSERTION anywhere
set -u
BIN=${1:?usage: drive.sh <ch-binary> [runs] [agents]}
RUNS=${2:-10}
AGENTS=${3:-40}
# Absolute: the per-run subshell cd's into the scratch workdir (agents
# resolve CWD-relative specifiers there), so a relative script path breaks.
DIR=$(cd "$(dirname "$0")" && pwd)
CLEAN=0; ASSERTS=0; SIGNALS=0; OTHER=0
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

# Fixture modules: one per agent (each agent's fetch registers a distinct
# record in its own runtime). The main thread's concurrent import target,
# dep-main.js, is committed next to the repro (it resolves through the
# main script's scriptDirMap entry, not CWD).
i=0
while [ "$i" -lt "$AGENTS" ]; do
    printf 'export const v = %s;\n' "$i" >"$WORK/dep-$i.js"
    i=$((i + 1))
done

WANT="agents=$AGENTS started=$AGENTS importOk=$AGENTS importErr=0"
i=0
while [ "$i" -lt "$RUNS" ]; do
    (cd "$WORK" && "$BIN" -Test262 -args "$AGENTS" -endargs "$DIR/agents-loadsync.js") \
        >"$WORK/out" 2>"$WORK/err"
    rc=$?
    if grep -q ASSERTION "$WORK/out" "$WORK/err" 2>/dev/null; then
        ASSERTS=$((ASSERTS + 1))
    elif [ "$rc" -ge 128 ]; then
        SIGNALS=$((SIGNALS + 1))
    elif [ "$rc" -ne 0 ]; then
        OTHER=$((OTHER + 1))
        cp "$WORK/out" "$WORK/err" "$WORK/other-$i." 2>/dev/null || true
    elif ! grep -q "^$WANT\$" "$WORK/out" || grep -q HOST-MAP-SYNC-LOSS "$WORK/out"; then
        OTHER=$((OTHER + 1))
        cp "$WORK/out" "$WORK/err" "$WORK/other-$i." 2>/dev/null || true
    else
        CLEAN=$((CLEAN + 1))
    fi
    i=$((i + 1))
done

echo "bin=$BIN runs=$RUNS agents=$AGENTS clean=$CLEAN asserts=$ASSERTS signals=$SIGNALS other=$OTHER"
# Exit-code contract: 0 only when every run was clean, so this driver can
# gate (an A/B run against a pre-fix binary is SUPPOSED to exit 1).
[ "$ASSERTS" -eq 0 ] && [ "$SIGNALS" -eq 0 ] && [ "$OTHER" -eq 0 ] && [ "$CLEAN" -eq "$RUNS" ]
