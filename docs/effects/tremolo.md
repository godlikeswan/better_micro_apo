# Tremolo

Cycles the signal level up and down. Use it for a classic amplifier pulse or a
subtle breathing motion. It changes amplitude only: pitch, tone, and stereo
position are untouched.

## Try this

- In the Add effect menu, Tremolo sits after [Chorus](chorus.md) and before
  [Pitch Shifter](pitch-shifter.md). The list itself can still be reordered.
  Tremolo is a lightweight effect with up to 16 instances.
- Start with **7 Hz** Rate, **70%** Depth, and **Sine**. Lower Rate for a slow
  swell; raise it for a rapid flutter.
- **Depth** sets how far the level falls. **0%** leaves the audio untouched,
  while **100%** swings from unity down to silence. There is no wet **Level**
  control, so Depth is the only mix-like setting.
- **Waveform** chooses the LFO shape. Sine is smooth, Triangle is a little more
  mechanical, and Square switches between the two extremes. The gain is eased
  over about 2 ms so Square does not click.
- **Stereo spread** offsets the right channel's LFO by up to half a cycle. At
  **0%** both sides pulse together; higher values move the pulse between the
  speakers.
- Watch the bottom meters with high Depth: the loud part still reaches unity,
  and stacked effects can add gain elsewhere.
- Tremolo has controls but no live graph; the graph is hidden while it is
  selected.

## Controls

| Control | Default | Range | What it does |
| --- | --- | --- | --- |
| Rate | 7 Hz | 0.1 to 20 Hz | Speed of the level cycle. |
| Depth | 70% | 0 to 100% | How far the level drops; 0% bypasses. |
| Waveform | Sine | Sine, Triangle, Square | Shape of the level cycle. |
| Stereo spread | 0% | 0 to 100% | Right-channel LFO offset, up to half a cycle. |

## Under the hood

`TremoloStage` (`src/dsp/stages.cpp`):

- A phase accumulator runs in cycles at `Rate / sampleRate`. Sine, Triangle,
  and Square shapes share the implementation with Vibrato. Odd channels get the
  Stereo spread phase offset.
- Target gain is `1 - Depth * (0.5 - 0.5 * LFO)`, so a full sine reaches unity
  at the top and `1 - Depth` at the bottom. The target is followed by an 80 Hz
  one-pole smoother to soften waveform corners. Depth 0 holds the gain at
  unity, making the effect a pass-through.
- `Configure` allocates only small per-channel phase and gain state. `Update`
  recomputes coefficients, `Reset` restarts the LFO, and neither allocates.
  `NeedsSilence()` is false because no audio is buffered.
- There is no graph or stage telemetry for Tremolo; the bottom meters come from
  the post-DSP APO audio ring used by Listen.
- Settings use the `tremolo_rate_hz`, `tremolo_depth`, `tremolo_waveform`, and
  `tremolo_stereo_spread` keys. Settings schema version 14 added Tremolo and
  Vibrato; version 15 changed their defaults and moved mono copying to the
  [Input](input.md) row. A file with any other version is deleted rather than
  migrated.
