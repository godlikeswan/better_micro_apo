# Dynamic (proximity)

A frequency-selective compressor/expander aimed at the microphone proximity
effect. It isolates a band around a chosen frequency and either cuts it when it
is loud (reduce mode) or lifts it when it is quiet (extend mode). A broad Q
behaves like a dynamic low shelf.

Typical placement: before the broadband compressor so it sees the input
dynamics. Up to 16 instances are supported.

## Parameters

| Parameter | Default | Clamp | Unit / meaning |
| --- | --- | --- | --- |
| Mode | reduce | reduce / extend | Cut loud lows, or boost quiet lows. |
| Frequency | 120 Hz | 20–2000 Hz | Center of the isolated band. |
| Q | 0.7 | 0.1–10 | Bandwidth; low Q is broad. |
| Threshold | −45 dBFS | −80 to 0 dBFS | Level where the dynamics start acting. |
| Ratio | 4:1 | 1:1 to 20:1 | Slope applied above/below the threshold. |
| Range | 18 dB | 0–36 dB | Maximum cut or boost. |
| Attack | 10 ms | 0.1–1000 ms | Gain smoothing when the effect deepens. |
| Release | 150 ms | 1–5000 ms | Gain smoothing when it returns to zero. |

Newly added instances are enabled by `MakeEffect`; the default mode is reduce.

## Implementation details

`DynamicStage` (`src/dsp/stages.cpp`):

- A topology-preserving state-variable bandpass (Simper) isolates the band.
  Damping `k = 1/Q`, and the bandpass output is scaled by `k` to normalize its
  peak to unity at the center frequency. The frequency is limited to 49 % of
  the sample rate.
- The band peak is linked across channels, then smoothed with a fast detector
  envelope (1 ms attack, 100 ms release) so the gain does not pulse at zero
  crossings.
- Reduce mode computes a negative target from the level above the threshold;
  extend mode computes a positive target from the level below it. Both are
  limited by `rangeDb` and by the ratio slope, then smoothed with the configured
  attack/release.
- The resulting gain is added back to the band only:
  `out = x + band * (linearGain - 1)`. The rest of the signal is untouched.
- Per-channel filter state (`ic1`, `ic2`, and the band value) is allocated and
  cleared in `Configure`/`Reset`. Changing enable state, frequency, or Q resets
  the filter. `Process` and `Update` do not allocate.

## GUI notes

The graph shows the analog band-pass prototype that the state-variable filter
approximates, so the effect of moving Frequency or Q is visible. The Mode
selector switches between reducing a strong close-mic proximity effect and
filling out thin, distant lows.
