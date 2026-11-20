# Pitch Shifter

Moves the signal up or down in pitch without changing its playback speed. One
effect covers octave shifts and smaller intervals; **Dry level** scales the
unshifted signal and **Level** sets the shifted return. Both default to keeping
the dry voice at unity, so the shifted part is added on top.

## Try this

- Use **Shift** to choose the interval in semitones. **−12** is one octave down
  and is the default; **+12** is one octave up. The full range is **−24** to
  **+24** semitones, and fractional values are allowed.
- Set **Level** to **0%** to bypass the shifted return. Set **Dry level** to
  **0%** when you want only the shifted voice. Both mix into the same output, so
  watch the bottom meters and avoid **CLIP** when they are high.
- **Algorithm** selects the engine:
  - **Granular** is the default. It crossfades two overlapping grains from the
    sample history and is a good general choice for voice and instruments.
  - **WSOLA** adds a bounded waveform-similarity search at each grain restart.
    It can reduce splice artifacts on sustained material, at extra CPU cost.
  - **Phase vocoder** uses FFT phase propagation. It suits sustained tones and
    larger shifts, but has a longer analysis window. Detected partials are
    spread across neighboring bins and synthesis phase is re-locked when a bin
    becomes active, which reduces clicks when a partial crosses a bin boundary.
- **Window** is the analysis or grain length in samples. **4096** is about 85 ms
  at 48 kHz. Smaller values react faster but can sound more granular; larger
  values are steadier and add more latency.
- Granular and WSOLA use the exact Window value. Phase vocoder rounds it up to
  the next power of two so it can use a radix-2 FFT.
- Pitch Shifter buffers on the order of its Window, so it adds noticeable
  latency. Put it where that delay is acceptable, usually after cleanup and
  before or after other creative effects.
- A newly added Pitch Shifter uses Granular, −12 semitones, a 4096-sample
  Window, 50% Level, and 100% Dry level. The small ↺ buttons reset individual
  controls. At most 4 instances are supported.
- Pitch Shifter has controls but no live graph; the graph is hidden while it is
  selected.

## Controls

| Control | Default | Range | What it does |
| --- | --- | --- | --- |
| Algorithm | Granular | Granular, WSOLA, Phase vocoder | Selects the time-stretch/pitch-shift engine. |
| Shift | −12 semitones | −24 to +24 semitones | Pitch interval; negative is down, positive is up, fractions allowed. |
| Window | 4096 samples | 512 to 16384 samples | Grain or analysis length; Phase vocoder rounds up to a power of two. |
| Level | 50% | 0 to 100% | Shifted return level. |
| Dry level | 100% | 0 to 100% | Unshifted signal level. 100% preserves the original dry voice. |

## Under the hood

`PitchShifterStage` (`src/dsp/stages.cpp`):

- The shift ratio is `2^(semitones / 12)`. Granular and WSOLA advance two
  read heads through a per-channel sample history at that ratio and crossfade
  them with Hann windows. Each grain lasts the full Window and the hop is half
  the Window. When the older grain ends, it restarts one hop ahead of the
  still-active grain in input time; that jump supplies the `(1 - ratio)`
  samples skipped each hop, so the read lag stays near one to two Windows
  instead of walking to the end of the history. `Configure` reserves
  `6 * 16384 + 8` history samples per channel so any Window value fits.
- **WSOLA** searches ±48 samples around that nominal grain restart and compares
  up to 256 samples against the active grain. The best-aligned restart is
  clamped to the same bounded lag range, so the search stays allocation-free.
- **Phase vocoder** keeps a per-channel input ring, overlap-add ring, separate
  analysis and synthesis phase arrays, and several per-bin accumulators. Every
  `FFT / 4` samples a Hann-windowed FFT estimates each bin's true frequency
  from its phase advance. Each magnitude is split between the two nearest bins
  around `trueBin * ratio` with linear weights, instead of being rounded into
  one bin; that removes the level jump when a partial crosses a bin boundary.
  Per target bin it also accumulates a magnitude-weighted target frequency and
  a weighted analysis phase. Synthesis phase advances by that weighted target
  frequency at the normal hop, and re-locks to the weighted analysis phase when
  a previously empty bin becomes active, so a new bin does not start at an
  arbitrary phase. Energy that would land above Nyquist is dropped.
  `FftPlan::Inverse` already divides by the FFT size, so one 0.5 overlap-add
  scale keeps the wet level stable. Radix-2 plans and Hann windows for sizes
  512 through 16384 are built in `Configure`; changing Window only selects a
  plan. The plan index is relative to 512, so a 4096-sample Window selects
  plan 3 rather than an out-of-range index.
- Enabling, changing Algorithm, or changing Window resets the histories to avoid
  replaying old audio through a different engine. Shift, Level, and Dry level
  apply live; `Update` and `Process` never allocate. The dry sample is multiplied
  by Dry level and the engine's wet sample by Level before they are summed.
- `NeedsSilence()` is true while enabled with a nonzero Level, so buffered
  grains and vocoder overlap are flushed through silent buffers. There is no
  graph or stage telemetry for Pitch Shifter; the dry path is scaled by Dry level
  rather than filtered.
- Settings use the `pitch_algorithm`, `pitch_shift_semitones`,
  `pitch_window_samples`, `pitch_level`, and `pitch_dry_level` keys. Settings
  schema version 12 added Pitch Shifter; version 13 added Dry level, and version
  14 added Tremolo and Vibrato. A file with any other version is deleted rather
  than migrated.
