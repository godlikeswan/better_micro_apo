# Dynamic (proximity)

Tames the boomy low end you get when a microphone is close to your mouth, or
fills it back in when the mic is farther away. It only works on a band around a
chosen frequency, so the rest of your voice is left alone.

## Try this

- Put Dynamic before [Compressor](compressor.md), while the level still has its
  natural movement.
- If your voice sounds chesty or boomy close to the mic, leave **Extend quiet
  lows** off (the default reduce mode). Talk normally and move **Threshold** to
  the right until the low-end thumps start getting held back. Lower **Range** if
  the sound gets thin.
- If the voice sounds small and distant, turn **Extend quiet lows** on. Now the
  effect lifts quiet lows instead. Raise **Range** a little at a time until the
  voice has some body without sounding muddy.
- **Frequency** sets which low band is watched; 120 Hz is a useful starting
  point for a close voice. If the boom is higher up (around 200–300 Hz), move
  the slider there. **Q** controls how wide the band is. A low Q around 0.7 acts
  like a gentle shelf; a higher Q targets a narrower problem.
- **Ratio**, **Attack**, and **Release** work like the compressor controls, but
  only on that band. Keep **Ratio** moderate and use a slower **Release** if the
  low end pumps between words.
- A newly added Dynamic starts in reduce mode at 120 Hz, Q 0.7, and a −45 dBFS
  threshold. The little ↺ buttons reset individual controls. Up to 16 instances
  are supported.

## Controls

| Control | Default | Range | What it does |
| --- | --- | --- | --- |
| Extend quiet lows | Off | On / off | Switches from cutting loud lows to lifting quiet lows. |
| Frequency | 120 Hz | 20 to 2000 Hz | Center of the isolated band. |
| Q | 0.7 | 0.1 to 10 | Bandwidth; low Q is broad. |
| Threshold | −45 dBFS | −80 to 0 dBFS | Level where the dynamics start acting. |
| Ratio | 4:1 | 1:1 to 20:1 | Slope applied above/below the threshold. |
| Range | 18 dB | 0 to 36 dB | Maximum cut or boost. |
| Attack | 10 ms | 0.1 to 1000 ms | Gain smoothing when the effect deepens. |
| Release | 150 ms | 1 to 5000 ms | Gain smoothing when it returns to zero. |

## Under the hood

`DynamicStage` (`src/dsp/stages.cpp`):

- A topology-preserving state-variable bandpass (Simper) isolates the band.
  Damping `k = 1/Q`, and the bandpass output is scaled by `k` to normalize its
  peak to unity at the center frequency. The frequency is limited to 49% of the
  sample rate.
- The band peak is linked across channels, then smoothed with a fast detector
  envelope (1 ms attack, 100 ms release) so the gain does not pulse at zero
  crossings.
- Reduce mode computes a negative target from the level above the threshold;
  extend mode computes a positive target from the level below it. Both are
  limited by `rangeDb` and by the ratio slope, then smoothed with the configured
  attack/release.
- The resulting gain is added back to the band only: `out = x + band *
  (linearGain - 1)`. The rest of the signal is untouched.
- Per-channel filter state (`ic1`, `ic2`, and the band value) is allocated and
  cleared in `Configure`/`Reset`. Changing enable state, frequency, or Q resets
  the filter. `Process` and `Update` do not allocate. The graph shows the analog
  band-pass prototype the filter approximates.
