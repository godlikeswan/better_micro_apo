# Gain

Turns the microphone input up or down. If your mic is too quiet, this is the
first effect to reach for. It appears first in the Add effect list and is
enabled with a default of **+12 dB**.

## Getting a good level

- Put Gain near the top of the effect list while you set things up. Starting
  with a usable level means you can hear the detail in the microphone and judge
  what the other effects are doing. You can move it later if you like.
- Put on headphones, check **Listen to this device**, and talk at the volume
  you normally use.
- Move **Gain (dB)** until the voice sounds comfortable. Watch the meters at the
  bottom: for speech, peaks around **−5 dBFS** and RMS around **−20 dBFS** are a
  good place to land. There should be no **CLIP** — if it shows up, slide the
  gain down.
- If turning the mic up also makes room noise obvious, fix that with
  [Noise reduction](noise-reduction.md) or the unified
  [Gate/Compressor/Limiter/Expander](dynamics.md) effect instead of lowering the
  voice level again.
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
  the chain encodes the buffer back to the stream format.
- Gain has no graph; the meters under **Listen to this device** are the way to
  judge it.
