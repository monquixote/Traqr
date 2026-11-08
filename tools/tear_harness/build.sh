#!/usr/bin/env bash
set -e

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$DIR"

echo "=== Building TEAR DSP WebAssembly Engine ==="
source ../emsdk/emsdk_env.sh

emcc main_wasm.cpp TearDSP.cpp \
  -O3 -std=c++20 \
  -s WASM=1 \
  -s MODULARIZE=1 \
  -s EXPORT_NAME="TearModule" \
  -s EXPORTED_FUNCTIONS="['_tear_init','_tear_reset','_tear_note_on','_tear_note_off','_tear_set_param','_tear_get_param','_tear_render','_tear_is_playing','_malloc','_free']" \
  -s EXPORTED_RUNTIME_METHODS="['ccall','cwrap','HEAPF32']" \
  -o tear_dsp.js

echo "Build complete: tear_dsp.js and tear_dsp.wasm updated successfully!"
