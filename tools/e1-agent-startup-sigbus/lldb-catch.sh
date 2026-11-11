#!/bin/sh
# -------------------------------------------------------------------------------------------------------
# Copyright (c) ChakraCore Project Contributors. All rights reserved.
# Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
# -------------------------------------------------------------------------------------------------------
# Retry-loop lldb catch with OnSegmentAlloc/OnThreadInit tracing.
DIR=/Users/ascotti/Documents/code/ChakraCore-wt/agent-startup-sigbus/out/sigbus
COUNT=${1:-30}
for i in $(seq 1 $COUNT); do
  cat > $DIR/lldb.cmd <<EOF
process launch -s true
breakpoint set -n "_ZN6Memory31X64WriteBarrierCardTableManager14OnSegmentAllocEPcm" -C "frame info" -C "register read x0 x1" -C "continue"
breakpoint set -n "_ZN6Memory31X64WriteBarrierCardTableManager12OnThreadInitEv" -C "frame info" -C "register read sp" -C "continue"
process handle -n false -p false -s true SIGBUS
continue
bt all
reg read x0 x1 sp
quit
EOF
  ( lldb -b -s $DIR/lldb.cmd $DIR/bin-pre/ch > $DIR/lldb-tmp.txt 2>&1 & LP=$!; sleep 30; kill -9 $LP 2>/dev/null; wait $LP 2>/dev/null )
  if grep -q "stop reason = signal SIGBUS\|stop reason = SIGBUS" $DIR/lldb-tmp.txt; then
    echo "CAUGHT at iteration $i"; cp $DIR/lldb-tmp.txt $DIR/lldb-caught.txt; break
  fi
  echo "iter $i: $(grep -c 'frame info' $DIR/lldb-tmp.txt 2>/dev/null) bp hits"
done
