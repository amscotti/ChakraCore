#!/usr/bin/env python3
#-------------------------------------------------------------------------------------------------------
# Copyright (c) ChakraCore Project Contributors. All rights reserved.
# Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
#-------------------------------------------------------------------------------------------------------
"""Run bin/ch/jstoc.py with a private CWD and deliver the header to OUT.

jstoc.py writes <basename>.h into its own CWD, which cannot be a declared
build output. This wrapper runs it in a temp dir and copies the result to
the build-declared output path, so the generated header becomes a tracked
file (contract section 5.4 edge; see build.zig).

Usage: jstoc_run.py <jstoc.py> <input.js> <varname> <out-header>
"""

import os
import shutil
import subprocess
import sys
import tempfile


def main(argv):
    if len(argv) != 5:
        print("usage: jstoc_run.py <jstoc.py> <input.js> <varname> <out-header>",
              file=sys.stderr)
        return 2
    jstoc_py, js_file, varname, out = argv[1], argv[2], argv[3], argv[4]
    tmp = tempfile.mkdtemp(prefix="jstoc")
    try:
        # Absolute paths: the build may pass relative paths, but jstoc.py
        # runs with CWD at the temp dir (it writes <basename>.h there).
        cmd = [sys.executable, os.path.abspath(jstoc_py),
               os.path.abspath(js_file), varname]
        proc = subprocess.run(cmd, cwd=tmp)
        if proc.returncode != 0:
            print("jstoc_run: jstoc.py failed with exit %d" % proc.returncode,
                  file=sys.stderr)
            return proc.returncode
        gen = os.path.join(tmp, os.path.basename(js_file) + ".h")
        if not os.path.isfile(gen):
            print("jstoc_run: expected output %s was not created" % gen,
                  file=sys.stderr)
            return 1
        shutil.copyfile(gen, out)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
