# Agent Instructions

## Firmware Versioning (`version.mk`)

- **Always bump the firmware version before building a `.ptf` release** when any firmware source (`core/`, `platforms/`, `synth/`, `util/`) has changed, unless the user explicitly asks to keep the current version:
  - Patch bump (bug fixes, DSP tweaks, incremental changes):
    - `make bump-patch TARGET=MINI` (or `PLUS`, `ORIGINAL`, or `all`)
  - Minor bump (new features or major engine additions):
    - `make bump-minor TARGET=MINI` (or `PLUS`, `ORIGINAL`, or `all`)
- Check current versions with `make version`.
- Version numbers are centralized in `version.mk` (`MINI_VER_*`, `PLUS_VER_*`, `ORIGINAL_VER_*`).

## Firmware Builds

- Tracker Mini: `make mini` -> `build/TrackerMini_NL_<ver>.ptf`
- Tracker+: `make plus` -> `build/TrackerPlus_NL_<ver>.ptf`
- Original Tracker: `make original` -> `build/PolyendTracker_NL_<ver>.ptf`

## Shared DSP & WebAssembly Synth Harnesses

- `core/libs/Audio/WavetableCore.h` is shared between the Tracker firmware (`core/libs/Audio/play_memory_wavetable.cpp`) and the Wavetable WASM synth (`tools/wavetable_harness/`).
- When modifying shared DSP or web synth files under `tools/*_harness/`, rebuild and run the harness tests (e.g. `bash tools/wavetable_harness/build.sh && node tools/wavetable_harness/test_dsp.js`) and sync updated `index.html`, `*_dsp.js`, and `*_dsp.wasm` to `docs/synths/<synth>/`.
