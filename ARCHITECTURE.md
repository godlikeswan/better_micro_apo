# Architecture

The architecture documentation has been split into smaller topic files. Start
at [docs/index.md](docs/index.md):

- [APO overview](docs/apo/overview.md) — COM identity, interfaces, formats, files.
- [Installation and registration](docs/apo/installation.md)
- [Device binding](docs/apo/device-binding.md) — registry keys and raw streams.
- [APO operation](docs/apo/operation.md) — lifecycle, buffers, DSP chain.
- [GUI overview](docs/gui/overview.md) — window, events, lifecycle, modules.
- [Views and layout](docs/gui/views.md)
- [Widgets and controls](docs/gui/widgets.md)
- [Theming and repainting](docs/gui/theming.md)
- [DLL ↔ GUI settings sharing](docs/runtime-sharing.md)
- [List of effects](docs/effects/index.md)

## Current effect schema

Every preset starts with a fixed `Input` row, followed by `Gain`, `Noise
reduction`, `Gate/Compressor/Limiter/Expander`, `Equalizer`, `Mono`,
`Saturation/Overdrive/Distortion`, and `Reverb` in the order added by the user.
Input is always first, enabled, and not addable or deletable; its gain defaults
to **0 dB** with a **−24 to +24 dB** range. Standalone Gain remains optional and
starts at **+12 dB** with a **−24 to +50 dB** range. The unified dynamics effect
uses one of two modes — **Compressor/Limiter** above the threshold or
**Expander/Gate** below it — with a −15 dB threshold, 5:1 ratio, input/output
trim, target-latching hold, and optional lookahead. Its ratio slider ends at
**∞** (written as `inf` in the INI); the last finite position is 1000.

Durable settings use schema version 8 and the runtime snapshot uses version 10.
Loading a settings file with any other `[dsp] version` deletes the file and
starts from defaults; there is no migration path. See
[DLL ↔ GUI settings sharing](docs/runtime-sharing.md) for the file formats, the
live-apply path, and the APO→GUI stage telemetry channel.
