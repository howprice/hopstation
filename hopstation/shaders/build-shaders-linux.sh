#!/bin/bash

# Adapted from https://github.com/TheSpydog/SDL_gpu_examples

# Exit immediately if a command exits with a non-zero status
set -e

# Allow script to be launched from any directory
SOURCE=${BASH_SOURCE[0]}
SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )
#echo "SOURCE is '$SOURCE'"
#echo "SCRIPT_DIR is '$SCRIPT_DIR"
pushd $SCRIPT_DIR > /dev/null

SHADERCROSS=../../bin/Linux/shadercross
BIN=../../data/shaders

if [ ! -f "$SHADERCROSS" ]; then
    echo "ERROR: shadercross not found at $SHADERCROSS"
    popd > /dev/null
    exit 1
fi

# Create output directory if it doesn't exist
mkdir -p "$BIN"

# Clean output directory to avoid loading stale shaders
rm -f "$BIN"/*

# Infer shader stage from filename

for filename in *.vert.hlsl; do
    if [ -f "$filename" ]; then
        echo "Compiling $filename to ${filename/.hlsl/.spv}"
        ${SHADERCROSS} "$filename" -s hlsl -d spirv -t vertex -e "main" -o "${BIN}/${filename/.hlsl/.spv}"
    fi
done

for filename in *.frag.hlsl; do
    if [ -f "$filename" ]; then
        echo "Compiling $filename to ${filename/.hlsl/.spv}"
        ${SHADERCROSS} "$filename" -s hlsl -d spirv -t fragment -e "main" -o "${BIN}/${filename/.hlsl/.spv}"
    fi
done

# return to original directory
popd > /dev/null
