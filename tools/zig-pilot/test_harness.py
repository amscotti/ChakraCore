#!/usr/bin/env python3
#-------------------------------------------------------------------------------------------------------
# Copyright (c) ChakraCore Project Contributors. All rights reserved.
# Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
#-------------------------------------------------------------------------------------------------------
"""Exercise false-success and stale-artifact boundaries with real Ninja edges."""
import json
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest

import pilot

HERE = Path(__file__).resolve().parent
FAKE_ZIG = '''#!/usr/bin/env python3
import json
from pathlib import Path
import sys
args = sys.argv[1:]
if args == ['version']:
    print('0.17.0-test')
    sys.exit(0)
if args == ['env']:
    print('test toolchain environment')
    sys.exit(0)
args = args[1:]
expanded = []
for arg in args:
    if arg.startswith('@'):
        import shlex
        expanded.extend(shlex.split(Path(arg[1:]).read_text()))
    else:
        expanded.append(arg)
args = expanded
if '--no-output' in args:
    sys.exit(0)
output = Path(args[args.index('-o') + 1])
if '-c' in args:
    source = Path(args[args.index('-c') + 1])
    if source.name.startswith('fail'):
        print('controlled compiler failure', file=sys.stderr)
        sys.exit(1)
    if source.name.startswith('noop'):
        sys.exit(0)
    output.write_text(json.dumps({'source': str(source), 'flags': args}))
    depfile = Path(args[args.index('-MF') + 1])
    paths = [source, source.parent / 'header with spaces.h']
    escape = lambda p: str(p).replace('\\\\', '\\\\\\\\').replace(' ', '\\\\ ').replace('$', '$$')
    depfile.write_text('tu.o: ' + ' '.join(escape(p) for p in paths) + '\\n')
else:
    output.write_text(json.dumps({'link_args': args}))
'''


@unittest.skipUnless(shutil.which('ninja'), 'Ninja is required for harness integration checks')
class HarnessTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='zig-pilot-test-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.build = self.root / 'build with spaces'
        self.build.mkdir()
        self.tool = self.root / 'tool with spaces'
        self.tool.mkdir()
        self.zig = self.tool / 'zig'
        self.zig.write_text(FAKE_ZIG)
        self.zig.chmod(0o755)
        (self.build / 'header with spaces.h').write_text('initial header\n')
        self.configure(('a', 'b'))

    def configure(self, names, command=None):
        entries = []
        for name in names:
            source = self.build / f'{name}.c'
            source.write_text('int example;\n')
            entries.append({'directory': str(self.build), 'file': str(source),
                            'arguments': ['/usr/bin/clang', '-c', str(source), '-o', f'{name}.o']})
        (self.build / 'compile_commands.json').write_text(json.dumps(entries))
        command = command or '/usr/bin/clang++ ' + ' '.join(f'{name}.o' for name in names) + ' -o program'
        (self.build / 'build.ninja').write_text(f'rule link\n  command = {command}\nbuild program: link\n')

    def run_tool(self, script, *args):
        return subprocess.run([sys.executable, str(HERE / script), str(self.build),
                               str(self.tool), *args], capture_output=True, text=True, timeout=30)

    def replay(self, *args):
        return self.run_tool('replay.py', '--target', 'x86_64-linux-gnu', '--jobs', '1', *args)

    def manifest(self):
        return json.loads((self.build / 'zigpilot-obj' / pilot.MANIFEST).read_text())

    def complete_replay(self):
        result = self.replay()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.manifest()['state'], 'complete')

    def test_successful_compound_edge_and_space_paths(self):
        self.configure(('a', 'b'), ': && /usr/bin/clang++ a.o b.o -o program && '
                       f'{shlex.quote(sys.executable)} -c "print(\'&&\')"')
        self.complete_replay()
        result = self.run_tool('link.py', 'program')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        output = json.loads((self.build / 'program').read_text())
        twins = [arg for arg in output['link_args'] if arg.endswith('.o')]
        self.assertEqual(len(twins), 2)
        self.assertTrue(all('zigpilot-obj' in arg and Path(arg).is_file() for arg in twins))
        self.assertIn('LINK-PILOT-OK', result.stdout)

    def test_unknown_target_fails_without_touching_previous_output(self):
        self.complete_replay()
        output = self.build / 'program'
        output.write_text('existing output')
        result = self.run_tool('link.py', '__missing_target__')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('could not enumerate', result.stderr)
        self.assertNotIn('LINK-PILOT-OK', result.stdout)
        self.assertEqual(output.read_text(), 'existing output')

    def test_recent_output_cannot_hide_noop_link(self):
        self.configure(('a',), ': -o program')
        self.complete_replay()
        (self.build / 'program').write_text('recent output')
        result = self.run_tool('link.py', 'program')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('missing=', result.stderr)
        self.assertFalse((self.build / 'program').exists())

    def test_compiler_rc_zero_without_object_is_failure(self):
        self.configure(('noop',))
        result = self.replay()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('without a nonempty object', result.stdout)
        self.assertEqual(self.manifest()['state'], 'failed')

    def test_partial_replay_invalidates_complete_generation(self):
        self.complete_replay()
        old = self.manifest()
        result = self.replay('--limit', '1', '--extra-flags=-DVALUE=2')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        current = self.manifest()
        self.assertNotEqual(old['generation'], current['generation'])
        self.assertEqual(current['state'], 'partial')
        result = self.run_tool('link.py', 'program')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('incomplete', result.stderr)

    def test_fail_fast_counts_only_work_that_ran(self):
        self.configure(('fail-a', 'fail-b', 'fail-c'))
        result = self.replay('--fail-fast')
        self.assertNotEqual(result.returncode, 0)
        manifest = self.manifest()
        self.assertEqual((manifest['passed'], manifest['failed'], manifest['not_run']), (0, 1, 2))
        self.assertIn('0/3 compiled; failed=1, not-run=2', result.stdout)

    def test_changed_source_or_header_rejects_objects(self):
        for filename in ('a.c', 'header with spaces.h'):
            with self.subTest(filename=filename):
                self.complete_replay()
                (self.build / filename).write_text('changed input\n')
                result = self.run_tool('link.py', 'program')
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('input(s) changed', result.stderr)

    def test_database_reordering_rejects_index_mapping(self):
        self.complete_replay()
        path = self.build / 'compile_commands.json'
        path.write_text(json.dumps(list(reversed(json.loads(path.read_text())))))
        result = self.run_tool('link.py', 'program')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('database changed', result.stderr)

    def test_changed_or_missing_object_is_rejected(self):
        self.complete_replay()
        twin = Path(self.manifest()['objects'][0]['path'])
        twin.write_text('tampered object')
        result = self.run_tool('link.py', 'program')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('object is missing, empty, or changed', result.stderr)
        twin.unlink()
        self.assertNotEqual(self.run_tool('link.py', 'program').returncode, 0)

    def test_changed_toolchain_rejects_objects(self):
        self.complete_replay()
        self.zig.write_text(FAKE_ZIG + '\n# changed toolchain\n')
        result = self.run_tool('link.py', 'program')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('Zig executable/version/environment changed', result.stderr)

    def test_response_objects_are_rewritten_but_rpath_is_preserved(self):
        self.configure(('a', 'b'), '/usr/bin/clang++ @objects.rsp -o program')
        (self.build / 'objects.rsp').write_text('a.o b.o -install_name @rpath/library.dylib')
        self.complete_replay()
        result = self.run_tool('link.py', 'program')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        output = json.loads((self.build / 'program').read_text())['link_args']
        self.assertIn('@rpath/library.dylib', output)
        self.assertEqual(sum('zigpilot-obj' in arg and arg.endswith('.o') for arg in output), 2)

    def test_extra_flags_preserve_quoted_argument(self):
        result = self.replay('--extra-flags=-DNAME="one two"')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        artifact = json.loads(Path(self.manifest()['objects'][0]['path']).read_text())
        self.assertIn('-DNAME=one two', artifact['flags'])

    def test_empty_target_and_missing_response_are_not_success(self):
        self.complete_replay()
        (self.build / 'build.ninja').write_text('build empty: phony\n')
        result = self.run_tool('link.py', 'empty')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('no output-producing', result.stderr)
        self.configure(('a', 'b'), '/usr/bin/clang++ @missing.rsp -o program')
        self.complete_replay()
        result = self.run_tool('link.py', 'program')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('response file is missing', result.stderr)

    def test_concurrent_generation_is_rejected(self):
        self.complete_replay()
        with pilot.build_lock(self.build):
            result = self.replay()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('another replay/link', result.stderr)

    def test_extra_flags_cannot_override_target_or_sdk(self):
        result = self.replay('--extra-flags=-target x86_64-linux-musl')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('must not override', result.stderr)
        self.complete_replay()
        result = self.run_tool('link.py', 'program', '--extra-link-flags=--sysroot=/different-sdk')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('must not override', result.stderr)

    def test_unknown_linker_cannot_use_reference_objects(self):
        self.configure(('a',), f'{shlex.quote(sys.executable)} unknown-linker.py a.o -o program')
        self.complete_replay()
        (self.build / 'program').write_text('existing output')
        result = self.run_tool('link.py', 'program')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('supported compiler/archive driver', result.stderr)
        self.assertEqual((self.build / 'program').read_text(), 'existing output')


if __name__ == '__main__':
    unittest.main()
