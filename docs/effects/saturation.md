# Saturation/Overdrive/Distortion

Adds a gritty, overdriven edge. Use it for guitar, a deliberately rough vocal,
or a special effect — not as a general fix for a quiet microphone. Gentle
settings add warmth and bite; higher Drive and Curvature get buzzy.

## Try this

- Put Saturation near the end of the list, usually after
  [Mono](mono.md) and before [Reverb](reverb.md), so the reverb tail stays
  clean.
- Start with **Drive** at 0 dB and raise it until the signal is pushed into the
  curve. A little adds warmth and bite; a lot gets buzzy.
- **Curvature** controls how hard the curve bends. Around 0% it is almost
  linear; around 100% it is fully saturated. Move it slowly — the graph in the
  panel updates as you go.
- While audio runs, the panel places mirrored dots and dotted vertical stripes
  on the transfer curve from the stage's input peak after the Drive pre-gain.
  Drive widens that gap toward the flat part of the curve; it does not bend the
  curve itself. This is an operating-point cue, not a waveform display; the thin
  meter to the right shows the stage's output peak with a short-held fill and a
  thicker long-hold peak line whose colour matches the bar zone it sits in.
- **Function** changes the character. **Tanh** is the smooth default; try the
  others if you want a different color.
- **Output** sets the level after the effect. Saturation can sound much louder
  than the clean signal even without clipping, so turn Output down if it jumps
  out at you. If the bottom meter shows **CLIP**, lower Output first.
- A newly added Saturation starts with Tanh, 0 dB Drive, 50% Curvature, and 0 dB
  Output. The small ↺ buttons reset individual controls. Up to 16 instances are
  supported.

## Controls

| Control | Default | Range | What it does |
| --- | --- | --- | --- |
| Function | Tanh | Tanh, Arctan, Cubic polynomial, Logarithmic, Exponential, Error function | Curve family. |
| Drive | 0 dB | 0 to 36 dB | Level into the curve. |
| Curvature | 50% | 0 to 100% | How hard the curve bends, from nearly linear to fully saturated. |
| Output | 0 dB | −24 to +12 dB | Level after the curve. |

## Under the hood

`DistortionStage` (`src/dsp/stages.cpp`) keeps the historical C++ and INI key
names, while the GUI, effect list, and documentation use
"Saturation/Overdrive/Distortion":

- `Update` folds the function and curvature into one coefficient pair with
  `MakeDistortionCurve` and converts Drive/Output to linear scalars. All curve
  setup happens off the real-time path.
- `Process` evaluates `Shape(sample * drive) * output` per interleaved sample.
  No state, no buffers, no latency.
- All six curves are odd and map −1…1 to −1…1, so no DC blocker is needed.
- The curve math lives in `src/dsp/transfer.h`; the GUI graph calls the same
  inline functions, so the plotted curve matches what the DSP applies. Drive is
  a pre-gain, so the graph draws the shaping curve at unity input and feeds the
  `.monitor` input peak through that gain to place the mirrored markers. Baking
  Drive into the curve made every setting look like a different curve shape;
  treating it as pre-gain keeps the shape fixed and only moves the operating
  point.
