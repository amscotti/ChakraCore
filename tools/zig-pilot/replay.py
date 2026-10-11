#!/usr/bin/env python3
#-------------------------------------------------------------------------------------------------------
# Copyright (c) ChakraCore Project Contributors. All rights reserved.
# Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
#-------------------------------------------------------------------------------------------------------
"""Compile an isolated Zig object generation and publish its verified manifest."""
import argparse
from concurrent.futures import FIRST_COMPLETED, ThreadPoolExecutor, wait
from datetime import datetime, timezone
from pathlib import Path
import subprocess
import sys
from threading import Lock
import uuid

import pilot


def replay(args):
    build = Path(args.build_dir).resolve()
    entries, database_hash = pilot.database(build)
    options = pilot.configuration(args.target, args.sysroot, args.extra_flags)
    compiler = pilot.toolchain(args.zig_bin)
    print(f'Zig {compiler["version"]}; target={options["target"]}; SDK={options["sysroot"]}', flush=True)
    print(compiler['zig_env'], flush=True)
    with pilot.build_lock(build) as storage:
        generation = uuid.uuid4().hex
        outdir = storage / generation
        outdir.mkdir()
        selected = entries[:args.limit] if args.limit else entries
        manifest = {
            'schema': pilot.SCHEMA, 'generation': generation, 'state': 'running',
            'started_utc': datetime.now(timezone.utc).isoformat(),
            'database_sha256': database_hash, 'toolchain': compiler, 'options': options,
            'total': len(entries), 'selected': len(selected), 'objects': [], 'inputs': {},
        }
        # Invalidate the previous complete run before any partial/failed replay begins.
        pilot.write_json(storage / pilot.MANIFEST, manifest)
        inputs, input_lock = {}, Lock()

        def record_input(path):
            name = str(path)
            with input_lock:
                if name not in inputs:
                    inputs[name] = pilot.digest(path)

        if options['sysroot']:
            for name in ('SDKSettings.json', 'SDKSettings.plist'):
                path = Path(options['sysroot']) / name
                if path.is_file():
                    record_input(path)

        def one(index, entry):
            output = outdir / f'tu{index}.o'
            depfile = outdir / f'tu{index}.d'
            try:
                record_input(Path(entry['source']))
                flags = pilot.strip_driver_options(entry['args'][1:], dependencies=True)
                oi, _ = pilot.output_argument(flags)
                flags[oi] = str(output) if flags[oi - 1] == '-o' else '-o' + str(output)
                command = [compiler['executable'], entry['driver'], *flags,
                           *pilot.target_flags(options), *options['extra_flags'],
                           '-MD', '-MF', str(depfile), '-MT', output.name]
                process = subprocess.run(command, cwd=entry['directory'],
                                         capture_output=True, text=True, timeout=600)
                (outdir / f'tu{index}.log').write_text(process.stdout + process.stderr)
                if process.returncode:
                    raise pilot.PilotError(f'compiler rc={process.returncode}\n{process.stderr[-4000:]}')
                if not output.is_file() or output.stat().st_size == 0:
                    raise pilot.PilotError('compiler returned success without a nonempty object')
                for path in pilot.dependency_paths(depfile, entry['directory']):
                    record_input(path)
                return index, entry['source'], {
                    'index': index, 'original': entry['original'], 'path': str(output),
                    'sha256': pilot.digest(output),
                }, None
            except (pilot.PilotError, OSError, subprocess.TimeoutExpired) as error:
                output.unlink(missing_ok=True)
                return index, entry['source'], None, str(error)

        objects, failures = [], []
        stop, next_index = False, 0
        with ThreadPoolExecutor(max_workers=args.jobs) as executor:
            pending = set()
            while pending or (next_index < len(selected) and not stop):
                while not stop and next_index < len(selected) and len(pending) < args.jobs:
                    pending.add(executor.submit(one, next_index, selected[next_index]))
                    next_index += 1
                done, pending = wait(pending, return_when=FIRST_COMPLETED)
                for future in done:
                    index, source, artifact, error = future.result()
                    print(f'[{"FAIL" if error else "OK  "}] {index}: {source}', flush=True)
                    if error:
                        failures.append((index, source, error))
                        stop |= args.fail_fast
                    else:
                        objects.append(artifact)
                # Already-running compiles are drained and counted; new work stops.

        skipped = len(selected) - len(objects) - len(failures)
        manifest.update(objects=sorted(objects, key=lambda item: item['index']), inputs=inputs,
                        passed=len(objects), failed=len(failures), not_run=skipped)
        invalid = None
        try:
            pilot.validate_inputs(inputs)
            if pilot.digest(build / 'compile_commands.json') != database_hash:
                raise pilot.PilotError('compile database changed during replay')
        except (pilot.PilotError, OSError) as error:
            invalid = str(error)
        complete = not failures and not skipped and not invalid and len(objects) == len(entries)
        manifest['state'] = 'complete' if complete else ('failed' if failures or invalid else 'partial')
        manifest['finished_utc'] = datetime.now(timezone.utc).isoformat()
        if invalid:
            manifest['error'] = invalid
        pilot.write_json(storage / pilot.MANIFEST, manifest)
        print(f'=== {len(objects)}/{len(selected)} compiled; failed={len(failures)}, '
              f'not-run={skipped}; generation={manifest["state"]} ===')
        for index, source, error in failures[:10]:
            print(f'--- FAIL {index}: {source}\n{error}')
        if invalid:
            print(f'ABORT: {invalid}')
        return 1 if failures or invalid or skipped else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('build_dir')
    parser.add_argument('zig_bin', help='Zig executable or its containing directory')
    parser.add_argument('--target', required=True, help='explicit Zig target, including macOS minimum')
    parser.add_argument('--sysroot', help='SDK/sysroot; required for macOS')
    parser.add_argument('--limit', type=pilot.positive)
    parser.add_argument('--jobs', type=pilot.positive, default=8)
    parser.add_argument('--fail-fast', action='store_true')
    parser.add_argument('--extra-flags', default='', help='shell-quoted flags; use = for leading dashes')
    try:
        return replay(parser.parse_args())
    except (pilot.PilotError, OSError, ValueError, subprocess.SubprocessError) as error:
        print(f'ABORT: {error}', file=sys.stderr)
        return 2


if __name__ == '__main__':
    sys.exit(main())
