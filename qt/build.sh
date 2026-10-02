#!/usr/bin/env bash
# Linux build helper: ./build.sh [Release|Debug] [--run [app args...]]
set -euo pipefail

cd "$(dirname "$0")"
config=Release
if [[ $# -gt 0 && "$1" != --* ]]; then
    config="$1"
    shift
fi

cmake -S . -B build -DCMAKE_BUILD_TYPE="$config"
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure

if [[ "${1:-}" == "--run" ]]; then
    shift
    exec ./build/rf-powermeter-assistant "$@"
fi
echo "Built ./build/rf-powermeter-assistant"
