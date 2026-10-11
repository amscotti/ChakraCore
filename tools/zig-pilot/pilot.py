#!/usr/bin/env python3
#-------------------------------------------------------------------------------------------------------
# Copyright (c) ChakraCore Project Contributors. All rights reserved.
# Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
#-------------------------------------------------------------------------------------------------------
"""Shared configuration and artifact provenance for the POSIX Zig pilot."""
from contextlib import contextmanager
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess

SCHEMA = 1
MANIFEST = 'manifest.json'
ENVIRONMENT = (
    'CPATH', 'C_INCLUDE_PATH', 'CPLUS_INCLUDE_PATH', 'LIBRARY_PATH',
    'SDKROOT', 'MACOSX_DEPLOYMENT_TARGET', 'ZIG_LIB_DIR', 'ZIG_LIBC',
)


class PilotError(Exception):
    pass


def digest(path):
    with Path(path).open('rb') as source:
        return hashlib.file_digest(source, 'sha256').hexdigest()


def write_json(path, value):
    path = Path(path)
    temporary = path.with_suffix(path.suffix + '.tmp')
    temporary.write_text(json.dumps(value, indent=2, sort_keys=True) + '\n')
    temporary.replace(path)


@contextmanager
def build_lock(build):
    directory = Path(build) / 'zigpilot-obj'
    directory.mkdir(exist_ok=True)
    with (directory / 'lock').open('a') as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as error:
            raise PilotError('another replay/link is using this build directory') from error
        yield directory


def toolchain(zig_bin):
    executable = Path(zig_bin).expanduser()
    if executable.is_dir():
        executable /= 'zig'
    executable = executable.resolve()
    version = subprocess.run([str(executable), 'version'], check=True,
                             capture_output=True, text=True).stdout.strip()
    environment = subprocess.run([str(executable), 'env'], check=True,
                                 capture_output=True, text=True).stdout.strip()
    return {
        'executable': str(executable), 'sha256': digest(executable),
        'version': version, 'zig_env': environment,
        'environment': {name: os.environ.get(name) for name in ENVIRONMENT},
    }


def positive(value):
    number = int(value, 0)
    if number <= 0:
        raise ValueError('must be positive')
    return number


def macos_version(target):
    match = re.search(r'(?:^|-)macos\.(\d+(?:\.\d+){0,2})(?:-|$)', target)
    return match.group(1) if match else None


def configuration(target, sysroot, extra):
    flags = shlex.split(extra)
    if any(flag in ('-o', '-MF', '-MT', '-MQ', '-MJ') or
           flag.startswith(('--output=', '-o')) for flag in flags):
        raise PilotError('extra flags must not redirect compiler outputs')
    validate_extra_target_flags(flags)
    root = str(Path(sysroot).expanduser().resolve()) if sysroot else None
    if root and not Path(root).is_dir():
        raise PilotError(f'SDK/sysroot directory does not exist: {root}')
    if '-macos' in target:
        if not macos_version(target):
            raise PilotError('macOS --target must contain an explicit minimum version')
        if not root:
            raise PilotError('macOS --target requires --sysroot (xcrun --show-sdk-path)')
    return {'target': target, 'sysroot': root, 'extra_flags': flags}


def validate_extra_target_flags(flags):
    if any(flag in ('-target', '--target', '--sysroot', '-isysroot') or
           flag.startswith(('--target=', '--sysroot=', '-mmacosx-version-min='))
           for flag in flags):
        raise PilotError('extra flags must not override the manifest target/SDK; use --target/--sysroot')


def target_flags(options, linking=False):
    flags = ['-target', options['target']]
    if options['sysroot']:
        flags += ['--sysroot', options['sysroot']]
    if macos_version(options['target']):
        # Zig resolves framework search paths relative to --sysroot for both
        # compilation and linking; prefixing the SDK here would double it.
        flags += ['-F', '/System/Library/Frameworks']
        if linking:
            flags += ['-L', '/usr/lib']
        else:
            flags += ['-isystem', str(Path(options['sysroot']) / 'usr/include')]
    return flags


def strip_driver_options(args, dependencies=False):
    """Replace inherited target/output-dependency settings, retaining engine flags."""
    result = []
    paired = {'-target', '--target', '--sysroot', '-isysroot'}
    if dependencies:
        paired |= {'-MF', '-MT', '-MQ', '-MJ'}
    index = 0
    while index < len(args):
        value = args[index]
        if value in paired:
            if index + 1 == len(args):
                raise PilotError(f'missing argument for {value}')
            index += 2
            continue
        if value.startswith(('--target=', '--sysroot=', '-mmacosx-version-min=')):
            index += 1
            continue
        if dependencies and (value in ('-MD', '-MMD', '-MP') or
                             value.startswith(('-MF', '-MT', '-MQ', '-MJ'))):
            index += 1
            continue
        result.append(value)
        index += 1
    return result


def output_argument(args):
    for index, value in enumerate(args):
        if value == '-o':
            if index + 1 == len(args):
                raise PilotError('missing argument for -o')
            return index + 1, args[index + 1]
        if value.startswith('-o') and len(value) > 2:
            return index, value[2:]
    return None, None


def absolute(path, directory):
    return (Path(directory) / path).resolve()


def database(build):
    path = Path(build) / 'compile_commands.json'
    raw = json.loads(path.read_text())
    if not isinstance(raw, list) or not raw:
        raise PilotError('compile_commands.json must contain at least one entry')
    entries = []
    outputs = set()
    for entry in raw:
        args = entry.get('arguments') or shlex.split(entry['command'])
        if not args or any(value in ('&&', '||', ';', '|') for value in args):
            raise PilotError('compile database entries must be simple compiler commands')
        index, output = output_argument(args)
        if index is None or '-c' not in args:
            raise PilotError(f'expected a compile command with -c and -o: {entry["file"]}')
        directory = absolute(entry['directory'], build)
        original = str(absolute(output, directory))
        if original in outputs:
            raise PilotError(f'duplicate compile output: {original}')
        outputs.add(original)
        source = absolute(entry['file'], directory)
        cxx = source.suffix in ('.cpp', '.cc', '.cxx', '.C', '.mm') or \
            '++' in Path(args[0]).name
        entries.append({
            'args': args, 'directory': str(directory), 'source': str(source),
            'original': original, 'driver': 'c++' if cxx else 'cc',
        })
    return entries, digest(path)


def dependency_paths(depfile, directory):
    text = Path(depfile).read_text().replace('\\\n', '')
    if ':' not in text:
        raise PilotError(f'invalid compiler dependency file: {depfile}')
    # -MT uses a simple tuN.o target; Make escapes spaces and doubles dollar signs.
    return [absolute(name, directory) for name in
            shlex.split(text.split(':', 1)[1].replace('$$', '$'))]


def validate_inputs(inputs):
    changed = [name for name, expected in inputs.items()
               if not Path(name).is_file() or digest(name) != expected]
    if changed:
        raise PilotError(f'{len(changed)} source/header/SDK input(s) changed; '
                         f're-run replay.py (first: {changed[0]})')


def load_generation(build, current_toolchain):
    path = Path(build) / 'zigpilot-obj' / MANIFEST
    if not path.is_file():
        raise PilotError('no compile manifest; run a complete replay.py first')
    manifest = json.loads(path.read_text())
    if manifest.get('schema') != SCHEMA or manifest.get('state') != 'complete':
        raise PilotError('compile generation is incomplete; run a complete replay.py')
    if manifest['toolchain'] != current_toolchain:
        raise PilotError('Zig executable/version/environment changed; re-run replay.py')
    entries, database_hash = database(build)
    if manifest['database_sha256'] != database_hash:
        raise PilotError('compile database changed; re-run replay.py')
    if len(manifest['objects']) != len(entries):
        raise PilotError('compile manifest does not cover the full database')
    validate_inputs(manifest['inputs'])
    replacements = {}
    generation = (Path(build) / 'zigpilot-obj' / manifest['generation']).resolve()
    for entry, item in zip(entries, manifest['objects']):
        twin = Path(item['path'])
        if item['original'] != entry['original'] or not twin.resolve().is_relative_to(generation):
            raise PilotError('object mapping does not match this generation')
        if not twin.is_file() or twin.stat().st_size == 0 or digest(twin) != item['sha256']:
            raise PilotError(f'object is missing, empty, or changed: {twin}; re-run replay.py')
        replacements[entry['original']] = str(twin)
    return manifest, entries, replacements


def macho_metadata(path, target, stack_size=None):
    if not macos_version(target):
        return None
    result = subprocess.run(['otool', '-l', str(path)], check=True,
                            capture_output=True, text=True).stdout
    minimum = re.search(r'\bminos (\S+)', result)
    stack = re.search(r'cmd LC_MAIN\s+cmdsize \d+\s+entryoff \d+\s+stacksize (\d+)', result)
    expected = tuple(int(value) for value in macos_version(target).split('.'))
    actual = tuple(int(value) for value in minimum.group(1).split('.')) if minimum else ()
    if (actual + (0, 0))[:3] != (expected + (0, 0))[:3]:
        raise PilotError(f'{path}: unexpected macOS minimum (expected {macos_version(target)})')
    if stack_size is not None and (not stack or int(stack.group(1)) != stack_size):
        raise PilotError(f'{path}: unexpected main-thread stack (expected {stack_size})')
    return {'macos_minimum': minimum.group(1),
            'stack_size': int(stack.group(1)) if stack else None}
