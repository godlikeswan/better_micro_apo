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
reduction`, `Gate/Expander`, `Compressor/Limiter`, `Equalizer`, `Mono`,
`Saturation/Overdrive/Distortion`, and `Reverb` in the order added by the user.
Input is always first, enabled, and not addable or deletable; its gain defaults
to **0 dB** with a **−24 to +24 dB** range. Standalone Gain remains optional and
starts at **+12 dB** with a **−24 to +50 dB** range. Gate/Expander and
Compressor/Limiter are separate effects with no mode selector: finite Ratio
expands below the threshold or compresses above it, while **∞** (written as
`inf` in the INI) is a hard gate or limiter. Both share the same parameter set —
−15 dB threshold, 5:1 ratio, input/output trim, target-latching hold, and
optional lookahead — and the last finite ratio position is 1000. Equalizer has
a signed static **Level** (default −12 dB, −96…+96 dB) and a **Dynamic**
selector of **No**, **Compressor**, or **Expander**, with a ratio that also ends
at ∞, a 0…96 dB **Range**, and the same hold and lookahead ranges as the
standalone dynamics effects.

Durable settings use schema version 10 and the runtime snapshot uses version 12.
Loading a settings file with any other `[dsp] version` deletes the file and
starts from defaults; there is no migration path. See
[DLL ↔ GUI settings sharing](docs/runtime-sharing.md) for the file formats, the
live-apply path, and the APO→GUI stage telemetry channel.
