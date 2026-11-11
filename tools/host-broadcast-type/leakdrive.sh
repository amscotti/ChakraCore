#!/bin/sh
# -------------------------------------------------------------------------------------------------------
# Copyright (c) ChakraCore Project Contributors. All rights reserved.
# Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
# -------------------------------------------------------------------------------------------------------
# RSS-isolation A/B for the agentless-broadcast leak probe
# (host-broadcast-type repair iteration).
# Usage: sh tools/host-broadcast-type/leakdrive.sh <ch-binary> [iters=6000]
#
# Runs leakprobe.js in both modes (broadcast vs control — identical
# allocation loops, the broadcast leg adds one agentless valid
# WScript.Broadcast per iteration) and reports each leg's peak RSS.
# Pre-repair: the broadcast leg's RSS vastly exceeds the control leg
# (every leaked content reference pins its 64 KiB buffer). Post-repair:
# the two legs match.
set -u
BIN=${1:?usage: leakdrive.sh <ch-binary> [iters]}
ITERS=${2:-6000}
DIR=$(cd "$(dirname "$0")" && pwd)
TMP=${TMPDIR:-/tmp}/host-broadcast-type-leak
mkdir -p "$TMP"

for MODE in broadcast control; do
    # -Test262 installs WScript.Broadcast; without it the probe's first
    # broadcast throws "doesn't support property or method 'Broadcast'".
    # macOS /usr/bin/time -l reports the maximum RSS in BYTES.
    /usr/bin/time -l "$BIN" -nonative -ESBigInt -ESSharedArrayBuffer -Test262 -args "$MODE" "$ITERS" -endargs "$DIR/leakprobe.js" >"$TMP/out-$MODE" 2>"$TMP/time-$MODE"
    rc=$?
    rss=$(awk '/maximum resident set size/ {print $1}' "$TMP/time-$MODE")
    mib=$(( ${rss:-0} / 1048576 ))
    echo "bin=$BIN mode=$MODE iters=$ITERS rc=$rc peak_rss=${rss:-parse-failed}B (~${mib}MiB)"
done
