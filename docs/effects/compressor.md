# Compressor

A linked peak compressor that tames peaks before makeup gain lifts the signal.
It is the core stage behind the "raise level without distortion" goal.

Typical placement: after noise reduction/gate/dynamics and before Gain. Up to
16 instances are supported.

## Parameters

| Parameter | Default | Clamp | Unit / meaning |
| --- | --- | --- | --- |
| Threshold | −58 dBFS | −60 to 0 dBFS | Level above which reduction starts. |
| Ratio | 3:1 | 1:1 to 20:1 | Amount of reduction above the threshold. |
| Attack | 5 ms | 0.1–1000 ms | Time constant when more reduction is needed. |
| Release | 100 ms | 1–5000 ms | Time constant when less reduction is needed. |

Newly added instances are enabled by `MakeEffect`.

## Implementation details

`CompressorStage` (`src/dsp/stages.cpp`):

- The detector is the peak of all channels in a frame (linked), so stereo image
  is preserved.
- For a frame above the threshold, the target gain in dB is
  `(threshold − level) * (1 − 1/ratio)`, clamped to be non-positive.
- An envelope in dB moves toward the target with the one-pole attack/release
  coefficients from `TimeCoefficient`; attack is used when more reduction is
  needed, release when less is needed.
- The envelope is converted to linear gain and applied to every channel.
- Coefficients are recomputed in `Configure`/`Update`; the only state is the
  envelope, cleared in `Reset`. No buffers are allocated and `Process` is
  allocation-free.

## GUI notes

The parameter panel shows a live compressor transfer curve drawn from the same
inline function in `src/dsp/transfer.h` used by the stage, so sliders and audio
cannot disagree. Compressor is normally followed by [Gain](gain.md) for makeup.
