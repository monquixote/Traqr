# Traqr (Polyend Tracker Community Edition) - Documentation & Wiki

Welcome to the documentation for **Traqr** (Polyend Tracker Community Edition), covering the **Original Polyend Tracker** (`MK66`), **Polyend Tracker+** and **Polyend Tracker Mini** (`IMX`).

* **[Interactive Web DSP Hub](https://monquixote.github.io/Traqr/)** - Real-time WASM web interfaces for JUNO and TEAR synth engines.
* **[Release Changelog (`CHANGELOG.md`)](../CHANGELOG.md)** - Full list of new features, improvements, and fixes in each release.
* **[Latest Releases on GitHub](https://github.com/monquixote/Traqr/releases)** - Download `.ptf` firmware builds and preset archives.

---

## Synth Engines & Performance

* **`JUNO` Vintage Analog Synthesizer**: Virtual analog DCO architecture, non-resonant HPF with +3.5dB bass boost, 4-pole OTA IR3109 zero-delay feedback resonant LPF, dual-BBD stereo chorus, and analog component drift modeling.
  * [Live Web Interface](https://monquixote.github.io/Traqr/synths/juno/)
* **`TEAR` Sync Synthesizer**: 5 sync modes (Hard Tear, Soft Reverse, Windowed Formant, Dual SuperTear, Ring Shred), morphable slave shape, dedicated sync envelope/LFO, and 5 saturation circuits.
  * [Live Web Interface](https://monquixote.github.io/Traqr/synths/tear/)
* **Synth Performance Dashboard**: Dedicated live tracking screen for Macros 1–6, Delay/Reverb sends, Volume, Pan, polyphony counts, and real-time pattern FX automation tracking.

---

## Musician & User Guides

* **[Custom Themes & Fonts (`docs/custom_themes_and_fonts.md`)](custom_themes_and_fonts.md)**
  * How to install and create custom JSON color themes (`/Themes/*.json`) and custom antialiased bitmap fonts (`/Fonts/*.ptfont`) using the [Web Theme & Font Editor](https://polyend.github.io/tracker-theme-editor/) ([`polyend/tracker-theme-editor`](https://github.com/polyend/tracker-theme-editor)), plus full color key reference tables.
* **[Wavetable Envelopes, LFOs & Step FX (`docs/wavetable_envelopes.md`)](wavetable_envelopes.md)**
  * Complete guide to the 256-shape `WT Env` and `WT LFO` instrument modulators, dual-axis `Warp X/Y` shaping, `w` / `y` pattern step FX, and loading custom `.wav` contours from `/Envelopes/`.
* **[Modular Sample Editor Effects - User & Architecture Guide (`docs/modular_effects_system.md`)](modular_effects_system.md)**
  * Overview of hot-loadable `.ptfx` Sample Editor effects and generators (`/Effects/*.ptfx`), bundled reference plugins, and hardware memory/cache architecture.
* **[SD Card Image Viewer (`docs/image_viewer.md`)](image_viewer.md)**
  * How to prepare and view up to 48 baseline JPEGs (`800 × 390` px) per folder in `/Images/Reference/`, `/Images/Notes/`, and `/Images/Custom/` under `Config > Info`.

---

## Developer & SDK Documentation

* **[Firmware Architecture, Hardware Limits & Developer Gotchas (`docs/developer_guide.md`)](developer_guide.md)**
  * Hardware memory budgets (`FLASH`, `SRAM_DTC`, `SRAM_ITC`, `EXTERNAL_RAM`, plus theoretically reclaimable `SAMPLE_MEMORY_SIZE` headroom), the shared `768 KB` scratch buffer layout (`externalRamScratchBuff`), the Copy-on-Write `/Workspace/instruments/` fast project loader, Hybrid SD Card Sample Streaming (`VoiceStreamManager`), FT812 display/bootloader constraints, and platform preprocessor guards (`PLATFORM_IMX`, `TRACKER_MINI`, `GUI_HANDLE_SYNTH`).
* **[Custom Synth Engine SDK (`docs/custom_synth_sdk.md`)](custom_synth_sdk.md)**
  * Step-by-step C++20 developer tutorial and API reference (`synth/include/PolySynth/sdk/`) for building and registering new polyphonic synthesizer engines on **Tracker+** and **Tracker Mini**, including a walkthrough of the open-source **`CHIP`** (`ChiptuneSynthVoice`) engine.
* **[Modular Sample Editor Effects & `.ptfx` Developer SDK (`docs/modular_effects_system.md`)](modular_effects_system.md)**
  * Complete C/C++ developer guide (`editor-fx-sdk/`), parameter/host API reference, 472-byte `PtfxHeader` binary spec, and firmware cache/buffer architecture for `.ptfx` Sample Editor effects and generators across all 3 Tracker devices.
* **[Repository README - Build Instructions & PR Guidelines (`README.md`)](../README.md)**
  * Top-level build instructions, flashing steps, project philosophy, and contribution rules.

---

## Polyend Tracker Open-Source Ecosystem

* **[`monquixote/Traqr`](https://github.com/monquixote/Traqr)** - Traqr (Polyend Tracker Community Edition) firmware, Synth Engines, and .ptfx SDK.
* **[`polyend/tracker-ce`](https://github.com/polyend/tracker-ce)** - Upstream Polyend Tracker Community Edition repository.
* **[`polyend/tracker-theme-editor`](https://github.com/polyend/tracker-theme-editor)** ([Live Web App](https://polyend.github.io/tracker-theme-editor/)) - Visual browser-based Theme Editor (`/Themes/*.json`) and Font Editor (TTF/OTF to `/Fonts/*.ptfont`).
* **[`polyend/tracker-lib`](https://github.com/polyend/tracker-lib)** - Open-source library for reading, writing, and inspecting Polyend Tracker project (`.mt`), pattern (`.mtp`), and instrument (`.pti`) files.
* **[Polyend Backstage Forum](https://backstage.polyend.com/)** - Community discussions, custom themes, fonts, and plugins.
* **[The Tracker Overflow (Discord)](https://discord.com/invite/uEPTnwckJV)** - Unofficial Polyend Tracker Discord where users and developers can join and find like-minded people.