#!/usr/bin/env bash
set -e

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$DIR"

echo "=== Building Wavetable DSP WebAssembly Engine ==="
source ../emsdk/emsdk_env.sh

em++ main_wasm.cpp WavetableDSP.cpp \
  -O3 -std=c++20 -fno-exceptions \
  -s WASM=1 \
  -s MODULARIZE=1 \
  -s EXPORT_NAME="WtModule" \
  -s ALLOW_MEMORY_GROWTH=1 \
  -s EXPORTED_FUNCTIONS="['_wt_init','_wt_reset','_wt_note_on','_wt_note_off','_wt_all_notes_off','_wt_set_param','_wt_get_param','_wt_active_voices','_wt_param_count','_wt_param_min','_wt_param_max','_wt_param_default','_wt_render','_wt_load_pcm16','_wt_get_window_size','_wt_get_window_count','_wt_get_total_samples','_wt_get_display_wave','_wt_get_env_preview','_wt_export_pti_size','_wt_export_pti','_wt_import_pti','_malloc','_free']" \
  -s EXPORTED_RUNTIME_METHODS="['ccall','cwrap','HEAPF32','HEAP16','HEAPU8']" \
  -o wt_dsp.js

echo "Build complete: wt_dsp.js and wt_dsp.wasm updated successfully!"
