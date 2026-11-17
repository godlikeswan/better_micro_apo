# Gain

Flat makeup gain. It lifts the signal after the compressor so quiet microphones
can be used at a useful level.

Typical placement: after the compressor and before mono/distortion/reverb. Up
to 16 instances are supported.

## Parameters

| Parameter | Default | Clamp | Unit / meaning |
| --- | --- | --- | --- |
| Gain | 0 dB | −24 to +48 dB | Linear gain applied to every sample. |

A newly added Gain starts at **0 dB**, so adding it is safe. The old fixed chain
used +28 dB; that value is not applied to new instances.

## Implementation details

`GainStage` (`src/dsp/stages.cpp`):

- `ApplyGain` converts `gainDb` to a linear scalar when the effect is enabled
  and the value is nonzero, otherwise the scalar is 1.0.
- `Update` recomputes the scalar from the effect instance; it has no
  format-dependent state and allocates nothing.
- `Process` multiplies every interleaved sample by the scalar when it is not
  1.0. No state, no latency, no clipping here — final clipping happens when
  [Chain](../apo/operation.md) encodes the buffer back to the stream format.

## GUI notes

The single Gain slider carries the ↺ per-parameter reset icon; its default is
0 dB. Because the GUI clamps to −24…+48 dB, large boosts are possible, so pair
it with the peak/RMS meter under **Listen to this device** and watch for CLIP.
