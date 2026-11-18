#-------------------------------------------------------------------------------------------------------
# Copyright (C) Microsoft. All rights reserved.
# Copyright (c) ChakraCore Project Contributors. All rights reserved.
# Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
#-------------------------------------------------------------------------------------------------------
"""Unit tests for tools/test262/run.py pattern selection and --intl402 opt-in."""

from pathlib import Path
import sys
import unittest

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import run  # noqa: E402


class TestRunPatterns(unittest.TestCase):
    def test_default_patterns_exclude_intl402_by_default(self):
        args = run.build_parser().parse_args(["--binary", "zig-out/bin/ch"])
        self.assertFalse(args.intl402)
        patterns = run.resolve_patterns(args.patterns, args.intl402)
        self.assertEqual(
            patterns,
            [
                "test/language/**/*.js",
                "test/built-ins/**/*.js",
                "test/annexB/**/*.js",
            ],
        )
        self.assertNotIn("test/intl402/**/*.js", patterns)

    def test_intl402_flag_appends_to_default_patterns(self):
        args = run.build_parser().parse_args(["--binary", "zig-out/bin/ch", "--intl402"])
        self.assertTrue(args.intl402)
        patterns = run.resolve_patterns(args.patterns, args.intl402)
        self.assertEqual(
            patterns,
            [
                "test/language/**/*.js",
                "test/built-ins/**/*.js",
                "test/annexB/**/*.js",
                "test/intl402/**/*.js",
            ],
        )

    def test_explicit_patterns_override_defaults_without_duplication(self):
        args_plain = run.build_parser().parse_args(
            ["--binary", "zig-out/bin/ch", "test/intl402/Locale/**/*.js"]
        )
        self.assertEqual(
            run.resolve_patterns(args_plain.patterns, args_plain.intl402),
            ["test/intl402/Locale/**/*.js"],
        )

        args_flag = run.build_parser().parse_args(
            ["--binary", "zig-out/bin/ch", "--intl402", "test/intl402/**/*.js"]
        )
        self.assertEqual(
            run.resolve_patterns(args_flag.patterns, args_flag.intl402),
            ["test/intl402/**/*.js"],
        )


if __name__ == "__main__":
    unittest.main()
