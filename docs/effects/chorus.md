# Chorus

Thickens the sound by mixing the dry signal with several slightly detuned,
slowly swept delay lines. It is useful for widening a doubled vocal or guitar;
subtle settings add movement, while high Depth and Rate become an obvious warble.

## Try this

- Put Chorus after cleanup and level effects. Check **Treat as mono** on the
  [Input](input.md) row if a mono source should feed both sides;
  [Reverb](reverb.md) and [Echo](echo.md) can come after so their tails stay
  clean.
- Start with **3 Voices** and talk or sing. More voices sound denser and cost
  more CPU, but the wet sum is normalized so changing Voices does not jump the
  overall level.
- **Rate** is the speed of the sweep. The default **0.6 Hz** is a gentle motion;
  small values drift slowly, and large values vibrate quickly.
- **Depth** sets how far each delay moves. **4 ms** is a natural vocal chorus;
  larger values sound more obviously detuned and add more variable delay.
- **Width** spreads the voice phases. **70%** is a wide but still coherent
  stereo image; at **0%** the voices move together, including on stereo streams.
- **Level** mixes the modulated voices over the dry signal, which stays at
  unity. Start at **40%** and lower it if the voice loses focus.
- Chorus buffers about 65 ms at the maximum settings. Check the bottom meters
  with **Listen to this device** and avoid **CLIP** if you stack several wet
  effects.
- A newly added Chorus uses 3 Voices, 0.6 Hz Rate, 4 ms Depth, 70% Width, and
  40% Level. The small ↺ buttons reset individual controls. At most 4 instances
  are supported.
- Chorus has controls but no live graph; the graph is hidden while it is selected.

## Controls

| Control | Default | Range | What it does |
| --- | --- | --- | --- |
| Voices | 3 | 2 to 10 | Number of modulated delay lines in the wet mix. |
| Rate | 0.6 Hz | 0.01 to 50 Hz | Speed of the delay-line modulation. |
| Depth | 4 ms | 0 to 50 ms | Peak-to-peak delay sweep of each voice. |
| Width | 70% | 0 to 100% | Spread of voice LFO phases and stereo decorrelation. |
| Level | 40% | 0 to 100% | Wet return level. Dry stays at unity. |

## Under the hood

`ChorusStage` (`src/dsp/stages.cpp`):

- `Configure` reserves ten voice buffers of `ceil(65 ms * sampleRate) + 8`
  frames per channel, plus small per-channel wet and input scratch arrays. A
  live edit only recomputes coefficients; no control change allocates. Enabling
  or changing Voices resets the buffers so a different voice count does not
  replay stale audio.
- Each voice reads its own circular buffer with linear interpolation. The
  center delay is 15 ms plus half the Depth, and a sine LFO moves it by up to
  half the Depth in either direction.
- Width offsets the LFO phases across the active voices and adds a quarter of
  that spread per channel, so higher values decorrelate left and right. Voice
  phases live in `Process`, which lets Rate, Depth, Width, and Level change live.
- The wet sum is divided by the active voice count before Level is applied, so
  Voices changes density without changing loudness. The dry signal is added at
  unity and is never delayed.
- `NeedsSilence()` is true while enabled with a nonzero Level, so the buffered
  tail is flushed through silent buffers. There is no graph or stage telemetry
  for Chorus.
- Settings use the `chorus_voices`, `chorus_rate_hz`, `chorus_depth_ms`,
  `chorus_width`, and `chorus_level` keys. Settings schema version 12 added
  Chorus; a file with any other version is deleted rather than migrated.
