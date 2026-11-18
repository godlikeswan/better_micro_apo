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

The effect list is ordered `Gain`, `Noise reduction`,
`Gate/Compressor/Limiter/Expander`, `Equalizer`, `Mono`,
`Saturation/Overdrive/Distortion`, and `Reverb`. Gain comes first with a default
of **+12 dB** and a range of **−24 to +50 dB**. The unified dynamics effect uses
one of two modes — **Compressor/Limiter** above the threshold or
**Expander/Gate** below it — with a −15 dB threshold, 5:1 ratio, input/output
trim, target-latching hold, and optional lookahead. Input ranges −24 to +30 dB.
Its ratio slider ends at **∞** (written as `inf` in the INI); the last finite
position is 1000.

Durable settings use schema version 7 and the runtime snapshot uses version 9.
Loading a settings file with any other `[dsp] version` deletes the file and
starts from defaults; there is no migration path. See
[DLL ↔ GUI settings sharing](docs/runtime-sharing.md) for the file formats and
the live-apply path.
