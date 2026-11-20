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
reduction`, `Gate/Expander`, `Compressor/Limiter`, `Equalizer`,
`Saturation/Overdrive/Distortion`, `Reverb`, `Echo`, `Chorus`, `Tremolo`,
`Vibrato`, `Pitch Shifter`, and `Tuner` in type/menu order. The list itself is
processed from top to bottom
in the order the user adds or moves effects. Tuner is monitoring-only: it
publishes detected pitch for its graph without changing samples, adding gain, or
buffering audio.
Input is always first, enabled, and not addable or deletable; its gain defaults
to **0 dB** with a **−24 to +24 dB** range, and its **Treat as mono** checkbox
copies channel 0 to every other channel at the Input stage before the addable
effects run. Standalone Gain remains optional and starts at **+12 dB** with a
**−24 to +50 dB** range. Gate/Expander and
Compressor/Limiter are separate effects with no mode selector: finite Ratio
expands below the threshold or compresses above it, while **∞** (written as
`inf` in the INI) is a hard gate or limiter. Both share the same parameter set —
−15 dB threshold, 5:1 ratio, input/output trim, target-latching hold, and
optional lookahead — and the last finite ratio position is 1000. Equalizer has
a signed static **Level** (default −12 dB, −96…+96 dB) and a **Dynamic**
selector of **No**, **Compressor**, or **Expander**, with a ratio that also ends
at ∞, a 0…96 dB **Range**, and the same hold and lookahead ranges as the
standalone dynamics effects.

Echo adds damped **Stereo** or **Ping-pong** repeats with a 5–1200 ms **Time**
(default 300 ms), 0–98% **Feedback** (default 35%), 0–100% **Damping** (default
40%), and a wet-only **Level** (default 25%). Chorus mixes 2–10 voices (default
3) with a 0.01–50 Hz **Rate** (default 0.6 Hz), 0–50 ms **Depth** (default 4
ms), 0–100% **Width** (default 70%), and **Level** (default 40%). The lightweight
Tremolo and Vibrato follow Chorus: both have a 0.1–20 Hz **Rate** (default 7 Hz),
**Sine**, **Triangle**, or **Square** waveform, and up to 100% **Stereo spread**;
Tremolo's **Depth** is 0–100% (default 70%) and Vibrato's is 0–300 cents (default
100). Depth zero bypasses both. They allow up to 16 instances each, within the
overall 16-row list limit. The single Pitch Shifter covers −24…+24 semitones
(default −12) with **Granular**, **WSOLA**, or **Phase vocoder**, a 512–16384
sample **Window** (default 4096), **Level** (default 50%) for the shifted return,
and **Dry level** (default 100%) for the unshifted signal. Phase vocoder rounds
Window up to a power of two; the grain engines use the exact sample count. None
of these five has a live graph.

Durable settings use schema version 15, the runtime snapshot uses version 17,
and the stage-monitor channel uses version 8.
Loading a settings file with any other `[dsp] version` deletes the file and
starts from defaults; there is no migration path. See
[DLL ↔ GUI settings sharing](docs/runtime-sharing.md) for the file formats, the
live-apply path, and the APO→GUI stage telemetry channel.
