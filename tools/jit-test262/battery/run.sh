#!/bin/sh
# Battery runner for the jit-reentrancy-fix lane (doc/c2-jit-reentrancy-fix.md).
#
# Runs every battery script under the five caller modes from
# doc/c2-jit-test262.md's caller-mode matrix and prints an exit-code matrix.
# All cells must be 0 on a fixed engine; on the unfixed base every cell of
# warmed-set-methods / warmed-groupby / exn-set-methods / exn-groupby is 134
# in the four JIT modes (and 0 under -nonative, the expected-value oracle).
#
# Usage: run.sh [ch-binary] [probe-iterations]
#   ch-binary          default out/jit/bin/ch
#   probe-iterations   passed to callback-probes.js via -args (default 1200;
#                      lower it, e.g. 30, for slow -nonative oracle runs)
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
CH=${1:-$HERE/../../../out/jit/bin/ch}
ITERS=${2:-1200}

run_one() {
    flags=$1; name=$2; script=$3
    rc=0
    if [ "$script" = "callback-probes.js" ]; then
        "$CH" -args "$ITERS" -endargs $flags "$HERE/$script" >/dev/null 2>&1 || rc=$?
    else
        "$CH" $flags "$HERE/$script" >/dev/null 2>&1 || rc=$?
    fi
    printf '%s' "$rc"
}

printf '%-12s' 'mode\script'
for s in warmed-set-methods warmed-groupby exn-set-methods exn-groupby callback-probes; do
    printf ' %-18s' "$s"
done
printf '\n'
for mode in '|default' '-mic:1|mic' '-maxInterpretCount:1 -maxSimpleJitRunCount:1 -bgjit-|interp' \
            '-forceNative -off:fulljit|simplejit' '-forceNative -off:simpleJit -bgJitDelay:0|fulljit' \
            '-nonative|nojit'; do
    flags=${mode%|*}; name=${mode#*|}
    printf '%-12s' "$name"
    for s in warmed-set-methods warmed-groupby exn-set-methods exn-groupby callback-probes; do
        printf ' %-18s' "$(run_one "$flags" "$name" "$s.js")"
    done
    printf '\n'
done
