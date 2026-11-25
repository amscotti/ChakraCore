#!/bin/bash
#-------------------------------------------------------------------------------------------------------
# Copyright (C) Microsoft. All rights reserved.
# Copyright (c) ChakraCore Project Contributors. All rights reserved.
# Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
#-------------------------------------------------------------------------------------------------------

CH_DIR=$1
BUILD_TYPE=$2
CC=$3
CXX=$4
ROOT_DIR=$5
RES=

echo ""
echo "########## Running POSIX Native Tests ###########"
echo "Using CC [${CC}]"
echo "Using CXX [${CXX}]"
echo ""

SAFE_RUN() {
    local SF_OUTPUT
    SF_OUTPUT=$(eval "( $1 )" 2>&1)
    if [[ $? != 0 ]]; then
        >&2 echo "$SF_OUTPUT"
        exit 1
    fi
}

TEST () {
    if [[ $RES =~ $1 ]]; then
        echo "${TEST_PATH} : PASS"
    else
        echo "${TEST_PATH} FAILED"
        echo -e "$RES"
        exit 1
    fi
}

RUN () {
    TEST_PATH=$1
    echo "Testing $TEST_PATH"
    SAFE_RUN "cd $TEST_PATH && ${CH_DIR} Platform.js -args ${ROOT_DIR} > Makefile"
    RES=$(cd $TEST_PATH; cat Makefile)

    if [[ $RES =~ "# IGNORE_THIS_TEST" ]]; then
        echo "Ignoring $TEST_PATH"
    else
        SAFE_RUN "cd $TEST_PATH && make CC=\"${CC}\" CXX=\"${CXX}\""
        RES=$(cd "$TEST_PATH" && ./sample.o)
        local sample_status=$?
        if [[ $sample_status != 0 ]]; then
            echo "${TEST_PATH} FAILED"
            echo "sample.o exited ${sample_status}"
            echo -e "$RES"
            exit 1
        fi
        TEST "SUCCESS"
        SAFE_RUN "cd $TEST_PATH && rm -rf ./sample.o"
    fi
}

RUN_CMD () {
    TEST_PATH=$1
    CMD=$2
    echo "Testing $TEST_PATH"
    SAFE_RUN "cd $TEST_PATH && $CMD"
}

# static lib tests
tests=$(ls | tr "\t" " ")

for item in ${tests[*]}
do
    if [[ $item =~ "test-static-" ]]; then
        RUN $item
    fi
done

# shared lib tests
LIB_DIR="$(dirname ${CH_DIR})"
EXTENSION=""
if [[ `uname -a` =~ "Darwin" ]]; then
    export DYLD_LIBRARY_PATH=${LIB_DIR}/:$DYLD_LIBRARY_PATH
    EXTENSION="dylib"
else
    export LD_LIBRARY_PATH=${LIB_DIR}/:$LD_LIBRARY_PATH
    EXTENSION="so"
fi

RUN_SHARED_SINGLE_CPP () {
    local SRC_FILE=$1
    TEST_PATH=$2
    if [[ -f "${SRC_FILE}" && -f "test-shared-basic/Makefile" ]] && ! grep -q "# IGNORE_THIS_TEST" "test-shared-basic/Makefile"; then
        echo "Testing ${TEST_PATH}"
        local OBJ_FILE="${TEST_PATH}.o"
        local MK_FILE="Makefile.${TEST_PATH}"
        printf "include Makefile\n%s:\n\t\$(CC) ../%s \$(CFLAGS) -std=c++11 \$(FORCE_STARTS) \$(LDIR) \$(FORCE_ENDS) \$(filter-out -o sample.o,\$(LIBS)) -o %s\n" \
            "${TEST_PATH}" "${SRC_FILE}" "${OBJ_FILE}" > "test-shared-basic/${MK_FILE}"
        SAFE_RUN "cd test-shared-basic && make -f ${MK_FILE} CC=\"${CC}\" CXX=\"${CXX}\" ${TEST_PATH}"
        RES=$(cd test-shared-basic && ./${OBJ_FILE})
        local cpp_status=$?
        if [[ $cpp_status != 0 ]]; then
            echo "${TEST_PATH} FAILED"
            echo "${OBJ_FILE} exited ${cpp_status}"
            echo -e "$RES"
            exit 1
        fi
        TEST "PASS"
        SAFE_RUN "cd test-shared-basic && rm -rf ./${OBJ_FILE} ./${MK_FILE}"
    fi
}

RUN "test-shared-basic"
RUN_SHARED_SINGLE_CPP "test-timer-cancel.cpp" "test-timer-cancel"
RUN_SHARED_SINGLE_CPP "test-detach-arraybuffer.cpp" "test-detach-arraybuffer"

# test python
#RUN_CMD "test-python" "python helloWorld.py ${BUILD_TYPE} ${CH_DIR%/*}/libChakraCore.${EXTENSION}"

SAFE_RUN "rm -rf Makefile"
