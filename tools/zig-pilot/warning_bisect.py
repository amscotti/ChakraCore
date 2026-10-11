#!/usr/bin/env python3
#-------------------------------------------------------------------------------------------------------
# Copyright (c) ChakraCore Project Contributors. All rights reserved.
# Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
#-------------------------------------------------------------------------------------------------------
"""Compare date-macro diagnostics without changing the replay generation."""
import argparse
from pathlib import Path
import subprocess

import pilot


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('build_dir')
    parser.add_argument('zig_bin')
    parser.add_argument('--target', required=True)
    parser.add_argument('--sysroot')
    args = parser.parse_args()
    entries, _ = pilot.database(Path(args.build_dir).resolve())
    entry = next(item for item in entries if 'JsrtHelper' in item['source'])
    options = pilot.configuration(args.target, args.sysroot, '')
    compiler = pilot.toolchain(args.zig_bin)
    outdir = Path(args.build_dir).resolve() / 'zigpilot-bisect'
    outdir.mkdir(exist_ok=True)
    base = entry['args']
    zig = [compiler['executable'], 'c++', *pilot.strip_driver_options(base[1:]),
           *pilot.target_flags(options)]
    variants = (
        ('apple-as-is', base), ('zig-as-is', zig),
        ('zig-no-Werror-flag', [flag for flag in zig if flag != '-Werror']),
        ('zig-plus-nodate', zig + ['-Wno-error=date-time']),
        ('zig-only-nodate', [flag for flag in zig if flag != '-Werror'] + ['-Wno-date-time']),
    )
    for name, argv in variants:
        argv = list(argv)
        output = outdir / f'{name}.o'
        output.unlink(missing_ok=True)
        oi, _ = pilot.output_argument(argv)
        argv[oi] = str(output) if argv[oi - 1] == '-o' else '-o' + str(output)
        result = subprocess.run(argv, cwd=entry['directory'], capture_output=True,
                                text=True, timeout=300)
        diagnostic = (result.stderr.strip().splitlines() or ['clean'])[-1][:200]
        print(f'{name}: rc={result.returncode}; object={output.is_file()} :: {diagnostic}')


if __name__ == '__main__':
    main()
