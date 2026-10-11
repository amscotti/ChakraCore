#!/usr/bin/env python3
#-------------------------------------------------------------------------------------------------------
# Copyright (c) ChakraCore Project Contributors. All rights reserved.
# Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
#-------------------------------------------------------------------------------------------------------
"""Replay Ninja link edges using a complete, input-verified Zig object generation."""
import argparse
from datetime import datetime, timezone
from pathlib import Path
import re
import shlex
import subprocess
import sys

import pilot

# Keep shell operators separate from quoted/escaped arguments. Rebuild individual
# argv commands only; never quote an entire `: && compiler && post-build` chain.
TOKEN = re.compile(r'''(?P<operator>&&|\|\||[;&|()<>])|(?P<word>(?:[^\s'"\\;&|()<>]|\\.|"(?:\\.|[^"\\])*"|'[^']*')+)''')
DRIVER = re.compile(r'^(?:clang(?:\+\+)?|gcc|g\+\+|cc|c\+\+)(?:-\d+)?$')


def shell_commands(text):
    commands, words, start, previous = [], [], None, 0
    for match in TOKEN.finditer(text):
        if text[previous:match.start()].strip():
            raise pilot.PilotError('unsupported shell syntax in Ninja command')
        previous = match.end()
        if match.group('operator'):
            if match.group() != '&&' or not words:
                raise pilot.PilotError('only simple commands chained with && are supported')
            commands.append((start, words[-1][2], [word[0] for word in words]))
            words, start = [], None
        else:
            if start is None:
                start = match.start()
            decoded = shlex.split(match.group())
            if len(decoded) != 1:
                raise pilot.PilotError('invalid shell argument')
            words.append((decoded[0], match.start(), match.end()))
    if text[previous:].strip() or not words:
        raise pilot.PilotError('empty or unsupported Ninja command')
    commands.append((start, words[-1][2], [word[0] for word in words]))
    return commands


def is_driver(name, drivers):
    return name in drivers or DRIVER.fullmatch(Path(name).name) is not None


def fresh_output(path, build):
    path = Path(path)
    if not path.resolve().is_relative_to(build):
        raise pilot.PilotError(f'output is outside the disposable build directory: {path}')
    return path


def plan_edge(text, build, storage, number, manifest, replacements, drivers, extra, stack_size):
    commands = shell_commands(text)
    if any(is_driver(argv[0], drivers) and '-c' in argv for _, _, argv in commands):
        return None
    cwd, edits, outputs, metadata, directories = build, [], set(), {}, set()

    def rewrite_objects(args, directory):
        result = []
        oi, _ = pilot.output_argument(args)
        for index, value in enumerate(args):
            if index != oi and not value.startswith('-') and value.endswith('.o'):
                original = str(pilot.absolute(value, directory))
                if original not in replacements:
                    raise pilot.PilotError(f'object is absent from the compile manifest: {original}')
                value = replacements[original]
            result.append(value)
        return result

    for command_index, (start, end, argv) in enumerate(commands):
        if argv[0] == 'cd':
            if len(argv) != 2:
                raise pilot.PilotError('unsupported cd command')
            cwd = pilot.absolute(argv[1], cwd)
            continue
        driver = is_driver(argv[0], drivers)
        archive = Path(argv[0]).name in ('ar', 'llvm-ar', 'gcc-ar')
        rewritten = list(argv)
        expanded = []
        for index, value in enumerate(argv):
            if value.startswith('@') and not value.startswith(('@rpath/', '@loader_path/', '@executable_path/')):
                response = pilot.absolute(value[1:], cwd)
                if not response.is_file():
                    raise pilot.PilotError(f'response file is missing: {response}')
                response_args = shlex.split(response.read_text())
                if any(arg.startswith('@') and not arg.startswith(
                        ('@rpath/', '@loader_path/', '@executable_path/')) for arg in response_args):
                    raise pilot.PilotError('nested response files are not supported')
                response_args = rewrite_objects(response_args, cwd)
                if driver:
                    response_args = pilot.strip_driver_options(response_args)
                rewritten_response = storage / f'link-{number}-{command_index}-{index}.rsp'
                rewritten_response.write_text(shlex.join(response_args) + '\n')
                rewritten[index] = '@' + str(rewritten_response)
                expanded.extend(response_args)
            else:
                expanded.append(value)
        if archive:
            if len(expanded) < 3 or not re.fullmatch(r'-?[a-zA-Z]+', expanded[1]):
                raise pilot.PilotError('unsupported archive command')
            outputs.add(fresh_output(pilot.absolute(expanded[2], cwd), build))
        oi, output = pilot.output_argument(expanded)
        if output is not None and not (driver or archive) and any(
                index != oi and value.endswith('.o') for index, value in enumerate(expanded)):
            raise pilot.PilotError('object-linking edge does not use a supported compiler/archive driver')
        if output is not None:
            path = fresh_output(pilot.absolute(output, cwd), build)
            outputs.add(path)
            if driver:
                shared = any(flag in expanded for flag in ('-shared', '-dynamiclib'))
                metadata[path] = None if shared else stack_size
        elif driver:
            raise pilot.PilotError('link command has no discoverable -o output')
        if driver or archive:
            rewritten = rewrite_objects(rewritten, cwd)
        if driver:
            flags = pilot.strip_driver_options(rewritten[1:])
            # CMake separates `-Wl,-force_load,` from its archive. Join argv items,
            # preserving quoted paths, instead of matching raw whitespace.
            index = 0
            while index < len(flags):
                if flags[index] == '-Wl,-force_load,':
                    if index + 1 == len(flags):
                        raise pilot.PilotError('-force_load has no archive')
                    flags[index:index + 2] = [flags[index] + flags[index + 1]]
                index += 1
            flags += pilot.target_flags(manifest['options'], linking=True) + extra
            if pilot.macos_version(manifest['options']['target']) and not shared:
                flags.append(f'-Wl,-stack_size,{hex(stack_size)}')
            rewritten = [manifest['toolchain']['executable'], 'c++', *flags]
        if Path(argv[0]).name in ('cmake', 'cmake3') and len(argv) >= 4 and argv[1] == '-E':
            if argv[2] == 'make_directory':
                directories.update(pilot.absolute(value, cwd) for value in argv[3:])
            elif argv[2] == 'copy_if_different':
                destination = pilot.absolute(argv[-1], cwd)
                for source in argv[3:-1]:
                    path = destination / Path(source).name if destination.is_dir() or destination in directories else destination
                    outputs.add(fresh_output(path, build))
        if rewritten != argv:
            edits.append((start, end, shlex.join(rewritten)))
    for start, end, replacement in reversed(edits):
        text = text[:start] + replacement + text[end:]
    return {'command': text, 'outputs': sorted(outputs), 'metadata': metadata}


def link(args):
    build = Path(args.build_dir).resolve()
    extra_flags = shlex.split(args.extra_link_flags)
    pilot.validate_extra_target_flags(extra_flags)
    compiler = pilot.toolchain(args.zig_bin)
    with pilot.build_lock(build) as storage:
        report = {'state': 'running', 'targets': args.targets, 'edges': [],
                  'started_utc': datetime.now(timezone.utc).isoformat()}
        pilot.write_json(storage / 'last-link.json', report)
        try:
            manifest, entries, replacements = pilot.load_generation(build, compiler)
            report.update(generation=manifest['generation'], toolchain=compiler,
                          options=manifest['options'], stack_size=args.stack_size,
                          extra_link_flags=extra_flags)
            drivers = {entry['args'][0] for entry in entries}
            edges, seen = [], set()
            for target in args.targets:
                result = subprocess.run(['ninja', '-t', 'commands', target], cwd=build,
                                        capture_output=True, text=True, timeout=30)
                if result.returncode:
                    raise pilot.PilotError(f'ninja could not enumerate {target}: {result.stderr.strip()}')
                target_edges = []
                for command in result.stdout.splitlines():
                    if not command.strip() or command.lstrip().startswith('#'):
                        continue
                    edge = plan_edge(command, build, storage, len(edges) + len(target_edges),
                                     manifest, replacements, drivers,
                                     report['extra_link_flags'], args.stack_size)
                    if edge:
                        target_edges.append(edge)
                if not target_edges or not any(edge['outputs'] for edge in target_edges):
                    raise pilot.PilotError(f'{target}: no output-producing link/archive edges')
                print(f'--- target {target}: {len(target_edges)} non-compile edge(s)', flush=True)
                for edge in target_edges:
                    if edge['command'] not in seen:
                        edges.append(edge)
                        seen.add(edge['command'])
            for index, edge in enumerate(edges):
                # Removing this invocation's declared outputs makes a shell no-op
                # fail even if a previous link completed seconds ago.
                for output in edge['outputs']:
                    output.unlink(missing_ok=True)
                result = subprocess.run(edge['command'], cwd=build, shell=True,
                                        capture_output=True, text=True, timeout=900)
                missing = [str(path) for path in edge['outputs']
                           if not path.is_file() or path.stat().st_size == 0]
                print(f'  cmd{index}: rc={result.returncode} outputs='
                      f'{[str(path.relative_to(build)) for path in edge["outputs"]]} missing={missing}', flush=True)
                if result.returncode or missing:
                    raise pilot.PilotError(f'edge failed: rc={result.returncode}, missing={missing}\n{result.stderr[-4000:]}')
                artifacts = []
                for output in edge['outputs']:
                    details = pilot.macho_metadata(output, manifest['options']['target'], edge['metadata'][output]) \
                        if output in edge['metadata'] else None
                    artifacts.append({'path': str(output), 'sha256': pilot.digest(output), 'macho': details})
                report['edges'].append({'command': edge['command'], 'artifacts': artifacts})
                # A custom generator must not invalidate the objects about to link.
                if not edge['metadata']:
                    pilot.validate_inputs(manifest['inputs'])
            pilot.validate_inputs(manifest['inputs'])
            report['state'] = 'complete'
            report['finished_utc'] = datetime.now(timezone.utc).isoformat()
            pilot.write_json(storage / 'last-link.json', report)
            print('LINK-PILOT-OK')
            return 0
        except (pilot.PilotError, OSError, ValueError, KeyError, TypeError, subprocess.SubprocessError) as error:
            report.update(state='failed', error=str(error))
            pilot.write_json(storage / 'last-link.json', report)
            raise


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('build_dir')
    parser.add_argument('zig_bin')
    parser.add_argument('targets', nargs='+')
    parser.add_argument('--extra-link-flags', default='', help='use = for a value beginning with -')
    parser.add_argument('--stack-size', type=pilot.positive, default=8 * 1024 * 1024,
                        help='explicit macOS executable stack, in bytes (default: 8 MiB)')
    try:
        return link(parser.parse_args())
    except (pilot.PilotError, OSError, ValueError, KeyError, TypeError, subprocess.SubprocessError) as error:
        print(f'ABORT: {error}', file=sys.stderr)
        return 2


if __name__ == '__main__':
    sys.exit(main())
