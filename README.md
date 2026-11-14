# Traqr (Polyend Tracker Community Edition)

Welcome to **Traqr** — an enhanced, community-driven edition of the firmware for the **Polyend Tracker**, **Tracker Mini**, and **Tracker+**.

Built directly from the original Polyend Tracker firmware source code, **Traqr** adds custom polyphonic synthesizer engines, a dedicated real-time Synth Performance dashboard, a global Master Stereo Chorus send effect, an upgraded Wavetable engine (Sub-Oscillator, Warp modes with independent Envelope/LFO modulation, and de-clicked window stepping), heavily optimized DSP with control-rate decimation, and browser-based WebAssembly synth interfaces, while maintaining complete project compatibility and respecting existing muscle memory.

---

### Download Firmware & Releases

* **Latest Releases & Assets**: [GitHub Releases](https://github.com/monquixote/Traqr/releases)
* **Latest Binary (`.ptf`) Downloads**:
  * [Tracker Mini Firmware (`TrackerMini_NL_*.ptf`)](https://github.com/monquixote/Traqr/releases/latest)
  * [Tracker+ Firmware (`TrackerPlus_NL_*.ptf`)](https://github.com/monquixote/Traqr/releases/latest)
  * [Original Tracker Firmware (`PolyendTracker_NL_*.ptf`)](https://github.com/monquixote/Traqr/releases/latest)
  * [TEAR Patches & Presets (`TEAR_Patches.zip`)](https://github.com/monquixote/Traqr/releases/latest)
  * [JULYO Patches & Presets (`JULYO_Patches.zip`)](https://github.com/monquixote/Traqr/releases/latest)
  * [Sample Editor Effects (`PolyendTracker_CE_Effects.zip`)](https://github.com/monquixote/Traqr/releases/latest)
* **Traqr User Guide (How to Use All Features)**: [Step-by-Step Feature Manual](https://monquixote.github.io/Traqr/guide.html)
* **Interactive Web Synth Hub**: [Traqr Web DSP Hub](https://monquixote.github.io/Traqr/)
  * [JULYO Web Interface & Preset Player](https://monquixote.github.io/Traqr/synths/julyo/)
  * [TEAR Web Interface & Preset Player](https://monquixote.github.io/Traqr/synths/tear/)

---

## What's New in Traqr

### 1. `JULYO` Synth Engine
A classic mid-1980s Japanese 60/106-style virtual-analog polyphonic DCO synthesizer voice — inspired by the iconic Roman-goddess polysynths (single-DCO saw/pulse/sub architecture, 4-pole OTA filter, +3.5dB bass-boost HPF, and I / II / I+II BBD stereo chorus):
* **True DCO Architecture**: Simultaneous Sawtooth, Pulse Width Modulation (manual, LFO, and dedicated envelope modulation), Sub-oscillator (-1 / -2 octaves), and Noise generator.
* **Analog HPF with Bass Boost**: Non-resonant high-pass filter with a +3.5dB low-shelf bass boost circuit at position 0.
* **4-Pole OTA Resonant Low-Pass Filter**: Zero-delay feedback 4-pole OTA ladder filter with self-oscillation and drive.
* **Dual-BBD Stereo Chorus**: Bucket-brigade device (BBD) stereo chorus emulation featuring Mode I, Mode II, and combined Mode I+II.
* **Analog Voice Tolerances & Thermal Wander**: Subtle per-voice component tolerances and leaky random-walk thermal wander modeling vintage hardware drift.
* **Factory Preset Library**: Includes 40 classic analog poly factory patches (Strings, Brass, Hoover, Reso Bass, Plucks, Chorus Poly, and more).

### 2. `TEAR` Synth Engine
An aggressive, cutting oscillator sync synthesizer:
* **5 Unique Sync Modes**: Hard Tear, Soft Reverse, Windowed Formant, Dual SuperTear, and Ring Shred.
* **Morphable Slave Oscillator**: Continuous blending from Sawtooth through Pulse into Dual-Saw.
* **Dedicated Modulators**: Dedicated Sync Decay Envelope and Sync LFO for expressive tearing sweeps.
* **Zero-Delay Feedback Filters & 5 Saturation Models**: 24dB Ladder and 12dB Warm SVF filters paired with Modern Soft, Classic Hard, Tube/Valve, Tape, and Transformer saturation circuits.
* **Factory Preset Library**: Acid screamer leads, aggressive reese basses, tear sync growls, crystalline plucks, and ambient drones.

### 3. Dedicated Synth Performance Screen (Optimised for Circuit Tracks)
A live performance dashboard designed for hands-on control over your synth tracks, with an 8-parameter layout specifically optimised for the **Novation Circuit Tracks**:
* **Optimised for Circuit Tracks Workflow**: Perfectly mirrors the 8-macro layout of the Circuit Tracks (Macros 1–6 + Delay Send + Reverb Send), giving you an intuitive, tactile live performance surface.
* **Macro Performance Controls**: Real-time visualization and tweaking of Macros 1–6 (with automatic unipolar and bipolar ranges and custom names from patch metadata).
* **Live Send & Mixer Controls**: Dedicated sliders and readouts for Delay Send dB and Reverb Send dB, alongside Track Volume and Panning.
* **Polyphony & RMS VU Meters**: Active voice counter and stereo RMS level meters per synth part.
* **Full Pattern FX Live Tracking**: Parameter automation in pattern FX slots (`FX_TYPE_SEND_CC_A`–`F`, Delay, Reverb, Volume) immediately animates the performance bars, value text, and bottom button labels in real time.

### 4. Advanced DSP Performance Optimizations
* **Control-Rate Decimation (`kCtl = 16`, ~2.75 kHz)**: Reduces transcendental math calls (`fastPow2`, `fastTan`) by over **93%**, completely eliminating CPU overflows and audio dropouts during heavy polyphonic playback.
* **Division-Free PolyBLEP**: Precomputed inverse delta phases eliminate hardware floating-point division from anti-aliasing calculations in the inner audio loop.
* **Shared BLEP Residuals & Bitmasked Buffers**: Optimizes waveform generation and replaces modulo operations with single-cycle bitwise masking in delay lines.

### 5. Enhanced Wavetable Engine (Sub-Oscillator, Warp Modes & De-Clicked Synthesis)
* **Phase-Locked Sub-Oscillator**: Per-instrument sub-oscillator in `Wavetable` Sample Playback with selectable `Off`, `Sine`, `Tri`, and `Square` waveforms and `0–100%` mix level (defaults to `20%` when enabled).
* **Oscillator Warp Modes & Simultaneous Modulation**: Per-instrument warp modes (`Off`, `Asym`, `Sync`, `Fold`) with `0–100%` warp depth (defaults to `50%` when enabled), plus a dedicated **`Wavetable Warp`** modulation target in Instrument Parameters so Wavetable Position and Warp can be modulated simultaneously by Envelopes and LFOs.
* **De-Clicked Window Stepping, Seam Fix & ITCM Headroom**: 32-sample de-clicked window-stepping crossfade, 32-sample note-onset declicker, cyclic single-cycle seam wrap bug fix, and ITCM RAM execution for `updateWavetable` — keeping 6+ track headroom while retaining the exact `372`-byte `strInstrument` layout for 100% Polyend project compatibility.

### 6. Master Stereo Chorus Send Effect
* **BBD-Style Global Stereo Chorus Send**: Shared stereo chorus (`AudioEffectChorus`) wired as a global send effect alongside Delay and Reverb for sample-based instruments, wavetable instruments, and Line In (Synth tracks excluded).
* **Per-Instrument Send & Master Chorus Menu**: Per-instrument `Chorus send` control on the Instrument Effects `More` page, plus a `Chorus` button on **Master 1/3** opening the **Master Chorus** subscreen (`Rate` `0.10–5.00 Hz`, `Depth` `0–100%`, `Width` `0–100%`, `Space` `0–8` moved inside, `Line in` send level with Shift+Press Mute, and `Back`).
* **Global Mixer 3/3 Return Channel**: Dedicated stereo `Chorus` return fader with L/R RMS metering, Mute, and Solo.

---

## Notice: Official Support, Warranty & Hardware Disclaimer

> **A Quick Note Before Flashing or Modifying Firmware**
>
> Thanks to the Tracker's isolated bootloader and hardware emergency recovery button (see [Flashing on Hardware & Emergency Recovery](#4-flashing-on-hardware--emergency-recovery)), experimenting with Community Edition builds is very safe. Even if a custom build fails to boot, you can easily re-flash a stable `.ptf` image from your microSD card. That said, we still need to include the standard legal disclaimer just in case:
>
> * **Provided "AS IS" Without Support Obligation**: While the foundation of this repository was originally developed and released by **Polyend**, the **Polyend Tracker Community Edition (CE)** firmware, source code, and any community-developed modifications or plugins are provided **"AS IS"**, without warranty of any kind, express or implied. **Polyend has no obligation to provide official technical support, maintenance, bug fixes, or customer service** for Community Edition firmware or custom builds.
> * **Use at Your Own Risk**: Flashing, modifying, or running custom firmware is done at your own discretion and risk. Polyend is not liable for any data loss, project corruption, malfunction, or hardware damage (including "bricking" a device or issues caused by code modifications) arising from the use of this software.
> * **Out-of-Warranty Repairs Available**: In the unlikely event that something ever goes wrong with your hardware while using custom firmware, it is **not covered under standard warranty**. However, **Polyend is always happy to help get your instrument back up and running through its official out-of-warranty repair service** (standard service, parts, and shipping fees apply. See [How to Contact Polyend Customer Support](https://backstage.polyend.com/t/how-to-contact-the-polyend-customer-support/15510)).

---

## Repository Structure

This unified codebase powers all three hardware instruments in the Tracker family:
* **Original Polyend Tracker** (`NXP Kinetis MK66FX1M0`, Cortex-M4F @ 180 MHz)
* **Polyend Tracker+ & Tracker Mini** (`NXP i.MX RT1051/RT1064`, Cortex-M7 @ 600 MHz)


```text
.
├── Makefile                # Unified top-level build & version management
├── version.mk              # Centralized version & build number configuration
├── CHANGELOG.md            # Stable release history & feature highlights
├── UPCOMING.md             # Unreleased changes currently in the beta builds
├── CONTRIBUTING.md         # Contribution rules & hardware testing workflow
├── LICENSE                 # MIT License (+ No Wholesale Commercial Clones & Precompiled Synth Terms)
├── NOTICE.md               # Third-party open-source attributions
├── core/                   # Shared Tracker application, audio engine, UI & open synth engines
├── platforms/
│   ├── mk66/               # Original Tracker (Kinetis MK66) HAL, drivers & linker scripts
│   └── imx/                # Tracker+ & Tracker Mini (i.MX RT1051) HAL, drivers & linker scripts
├── synth/
│   ├── include/PolySynth/  # PolySynth public headers & Open Synth SDK (synth/include/PolySynth/sdk/)
│   └── lib/                # Precompiled libPolySynth.a & libPolyDSP.a + LICENSE-PROPRIETARY.txt
├── util/                   # Shared C++ utility headers & third-party header libraries (ETL, magic_enum)
├── editor-fx-sdk/          # .ptfx Sample Editor plugin SDK, builder & effects/ sources
└── docs/                   # User guides, SDK tutorials & architecture documentation
```

---

## Guiding Principles of the Community Edition Firmware

This repository prioritizes workflow continuity: our aim is to respect existing muscle memory, introduce features, optimizations and improvements that fit the original vision of the device, while fixing bugs along the way.

If you have ideas for major workflow adjustments or structural changes, please open a [GitHub Discussion](https://github.com/polyend/tracker-ce/discussions) or a topic on the [Polyend Backstage Forum](https://backstage.polyend.com/) first, so we can align on direction together with the community.

For radical experiments or reimagined interfaces, we actively encourage community forks!

---

## Contributing

We welcome community contributions! ❤️

From bug fixes and workflow improvements to new open-source synth engines and `.ptfx` Sample Editor plugins. Please read [`CONTRIBUTING.md`](CONTRIBUTING.md) for our project philosophy, pull request rules, hardware testing workflow, and technical checklist before opening a Pull Request.

---

## Building Firmware

### 1. Prerequisites
* **[GNU Arm Embedded Toolchain `10.3-2021.10`](https://developer.arm.com/downloads/-/gnu-rm)**:
  * Direct official downloads from Arm:
    * **Linux (`x86_64`)**: [`gcc-arm-none-eabi-10.3-2021.10-x86_64-linux.tar.bz2`](https://developer.arm.com/-/media/Files/downloads/gnu-rm/10.3-2021.10/gcc-arm-none-eabi-10.3-2021.10-x86_64-linux.tar.bz2)
    * **Linux (`aarch64` / ARM64)**: [`gcc-arm-none-eabi-10.3-2021.10-aarch64-linux.tar.bz2`](https://developer.arm.com/-/media/Files/downloads/gnu-rm/10.3-2021.10/gcc-arm-none-eabi-10.3-2021.10-aarch64-linux.tar.bz2)
    * **macOS (`x86_64` / Apple Silicon via Rosetta)**: [`gcc-arm-none-eabi-10.3-2021.10-mac.tar.bz2`](https://developer.arm.com/-/media/Files/downloads/gnu-rm/10.3-2021.10/gcc-arm-none-eabi-10.3-2021.10-mac.tar.bz2)
    * **Windows (ZIP)**: [`gcc-arm-none-eabi-10.3-2021.10-win32.zip`](https://developer.arm.com/-/media/Files/downloads/gnu-rm/10.3-2021.10/gcc-arm-none-eabi-10.3-2021.10-win32.zip)
  * Ensure `arm-none-eabi-gcc` is on your `PATH`, extract the toolchain into `./agt/10.3/`, or pass `TOOLCHAIN=/path/to/arm-none-eabi-` to `make`.
* `make` (for building firmware) and `python3` (used by `make ptfx` to package `.ptfx` Sample Editor plugins and by the `make bump-*` version helpers; standard library only).

### 2. Build Commands
From the repository root:

```bash
# Build all three devices (Original Tracker, Tracker+, Tracker Mini)
make all

# Or build a single target:
make original   # -> build/PolyendTracker_CE_1.0.0.ptf
make plus       # -> build/TrackerPlus_CE_1.0.0.ptf
make mini       # -> build/TrackerMini_CE_1.0.0.ptf

# Build reference .ptfx Sample Editor effects into editor-fx-sdk/build/Effects/:
make ptfx

# Clean all build directories:
make clean
```

### 3. Version Management (`version.mk`)
All firmware version numbers are defined in [`version.mk`](version.mk). The **Build** identifier displayed on the hardware under **Config > Firmware > Build** is automatically populated from the short Git commit hash (`git rev-parse --short=7 HEAD`):

```bash
# Show current version, Git build hash & output filenames
make version

# Increment patch version (e.g. 1.0.0 -> 1.0.1)
make bump-patch TARGET=all        # or TARGET=original | TARGET=plus | TARGET=mini

# Increment minor version (e.g. 1.0.x -> 1.1.0)
make bump-minor TARGET=all        # or TARGET=original | TARGET=plus | TARGET=mini
```

### 4. Flashing on Hardware & Emergency Recovery

1. Copy the generated `.ptf` file from `build/` (`PolyendTracker_*.ptf`, `TrackerPlus_*.ptf`, or `TrackerMini_*.ptf`) to the `/Firmware/` folder on your Tracker's microSD card.
   * *Note*: Keep `.ptf` filenames at **31 characters or fewer** so the Original Tracker's 32-byte `/Firmware/_fwinfo` bootloader buffer remains null-terminated.
2. On the device, open **Config > Firmware > Firmware update**, select the `.ptf` file, and confirm.

> **Bootloader Resilience & Emergency Firmware Recovery**:
> * The Polyend Tracker bootloader is isolated from the application firmware:
>   * On the **Original Tracker (`MK66`)**, the bootloader runs on a separate dedicated microcontroller (`kBootloaderTrig`).
>   * On **Tracker+ and Tracker Mini (`IMX`)**, the bootloader resides in a protected region at the bottom of QSPI Flash (`0x60000000`–`0x6002FFFF`), while `.ptf` application builds are linked starting at `0x60030000` (do not modify the flash `ORIGIN` address in the linker scripts).
> * **Emergency Recovery**: If a custom firmware build ever hangs or fails to boot into the UI, you can always recover your device:
>   1. Place a known-good, stable `.ptf` firmware file for your device inside the `/Firmware/` folder on your microSD card (ideally as the only `.ptf` file in `/Firmware/`).
>   2. Insert the microSD card into your Tracker and power it on.
>   3. Use a paperclip or SIM-eject pin to press the **recessed hardware Reset / Emergency Bootloader pinhole button** (located on the bottom of the Original Tracker / Tracker+, or next to the USB-C port on Tracker Mini). The hardware bootloader will automatically pick up the `.ptf` file from `/Firmware/` and flash it.

---

## Documentation & Developer Guides

Full user and developer documentation is available in [`docs/`](docs/Home.md) and on the repository **Wiki**:

* **User Guides**:
  * [Custom Themes & Fonts](docs/custom_themes_and_fonts.md) + [Live Web Editor](https://polyend.github.io/tracker-theme-editor/) ([`polyend/tracker-theme-editor`](https://github.com/polyend/tracker-theme-editor))
  * [Wavetable Envelopes, LFOs & Step FX](docs/wavetable_envelopes.md)
  * [SD Card Image Viewer](docs/image_viewer.md)
* **Developer & Architecture Guides**:
  * [Firmware Architecture, Hardware Limits & Developer Gotchas](docs/developer_guide.md)
  * [Custom Synth Engine SDK](docs/custom_synth_sdk.md)
  * [Modular Sample Editor Effects & `.ptfx` Developer SDK](docs/modular_effects_system.md)

---

## Polyend Tracker Open-Source Ecosystem

This repository is part of a growing open-source ecosystem around the Polyend Tracker family:

* **[`polyend/tracker-theme-editor`](https://github.com/polyend/tracker-theme-editor)** ([Live Web App](https://polyend.github.io/tracker-theme-editor/)): Browser-based visual **Theme & Font Editor** with a hardware screen simulator.
* **[`polyend/tracker-lib`](https://github.com/polyend/tracker-lib)**: Open-source library and tools for reading, writing, and inspecting Polyend Tracker project (`.mt`), pattern (`.mtp`), and instrument (`.pti`) files.
* **[Polyend Backstage Forum](https://backstage.polyend.com/)**: Community forum for sharing custom themes, fonts, `.ptfx` plugins, synth engines, and discussing workflows.
* **[The Tracker Overflow (Discord)](https://discord.com/invite/uEPTnwckJV)**: Unofficial Polyend Tracker Discord where users and developers can join, chat, and find like-minded people.

---

## License

* **Source Code, SDKs, Open Synth Engines (`core/synth/engines/`), `.ptfx` Sample Editor SDK (`editor-fx-sdk/`), Drivers & Utilities**: Licensed under the **MIT License with a Wholesale Commercial Clone Restriction** (see [`LICENSE`](LICENSE)).
  * **Musicians & Creators**: Using the firmware for commercial music production, albums, paid live performances/gigs, sample packs, and monetized streaming/YouTube content is **expressly permitted**.
  * **Developers & Plugin Authors**: Reusing individual source files, drivers, DSP algorithms, UI widgets, or open-source C++ synth engines (`CHIP`, etc.) in your own open-source or commercial projects—and selling `.ptfx` Sample Editor plugins—is **expressly permitted** under the MIT License.
  * **No Wholesale Commercial Clones**: You may **not** sell compiled `.ptf` firmware builds, manufacture/sell commercial clone hardware running the Tracker firmware, or sell a direct port/wholesale clone of the complete Tracker application as a paid software product.
* **Precompiled Synth & DSP Libraries (`synth/lib/libPolySynth.a`, `synth/lib/libPolyDSP.a`)**: Provided under the [Polyend Precompiled Synthesizer & DSP Library License](synth/lib/LICENSE-PROPRIETARY.txt), permitting free use on authentic Polyend hardware as well as personal/DIY non-commercial projects (commercial embedding of the precompiled `.a` binaries is prohibited).
* **Third-Party Components**: See [`NOTICE.md`](NOTICE.md).
