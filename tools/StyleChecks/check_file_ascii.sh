#-------------------------------------------------------------------------------------------------------
# Copyright (C) Microsoft. All rights reserved.
# Copyright (c) 2021 ChakraCore Project Contributors. All rights reserved.
# Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
#-------------------------------------------------------------------------------------------------------

ERRFILE=check_ascii.sh.err

python3 tools/StyleChecks/check_style_portable.py --mode ascii-file --errfile "$ERRFILE" "$1"
