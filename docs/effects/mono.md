# Mono (left to all channels)

Copies the processed left channel to every other channel. Use it when you have a
mono microphone plugged into input 1 of a stereo interface, so only the left side
has signal.

## Try this

- Turn it on if one headphone side is silent, or if your voice only shows up on
  one channel. After this effect, both sides carry the same signal.
- Put Mono after [Gain](gain.md) and before
  [Saturation/Overdrive/Distortion](saturation.md) or [Reverb](reverb.md), so a
  mono source still feeds a wide reverb tail.
- There are no sliders here. If the copied signal is too quiet, raise Gain and
  check the bottom meters: peaks around **−5 dBFS**, RMS around **−20 dBFS**, and
  no **CLIP**.
- If your stream is already mono, this effect does nothing. Up to 16 instances
  are supported.

## Controls

This effect has no adjustable parameters. It is a single enable flag.

## Under the hood

`MonoStage` (`src/dsp/stages.cpp`):

- `Update` stores the enable flag.
- `Process` reads `values[0]` (the left channel) for each frame and writes it to
  channels 1 through `channels - 1`. It does nothing when disabled or when the
  stream is already mono.
- No buffers, coefficients, or state are allocated, and there is no latency.
- The parameter panel shows a short explanatory label because there are no
  sliders. The setting is stored per device with the other effect settings.
