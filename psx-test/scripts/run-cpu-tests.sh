#!/bin/bash

# Get the directory where the script is located
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Change to the script directory
pushd "$SCRIPT_DIR" > /dev/null

echo
echo "Building all configurations (GCC and Clang, Debug and Release)"
./build.sh
if [ $? -ne 0 ]; then
    echo "Build failed"
    popd > /dev/null
    exit 1
fi

# Change to data directory
cd ../../data

# Amidog has drawn final pixel and starts accessing joypad at PC 0x80013a24
SAVE_VRAM_PC=0x80013a24

TEST_ARGS="--exe test/amidog/psxtest_cpu.exe --amidog-cpu-test --save-vram-pc $SAVE_VRAM_PC "

echo
echo "Running Amidog CPU tests (GCC Debug)"
../psx-test/build/gcc/debug/psx-test $TEST_ARGS
if [ $? -ne 0 ]; then
    echo "Amidog CPU tests (GCC Debug) FAILED"
    popd > /dev/null
    exit 1
fi
echo "Amidog CPU tests (GCC Debug) passed."

echo
echo "Running Amidog CPU tests (GCC Release)"
../psx-test/build/gcc/release/psx-test $TEST_ARGS
if [ $? -ne 0 ]; then
    echo "Amidog CPU tests (GCC Release) FAILED"
    popd > /dev/null
    exit 1
fi
echo "Amidog CPU tests (GCC Release) passed."

echo
echo "Running Amidog CPU tests (Clang Debug)"
../psx-test/build/clang/debug/psx-test $TEST_ARGS
if [ $? -ne 0 ]; then
    echo "Amidog CPU tests (Clang Debug) FAILED"
    popd > /dev/null
    exit 1
fi
echo "Amidog CPU tests (Clang Debug) passed."

echo
echo "Running Amidog CPU tests (Clang Release)"
../psx-test/build/clang/release/psx-test $TEST_ARGS
if [ $? -ne 0 ]; then
    echo "Amidog CPU tests (Clang Release) FAILED"
    popd > /dev/null
    exit 1
fi
echo "Amidog CPU tests (Clang Release) passed."

popd > /dev/null

exit 0
