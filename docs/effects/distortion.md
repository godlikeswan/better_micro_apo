# Distortion

Soft saturation with a selectable odd transfer curve. It adds warmth or edge
without generating DC offset: every curve maps full scale to full scale.

Typical placement: after mono and before reverb, so the reverb tail stays
clean. Up to 16 instances are supported.

## Parameters

| Parameter | Default | Clamp | Unit / meaning |
| --- | --- | --- | --- |
| Function | Tanh | Tanh, Arctan, Cubic polynomial, Logarithmic, Exponential, Error function | Curve family. |
| Drive | 0 dB | 0–36 dB | Level into the curve. |
| Curvature | 50 % | 0–100 % | How hard the curve bends, from nearly linear to fully saturated. |
| Output | 0 dB | −24 to +12 dB | Level after the curve. |

Newly added instances are enabled by `MakeEffect`; the default function is Tanh.

## Implementation details

`DistortionStage` (`src/dsp/stages.cpp`):

- `Update` folds the function and curvature into one coefficient pair with
  `MakeDistortionCurve` and converts Drive/Output to linear scalars. All curve
  setup happens off the real-time path.
- `Process` evaluates `Shape(sample * drive) * output` per interleaved sample.
  No state, no buffers, no latency.
- All six curves are odd and map −1…1 to −1…1, so no DC blocker is needed.
- The curve math lives in `src/dsp/transfer.h`; the GUI graph calls the same
  inline functions, so the plotted curve matches what the DSP applies.

## GUI notes

The Function combo lists the six curves. The parameter panel shows a live
transfer curve from `src/dsp/transfer.h`; moving Curvature or Drive changes it
immediately. The explanatory note about drive/curvature was removed from the UI
to keep the panel compact — the graph now conveys the shape.
