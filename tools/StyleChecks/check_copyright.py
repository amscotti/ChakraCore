#-------------------------------------------------------------------------------------------------------
# Copyright (C) Microsoft. All rights reserved.
# Copyright (c) ChakraCore Project Contributors. All rights reserved.
# Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
#-------------------------------------------------------------------------------------------------------

# Python 2.7 and 3.x compatibility for easier testing regardless of python installation
from __future__ import print_function

import sys
import os.path
import re

copyright_lines = [
    r'-------------------------------------------------------------------------------------------------------',
    r' Copyright \(C\) Microsoft( Corporation and contributors)?\. All rights reserved\.',
    r' Copyright \(c\)( 2022)? ChakraCore Project Contributors\. All rights reserved\.',
    r' Licensed under the MIT license\. See LICENSE\.txt file in the project root for full license information\.',
    r'.*' # the above should always be followed by at least one other line, so make sure that line is present
]

# New contributor-owned files need not claim Microsoft ownership. Keep the
# existing combined and PAL notices valid while accepting the same license form.
project_copyright_lines = [
    r'-------------------------------------------------------------------------------------------------------',
    r' Copyright \(c\)( \d{4})? ChakraCore Project Contributors\. All rights reserved\.',
    r' Licensed under the MIT license\. See LICENSE\.txt file in the project root for full license information\.',
    r'.*'
]

pal_copyright_lines = [
    r'-------------------------------------------------------------------------------------------------------',
    r' ChakraCore/Pal',
    r' Contains portions \(c\) copyright Microsoft, portions copyright \(c\) the \.NET Foundation and Contributors',
    r' and edits \(c\) copyright the ChakraCore Contributors\.',
    r' See THIRD-PARTY-NOTICES\.txt in the project root for \.NET Foundation license',
    r' Licensed under the MIT license\. See LICENSE\.txt file in the project root for full license information\.',
    r'.*' # the above should always be followed by at least one other line, so make sure that line is present
]

if len(sys.argv) < 2:
    print("Requires passing a filename as an argument.")
    exit(1)

file_name = sys.argv[1]
if not os.path.isfile(file_name):
    print("File does not exist:", file_name, "(not necessarily an error)")
    exit(0)

notices = [pal_copyright_lines] if file_name[:4] == "pal/" else [copyright_lines, project_copyright_lines]

def report_incorrect(file_name, pairs, fail_line):
    # found a problem so report the problem to the caller and exit
    print(file_name, "... does not contain a correct copyright notice.\n")
    # print the relevant lines to help the reader find the problem
    for (_, line) in pairs:
        print("    ", line, end="")
    print()
    if (fail_line != ""):
        print("Match failed at line:")
        print("    ", fail_line, end="")
        print()

with open(file_name, 'r') as sourcefile:
    hashbang = sourcefile.readline()
    if not hashbang.startswith("#!"):
        sourcefile.seek(0)
    lines = [sourcefile.readline() for _ in range(max(len(notice) for notice in notices))]

if not any(lines):
    exit(0)
for notice in notices:
    regexes = [re.compile('^.{1,5}%s$' % line) for line in notice]
    pairs = list(zip(regexes, lines))
    if all(line and regex.match(line.rstrip()) for regex, line in pairs):
        exit(0)

report_incorrect(file_name, pairs, next((line for regex, line in pairs
                                       if not line or not regex.match(line.rstrip())), ""))
exit(1)
