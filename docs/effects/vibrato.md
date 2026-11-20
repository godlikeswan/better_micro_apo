# Vibrato

Moves the pitch up and down by cycling a fractional delay line. It changes
pitch only: level, tone, and stereo position are untouched. Unlike
[Pitch Shifter](pitch-shifter.md), the shift is periodic rather than a fixed
interval.

## Try this

- In the Add effect menu, Vibrato sits after [Tremolo](tremolo.md) and before
  [Pitch Shifter](pitch-shifter.md). The list itself can still be reordered.
  Vibrato is a lightweight effect with up to 16 instances.
- Start with **7 Hz** Rate, **100 cents** Depth, and **Sine**. Small depths sound
  like natural vocal or guitar vibrato; larger depths become an obvious wobble.
- **Depth** is the peak pitch excursion in cents, not a wet mix. **0** cents
  passes the current sample through unchanged; **300** cents reaches three
  semitones at the waveform's extremes.
- **Waveform** chooses the LFO shape. Sine is the smooth default, Triangle is a
  little more mechanical, and Square switches between the two pitch extremes.
- **Stereo spread** offsets the right channel's LFO by up to half a cycle. At
  **0%** both sides move together; higher values make the pitch motion move
  between the speakers.
- While active, Vibrato keeps a center delay so the read head can move in both
  directions. It buffers about **1.7 seconds** at 48 kHz at the maximum Depth
  and slowest Rate, so extreme settings add substantial latency. At 0 cents the
  current sample is passed straight through instead.
- Vibrato has controls but no live graph; the graph is hidden while it is
  selected.

## Controls

| Control | Default | Range | What it does |
| --- | --- | --- | --- |
| Rate | 7 Hz | 0.1 to 20 Hz | Speed of the pitch cycle. |
| Depth | 100 cents | 0 to 300 cents | Peak pitch excursion; 0 passes through. |
| Waveform | Sine | Sine, Triangle, Square | Shape of the pitch cycle. |
| Stereo spread | 0% | 0 to 100% | Right-channel LFO offset, up to half a cycle. |

## Under the hood

`VibratoStage` (`src/dsp/stages.cpp`):

- `Configure` sizes one circular buffer per channel for the deepest Depth at
  the slowest Rate, about 83,000 frames at 48 kHz, and chooses a center read
  position near its midpoint. A live Rate or Depth change only retunes the LFO;
  it never allocates.
- The stage integrates the requested pitch ratio into the read position rather
  than smoothing a delay target. Square holds `+Depth` cents for one part of
  each cycle and `−Depth` cents for the rest; the duty cycle is adjusted so the
  average pitch is unchanged. Sine and Triangle use a centred ratio with the
  same zero average.
- The fractional read position moves continuously, so a Square transition does
  not jump the delay or click. Odd channels get the Stereo spread phase offset.
  At 0 cents the ring is still kept current, but the output is the unmodified
  input.
- `NeedsSilence()` is true while enabled with a nonzero Depth so the delay line
  flushes through silent buffers. There is no graph or stage telemetry for
  Vibrato.
- Settings use the `vibrato_rate_hz`, `vibrato_depth_cents`,
  `vibrato_waveform`, and `vibrato_stereo_spread` keys. Settings schema version
  14 added Tremolo and Vibrato; version 15 changed their defaults and moved mono
  copying to the [Input](input.md) row. A file with any other version is deleted
  rather than migrated.
