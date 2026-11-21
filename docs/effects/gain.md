# Gain

Turns the microphone input up or down after the fixed [Input](input.md) row. If
your mic is too quiet, this is the first effect to reach for. It appears first in
the Add effect menu and is enabled with a default of **+12 dB**.

## Getting a good level

- Put Gain near the top of the addable effects while you set things up. Starting
  with a usable level means you can hear the detail in the microphone and judge
  what the other effects are doing. You can move it later if you like.
- Put on headphones, check **Listen to this device**, and talk at the volume you
  normally use.
- Move **Gain (dB)** until the voice sounds comfortable. Watch the meters: for
  speech, peaks around **−5 dBFS** and RMS around **−20 dBFS** are a good place
  to land. There should be no **CLIP** — if it shows up, slide the gain down.
- If turning the mic up also makes room noise obvious, fix that with
  [Noise reduction](noise-reduction.md), [Gate/Expander](gate-expander.md), or
  [Compressor/Limiter](compressor-limiter.md) instead of lowering the voice level
  again.
- A newly added Gain starts at **+12 dB**, so it raises the level immediately.
  The little ↺ button next to it goes back to +12 dB.

## Controls

| Control | Default | Range | What it does |
| --- | --- | --- | --- |
| Gain (dB) | +12 dB | −24 to +50 dB | Turns the whole signal up or down. |

## Under the hood

`GainStage` (`src/dsp/stages.cpp`):

- `ApplyGain` converts `gainDb` to a linear scalar when the effect is enabled
  and the value is nonzero, otherwise the scalar is 1.0.
- `Update` recomputes the scalar from the effect instance; it has no
  format-dependent state and allocates nothing.
- `Process` multiplies every interleaved sample by the scalar when it is not
  1.0. No state, no latency, and no clipping here — final clipping happens when
  the chain encodes the buffer back to the stream format. The same stage also
  copies channel 0 to the other channels for the fixed Input row when
  **Treat as mono** is checked; standalone Gain forces that flag off.
- Gain has an input/output graph: the diagonal curve is the gain setting, the
  dot is the cross-section of the live input and output **peaks** from the APO's
  stage telemetry, and a highlighted cross marks the 0 dB point on both axes.
  Both values use the same short cross-buffer hold as the meter fill, so a brief
  sound still moves the dot between 100 ms GUI polls, and a meter reading above
  0 dB has a matching point above 0 on the graph. A thin meter to the right
  shows the Gain stage's output peak with green/yellow/red zones,
  dB labels, a short-held fill, and a thicker long-hold peak line coloured to
  match the bar zone it sits in. The caption reads **Transfer curve (peak, dB)**;
  the dB values are dBFS (decibels relative to full scale), where 0 is digital
  full scale, negative values are below it, and positive values mean the
  floating-point signal exceeds full scale before final conversion. The graph
  range is **−96 to +24 dBFS**, so a large boost can put the curve or dot above
  the visible area.
- The graph and its meter come from `.monitor` polling, not from the bottom
  Listen meter, so they do not change settings or stop audio.
