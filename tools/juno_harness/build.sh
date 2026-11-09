#!/usr/bin/env bash
set -e

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$DIR"

echo "=== Building JUNO DSP WebAssembly Engine ==="
source ../emsdk/emsdk_env.sh

emcc main_wasm.cpp JunoDSP.cpp \
  -O3 -std=c++20 \
  -s WASM=1 \
  -s MODULARIZE=1 \
  -s EXPORT_NAME="JunoModule" \
  -s EXPORTED_FUNCTIONS="['_juno_init','_juno_reset','_juno_note_on','_juno_note_off','_juno_all_notes_off','_juno_set_param','_juno_get_param','_juno_active_voices','_juno_param_count','_juno_param_min','_juno_param_max','_juno_param_default','_juno_render','_malloc','_free']" \
  -s EXPORTED_RUNTIME_METHODS="['ccall','cwrap','HEAPF32']" \
  -o juno_dsp.js

echo "Build complete: juno_dsp.js and juno_dsp.wasm updated successfully!"
