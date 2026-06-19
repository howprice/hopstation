#!/bin/bash

# Exit immediately if a command exits with a non-zero status
set -e

# Allow script to be launched from any directory
SOURCE=${BASH_SOURCE[0]}
SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )
#echo "SOURCE is '$SOURCE'"
#echo "SCRIPT_DIR is '$SCRIPT_DIR"
pushd $SCRIPT_DIR/.. > /dev/null

# Detect if Ninja is available and set the generator accordingly
if command -v ninja &> /dev/null; then
    CMAKE_GENERATOR="Ninja"
    echo "Using Ninja generator (faster builds)"
else
    CMAKE_GENERATOR="Unix Makefiles"
    echo "Using Unix Makefiles generator (Ninja not found)"
fi

echo "Building psx-test x86_64 Debug"
cmake -S . -B build/x86_64/Debug -G "$CMAKE_GENERATOR" -DCMAKE_BUILD_TYPE:STRING=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS:BOOL=TRUE -DCMAKE_C_COMPILER:FILEPATH=/usr/bin/clang -DCMAKE_CXX_COMPILER:FILEPATH=/usr/bin/clang++ -DCMAKE_OSX_ARCHITECTURES=x86_64 -DCMAKE_OSX_DEPLOYMENT_TARGET="11"
cmake --build build/x86_64/Debug --config Debug --target all

echo "Building psx-test x86_64 Release"
cmake -S . -B build/x86_64/Release -G "$CMAKE_GENERATOR" -DCMAKE_BUILD_TYPE:STRING=Release -DCMAKE_EXPORT_COMPILE_COMMANDS:BOOL=TRUE -DCMAKE_C_COMPILER:FILEPATH=/usr/bin/clang -DCMAKE_CXX_COMPILER:FILEPATH=/usr/bin/clang++ -DCMAKE_OSX_ARCHITECTURES=x86_64 -DCMAKE_OSX_DEPLOYMENT_TARGET="11"
cmake --build build/x86_64/Release --config Release --target all

echo "Building psx-test arm64 Debug"
cmake -S . -B build/arm64/Debug -G "$CMAKE_GENERATOR" -DCMAKE_BUILD_TYPE:STRING=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS:BOOL=TRUE -DCMAKE_C_COMPILER:FILEPATH=/usr/bin/clang -DCMAKE_CXX_COMPILER:FILEPATH=/usr/bin/clang++ -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET="11"
cmake --build build/arm64/Debug --config Debug --target all

echo "Building psx-test arm64 Release"
cmake -S . -B build/arm64/Release -G "$CMAKE_GENERATOR" -DCMAKE_BUILD_TYPE:STRING=Release -DCMAKE_EXPORT_COMPILE_COMMANDS:BOOL=TRUE -DCMAKE_C_COMPILER:FILEPATH=/usr/bin/clang -DCMAKE_CXX_COMPILER:FILEPATH=/usr/bin/clang++ -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET="11"
cmake --build build/arm64/Release --config Release --target all

# return to original directory
popd > /dev/null
