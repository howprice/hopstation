#!/bin/bash

# Get the script's directory and navigate to repo root
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

# Change to data directory and run hopstation
cd "$REPO_ROOT/data"
# "$@" passes all command-line arguments to hopstation (e.g., ./run.sh arg1 arg2)
exec "$REPO_ROOT/hopstation/build/clang/debug/hopstation" "$@"
