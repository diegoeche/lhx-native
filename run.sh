#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"
if [[ ! -f build/cmake/CMakeCache.txt ]]; then
  if [[ ! -f bin/LHX.iso ]]; then
    printf '%s\n' 'Place your own supported Hit Squad disc image at bin/LHX.iso, then run ./run.sh again.' >&2
    printf '%s\n' 'See README.md for the supported release and build requirements.' >&2
    exit 2
  fi
  cmake -S . -B build/cmake
fi
cmake --build build/cmake --parallel 4
exec ./build/cmake/lhx-native --data-dir "$PWD/bin/dos" "$@"
